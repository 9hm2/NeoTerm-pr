package io.neoterm.setup.usbserial

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.ParcelFileDescriptor
import android.system.Os
import android.system.OsConstants
import android.system.StructPollfd
import com.hoho.android.usbserial.driver.CdcAcmSerialDriver
import com.hoho.android.usbserial.driver.Ch34xSerialDriver
import com.hoho.android.usbserial.driver.Cp21xxSerialDriver
import com.hoho.android.usbserial.driver.FtdiSerialDriver
import com.hoho.android.usbserial.driver.ProlificSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialDriver
import com.hoho.android.usbserial.driver.UsbSerialPort
import io.neoterm.backend.Pty
import io.neoterm.component.config.NeoPreference
import io.neoterm.component.config.NeoTermPath
import io.neoterm.setup.proot.Kmsg
import io.neoterm.utils.NLog
import java.io.File

/**
 * USB-serial bridge with proper hotplug. /dev/ttyUSB<n> is a VIRTUAL port: the
 * proot open-redirect (syscall/enter.c) asks this server "PATH ttyUSBn" and
 * rewrites the open to the current live PTY slave. So a port exists only while a
 * device is attached, disconnects cleanly on unplug (the PTY master is closed →
 * the guest's read returns EOF), and the SAME ttyUSB number is reused on replug.
 *
 * The chip is driven app-side by usb-serial-for-android; data flows PTY-native;
 * the guest's termios (baud/data/stop/parity) is read off the PTY master and
 * programmed onto the chip; modem lines (DTR/RTS/CTS/DSR/DCD/RI) + BREAK come
 * through the proot ioctl proxy to this same server. Gated by
 * [NeoPreference.isUsbSerialEnabled]; the device table is kernel-derived
 * ([UsbSerialIds]); everything notable is logged to dmesg ([Kmsg]).
 */
object UsbSerialBridge {

  private const val POOL = 4
  private const val TAG = "UsbSerial"

  // Bound onto the guest's /sys/class/tty (Android SELinux blocks readdir of the
  // real one), so ls / pyserial can enumerate the ports. Populated per attach.
  private val sysClassTty = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-class-tty")

  /** Host dir to bind onto /sys/class/tty, or null if usb-serial is disabled. */
  fun sysfsBind(): String? {
    if (!NeoPreference.isUsbSerialEnabled()) return null
    if (!sysClassTty.isDirectory && !sysClassTty.mkdirs()) return null
    return sysClassTty.absolutePath
  }

  private class Slot(val ttyName: String, val index: Int) {
    @Volatile var running = false
    var deviceName: String? = null
    var port: UsbSerialPort? = null
    var conn: UsbDeviceConnection? = null
    var pfd: ParcelFileDescriptor? = null   // PTY master, created on attach
    var slavePath: String? = null           // /dev/pts/N the redirect points to
    val threads = ArrayList<Thread>()
    var lastParams: IntArray? = null
    var dtr = false
    var rts = false
    var lastModemLogMs = 0L
  }

  private val slots = Array(POOL) { Slot("ttyUSB$it", it) }
  private var controlServer: LocalServerSocket? = null
  @Volatile private var started = false

  // ── server lifecycle ────────────────────────────────────────────────────
  /** Start the app-side control server (open-redirect + LIST + modem). Called at
   *  proot launch and on attach; idempotent and gated by the toggle. */
  @Synchronized
  fun ensureReady() {
    if (started) return
    if (!NeoPreference.isUsbSerialEnabled()) return
    val srv = try { LocalServerSocket("io.neoterm.ttyusb") } catch (e: Exception) {
      Kmsg.log("usb-serial: control socket bind failed: ${e.message}"); return
    }
    controlServer = srv
    started = true
    Thread({
      Kmsg.log("usb-serial: ready — virtual hotplug ports /dev/ttyUSB0..${POOL - 1}")
      while (true) {
        val c = try { srv.accept() } catch (e: Exception) { break }
        // One handler thread per connection: the proot modem proxy holds a single
        // persistent connection for the lifetime of an open port, so serving it on
        // the accept thread would block LIST/PATH queries (getdents injection).
        Thread({
          runCatching { serveControl(c) }
          runCatching { c.close() }
        }, "ttyusb-conn").apply { isDaemon = true; start() }
      }
    }, "ttyusb-control").apply { isDaemon = true; start() }
  }

  // ── device recognition ──────────────────────────────────────────────────
  fun isSerial(device: UsbDevice): Boolean = driverTag(device) != null

  private fun driverTag(device: UsbDevice): String? {
    UsbSerialIds.driverFor(device.vendorId, device.productId)?.let { return it }
    for (i in 0 until device.interfaceCount) {
      val c = device.getInterface(i).interfaceClass
      if (c == UsbConstants.USB_CLASS_COMM || c == UsbConstants.USB_CLASS_CDC_DATA) return "CdcAcmSerialDriver"
    }
    return null
  }

  private fun driverFor(device: UsbDevice): UsbSerialDriver? = when (driverTag(device)) {
    "FtdiSerialDriver" -> FtdiSerialDriver(device)
    "Cp21xxSerialDriver" -> Cp21xxSerialDriver(device)
    "Ch34xSerialDriver" -> Ch34xSerialDriver(device)
    "ProlificSerialDriver" -> ProlificSerialDriver(device)
    "CdcAcmSerialDriver" -> CdcAcmSerialDriver(device)
    else -> null
  }

  // ── attach / detach ─────────────────────────────────────────────────────
  @Synchronized
  fun attach(usb: UsbManager, device: UsbDevice) {
    ensureReady()
    if (!NeoPreference.isUsbSerialEnabled()) return
    val id = "%04x:%04x".format(device.vendorId, device.productId)
    if (slots.any { it.deviceName == device.deviceName }) return  // already bridged
    val driver = driverFor(device) ?: return
    if (!usb.hasPermission(device)) {
      Kmsg.log("usb-serial: $id detected (${driver.javaClass.simpleName}) — waiting for USB permission")
      return
    }
    val slot = slots.firstOrNull { it.deviceName == null }
    if (slot == null) { Kmsg.log("usb-serial: $id detected but all $POOL ports busy"); return }
    val conn = runCatching { usb.openDevice(device) }.getOrNull()
    if (conn == null) { Kmsg.log("usb-serial: $id openDevice() failed"); return }
    val port = driver.ports.firstOrNull()
    if (port == null) { runCatching { conn.close() }; Kmsg.log("usb-serial: $id has no serial ports"); return }
    try {
      port.open(conn)
      port.setParameters(9600, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
    } catch (e: Exception) {
      Kmsg.log("usb-serial: $id open failed: ${e.message}")
      runCatching { port.close() }; runCatching { conn.close() }; return
    }
    val out = IntArray(1)
    val slave = Pty.open(out)
    if (slave == null) {
      Kmsg.log("usb-serial: $id openPty failed")
      runCatching { port.close() }; runCatching { conn.close() }; return
    }
    slot.pfd = ParcelFileDescriptor.adoptFd(out[0])
    slot.slavePath = slave
    slot.deviceName = device.deviceName
    slot.port = port
    slot.conn = conn
    slot.dtr = false; slot.rts = false; slot.lastParams = null; slot.running = true
    startPump(slot, slot.pfd!!)
    writeSysfs(slot, device)
    val product = runCatching { device.productName }.getOrNull()?.takeIf { it.isNotBlank() } ?: ""
    Kmsg.log("usb-serial: ${slot.ttyName} <- $id ${product.ifEmpty { driver.javaClass.simpleName }} (${driver.javaClass.simpleName}) @9600 8N1")
    NLog.e(TAG, "attached ${slot.ttyName} ($id)")
  }

  @Synchronized
  fun onDetached(device: UsbDevice?) {
    val name = device?.deviceName ?: return
    val slot = slots.firstOrNull { it.deviceName == name } ?: return
    teardown(slot, "${slot.ttyName}: adapter disconnected")
  }

  @Synchronized
  fun stopAll() {
    slots.forEach { if (it.deviceName != null) teardown(it, "${it.ttyName}: released (service stop)") }
    runCatching { controlServer?.close() }
    controlServer = null
    started = false
  }

  /** Close the chip and the PTY master so a program holding /dev/ttyUSB* gets EOF
   *  and exits cleanly (clean unplug). The port number is freed for reuse. */
  private fun teardown(slot: Slot, msg: String) {
    slot.running = false
    slot.threads.forEach { it.interrupt() }
    slot.threads.clear()
    runCatching { slot.port?.close() }
    runCatching { slot.conn?.close() }
    runCatching { slot.pfd?.close() }   // -> guest read returns EOF
    deleteRec(File(sysClassTty, slot.ttyName))
    slot.port = null
    slot.conn = null
    slot.pfd = null
    slot.slavePath = null
    slot.deviceName = null
    slot.lastParams = null
    slot.dtr = false
    slot.rts = false
    Kmsg.log("usb-serial: $msg")
  }

  // ── control server: open-redirect + enumeration + modem ─────────────────
  // PATH <ttyUSBn>          -> "<pts path>" | "NAK"   (proot open-redirect)
  // LIST                    -> "ttyUSB0 ttyUSB2 ..." | "-"
  // GET <pts>               -> "<modembits>" | "NAK"
  // SET|BIS|BIC <pts> <bits>-> "OK" | "NAK"
  // BRK <pts> <0|1|p>       -> "OK" | "NAK"
  // modem bits = Linux TIOCM_*: DTR 0x002 RTS 0x004 CTS 0x020 CAR 0x040 RNG 0x080 DSR 0x100
  private fun serveControl(c: LocalSocket) {
    val reader = c.inputStream.bufferedReader()
    val out = c.outputStream
    fun reply(s: String) { out.write((s + "\n").toByteArray()); out.flush() }
    // Loop over newline-delimited requests so a persistent client (the proot modem
    // proxy) can issue a burst of SET/BIS/BIC/GET without reconnecting. Returns when
    // the peer closes (readLine == null) — i.e. on proot exit or a dropped socket.
    while (true) {
      val line = (reader.readLine() ?: return).trim()
      if (line.isEmpty()) continue
      val parts = line.split(" ")
      when (parts.getOrNull(0)) {
        "PATH" -> {
          val tty = parts.getOrNull(1)
          val pts = synchronized(this) {
            slots.firstOrNull { it.ttyName == tty && it.slavePath != null && it.port != null }?.slavePath
          }
          reply(pts ?: "NAK")
        }
        "LIST" -> {
          val active = synchronized(this) { slots.filter { it.slavePath != null }.map { it.ttyName } }
          reply(if (active.isEmpty()) "-" else active.joinToString(" "))
        }
        "GET", "SET", "BIS", "BIC", "BRK" -> handleModem(parts, ::reply)
        else -> reply("NAK")
      }
    }
  }

  private fun handleModem(parts: List<String>, reply: (String) -> Unit) {
    val pts = parts.getOrNull(1)
    val slot = synchronized(this) { slots.firstOrNull { it.slavePath == pts && it.port != null } }
    val port = slot?.port
    if (slot == null || port == null) { reply("NAK"); return }
    val bits = parts.getOrNull(2)?.toIntOrNull() ?: 0
    when (parts[0]) {
      "GET" -> {
        var m = 0
        if (slot.dtr) m = m or 0x002
        if (slot.rts) m = m or 0x004
        runCatching { if (port.getCTS()) m = m or 0x020 }
        runCatching { if (port.getCD()) m = m or 0x040 }
        runCatching { if (port.getRI()) m = m or 0x080 }
        runCatching { if (port.getDSR()) m = m or 0x100 }
        reply(m.toString())
      }
      "SET" -> { setLines(slot, (bits and 0x002) != 0, (bits and 0x004) != 0); reply("OK") }
      "BIS" -> { setLines(slot, slot.dtr || (bits and 0x002) != 0, slot.rts || (bits and 0x004) != 0); reply("OK") }
      "BIC" -> { setLines(slot, slot.dtr && (bits and 0x002) == 0, slot.rts && (bits and 0x004) == 0); reply("OK") }
      "BRK" -> {
        val v = parts.getOrNull(2) ?: "0"
        runCatching {
          if (v == "p") { port.setBreak(true); Thread.sleep(250); port.setBreak(false) }
          else port.setBreak(v == "1")
        }
        Kmsg.log("usb-serial: ${slot.ttyName} BREAK ${if (v == "1") "on" else if (v == "p") "pulse" else "off"}")
        reply("OK")
      }
      else -> reply("NAK")
    }
  }

  private fun setLines(slot: Slot, dtr: Boolean, rts: Boolean) {
    val port = slot.port ?: return
    if (dtr != slot.dtr) { runCatching { port.setDTR(dtr) }; slot.dtr = dtr }
    if (rts != slot.rts) { runCatching { port.setRTS(rts) }; slot.rts = rts }
    // Throttle: reset sequences (esptool/avrdude) toggle DTR/RTS dozens of times.
    val now = System.currentTimeMillis()
    if (now - slot.lastModemLogMs >= 300) {
      slot.lastModemLogMs = now
      Kmsg.log("usb-serial: ${slot.ttyName} DTR=${if (slot.dtr) 1 else 0} RTS=${if (slot.rts) 1 else 0}")
    }
  }

  // ── pump ────────────────────────────────────────────────────────────────
  private fun startPump(slot: Slot, pfd: ParcelFileDescriptor) {
    val masterJfd = pfd.fileDescriptor
    val masterFd = pfd.fd
    // chip -> pty (port.read has a 200ms timeout; also polls the guest's termios).
    val rx = Thread({
      val buf = ByteArray(4096)
      while (slot.running) {
        applyParamsIfChanged(slot, masterFd)
        val n = try {
          slot.port?.read(buf, 200) ?: break
        } catch (e: Exception) {
          if (slot.running) Kmsg.log("usb-serial: ${slot.ttyName} read error: ${e.message}"); break
        }
        if (n > 0) writeFully(slot, masterJfd, buf, n)
      }
    }, "ttyusb-rx-${slot.ttyName}").apply { isDaemon = true }

    // pty -> chip, polled so detach (master close) releases the thread promptly.
    val tx = Thread({
      val pollfd = StructPollfd().apply { fd = masterJfd; events = OsConstants.POLLIN.toShort() }
      val buf = ByteArray(4096)
      while (slot.running) {
        pollfd.revents = 0
        val ready = try { Os.poll(arrayOf(pollfd), 200) } catch (e: Exception) { break }
        if (!slot.running) break
        if (ready > 0 && (pollfd.revents.toInt() and OsConstants.POLLIN) != 0) {
          val n = try { Os.read(masterJfd, buf, 0, buf.size) } catch (e: Exception) { break }
          if (n <= 0) break
          runCatching { slot.port?.write(buf.copyOf(n), 1000) }
        }
      }
    }, "ttyusb-tx-${slot.ttyName}").apply { isDaemon = true }

    slot.threads.add(rx); slot.threads.add(tx)
    rx.start(); tx.start()
  }

  private fun writeFully(slot: Slot, masterJfd: java.io.FileDescriptor, buf: ByteArray, len: Int) {
    var off = 0
    while (slot.running && off < len) {
      val w = try { Os.write(masterJfd, buf, off, len - off) } catch (e: Exception) { return }
      if (w <= 0) return
      off += w
    }
  }

  private fun applyParamsIfChanged(slot: Slot, masterFd: Int) {
    val port = slot.port ?: return
    val p = runCatching { Pty.serialParams(masterFd) }.getOrNull() ?: return
    if (p.contentEquals(slot.lastParams)) return
    slot.lastParams = p
    val parity = when (p[3]) {
      1 -> UsbSerialPort.PARITY_ODD
      2 -> UsbSerialPort.PARITY_EVEN
      3 -> UsbSerialPort.PARITY_MARK
      4 -> UsbSerialPort.PARITY_SPACE
      else -> UsbSerialPort.PARITY_NONE
    }
    val ok = runCatching { port.setParameters(p[0], p[1], p[2], parity) }.isSuccess
    val pc = "NOEMS"[p[3].coerceIn(0, 4)]
    val flow = when (p[4]) { 1 -> "rts/cts"; 2 -> "xon/xoff"; else -> "none" }
    Kmsg.log("usb-serial: ${slot.ttyName} set ${p[0]} ${p[1]}$pc${p[2]} flow=$flow${if (ok) "" else " (FAILED)"}")
  }

  // ── fake sysfs (/sys/class/tty/ttyUSB*) for pyserial / esptool enumeration ──
  // This host dir is bound onto the guest's /sys/class/tty (see ProotManager) —
  // Android SELinux blocks readdir of the real one. Each attach drops a port tree.
  private fun writeSysfs(slot: Slot, device: UsbDevice) {
    runCatching {
      val dir = File(sysClassTty, slot.ttyName)
      deleteRec(dir)
      val dev = File(dir, "device").apply { mkdirs() }
      val vid = "%04x".format(device.vendorId)
      val pid = "%04x".format(device.productId)
      val product = runCatching { device.productName }.getOrNull()?.takeIf { it.isNotBlank() } ?: "USB Serial"
      val manuf = runCatching { device.manufacturerName }.getOrNull() ?: ""
      val serial = runCatching { device.serialNumber }.getOrNull() ?: ""
      val numIf = device.interfaceCount.coerceAtLeast(1)
      File(dir, "dev").writeText("188:${slot.index}\n")
      File(dir, "uevent").writeText("MAJOR=188\nMINOR=${slot.index}\nDEVNAME=${slot.ttyName}\n")
      // Write the USB-device attributes at both levels — pyserial computes
      // usb_device_path = dirname(.../device) = the ttyUSB0 dir (subsystem=usb),
      // and reads idVendor/idProduct/bNumInterfaces/... from there; older versions
      // read from .../device. bNumInterfaces is parsed with int() -> must exist.
      for (base in arrayOf(dir, dev)) {
        File(base, "idVendor").writeText("$vid\n")
        File(base, "idProduct").writeText("$pid\n")
        File(base, "product").writeText("$product\n")
        File(base, "manufacturer").writeText("$manuf\n")
        File(base, "serial").writeText("$serial\n")
        File(base, "bNumInterfaces").writeText(" $numIf\n")   // sysfs has a leading space
        File(base, "bcdDevice").writeText("0100\n")
      }
      File(dev, "interface").writeText("$product\n")
      File(dev, "bInterfaceNumber").writeText("00\n")
      File(dev, "uevent").writeText("DEVTYPE=usb_interface\nPRODUCT=$vid/$pid\n")
      // subsystem symlink whose basename is "usb" -> pyserial treats it as a USB port.
      runCatching { Os.symlink("/sys/bus/usb", File(dev, "subsystem").absolutePath) }
    }.onFailure { NLog.e(TAG, "sysfs gen: ${it.message}") }
  }

  private fun deleteRec(f: File) {
    if (f.isDirectory) f.listFiles()?.forEach { deleteRec(it) }
    runCatching { f.delete() }
  }
}
