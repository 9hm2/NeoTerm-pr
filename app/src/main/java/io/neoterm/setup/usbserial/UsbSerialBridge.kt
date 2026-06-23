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
import io.neoterm.setup.proot.Kmsg
import io.neoterm.utils.NLog

/**
 * USB-serial bridge with hotplug: a fixed pool of PTYs is created once and bound
 * by proot onto /dev/ttyUSB0..N at launch, so the ports always exist. When an
 * adapter is attached it is assigned to a free pool slot and its chip (driven by
 * usb-serial-for-android) is pumped to that slot's PTY; on detach the slot is
 * freed but the PTY stays bound, so a later plug reuses it live — no new tab.
 *
 * Everything notable is logged to the kmsg/dmesg buffer ([Kmsg]) for debugging.
 * Data + termios (baud/data/stop/parity/flow) flow via the PTY; modem lines
 * (DTR/RTS/CTS/DSR/DCD/RI) + BREAK are handled by the proot ioctl proxy (later).
 */
object UsbSerialBridge {

  private const val POOL = 4
  private const val TAG = "UsbSerial"

  private class Slot(
    val pfd: ParcelFileDescriptor,   // owns the persistent PTY master fd
    val slavePath: String,
    val ttyName: String,
  ) {
    @Volatile var running = false
    var deviceName: String? = null
    var port: UsbSerialPort? = null
    var conn: UsbDeviceConnection? = null
    val threads = ArrayList<Thread>()
    var lastParams: IntArray? = null
    var dtr = false       // cached modem-control outputs (PTY can't carry them)
    var rts = false
    val masterJfd get() = pfd.fileDescriptor
    val masterFd: Int get() = pfd.fd
  }

  private var pool: Array<Slot>? = null
  private var controlServer: LocalServerSocket? = null

  // ── modem-line control server (io.neoterm.ttyusb) ───────────────────────
  // The proot ioctl proxy forwards the guest's TIOCMGET/TIOCMSET/TIOCMBIS/
  // TIOCMBIC/TCSBRK on a /dev/ttyUSB* fd here (resolved to its PTY slave path),
  // so DTR/RTS/CTS/DSR/DCD/RI/BREAK reach the real chip. Line protocol:
  //   GET <pts>            -> "<modembits>" | "NAK"
  //   SET|BIS|BIC <pts> <bits> -> "OK" | "NAK"
  //   BRK <pts> <0|1|p>    -> "OK" | "NAK"
  // modem bits = Linux TIOCM_*: DTR 0x002 RTS 0x004 CTS 0x020 CAR 0x040 RNG 0x080 DSR 0x100
  private fun startControlServer() {
    if (controlServer != null) return
    val srv = try { LocalServerSocket("io.neoterm.ttyusb") } catch (e: Exception) {
      Kmsg.log("usb-serial: modem control bind failed: ${e.message}"); return
    }
    controlServer = srv
    Thread({
      Kmsg.log("usb-serial: modem control server ready (io.neoterm.ttyusb)")
      while (true) {
        val c = try { srv.accept() } catch (e: Exception) { break }
        runCatching { handleControl(c) }
        runCatching { c.close() }
      }
    }, "ttyusb-control").apply { isDaemon = true; start() }
  }

  private fun handleControl(c: LocalSocket) {
    val line = c.inputStream.bufferedReader().readLine()?.trim() ?: return
    val out = c.outputStream
    fun reply(s: String) { out.write((s + "\n").toByteArray()); out.flush() }
    val parts = line.split(" ")
    if (parts.size < 2) { reply("NAK"); return }
    val op = parts[0]
    val pts = parts[1]
    val slot = synchronized(this) { pool?.firstOrNull { it.slavePath == pts && it.port != null } }
    val port = slot?.port
    if (slot == null || port == null) { reply("NAK"); return }
    val bits = parts.getOrNull(2)?.toIntOrNull() ?: 0
    when (op) {
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
    Kmsg.log("usb-serial: ${slot.ttyName} DTR=${if (slot.dtr) 1 else 0} RTS=${if (slot.rts) 1 else 0}")
  }

  // ── pool lifecycle ──────────────────────────────────────────────────────
  @Synchronized
  private fun ensurePool(): Array<Slot>? {
    pool?.let { return it }
    if (!NeoPreference.isUsbSerialEnabled()) return null
    val slots = ArrayList<Slot>(POOL)
    for (i in 0 until POOL) {
      val out = IntArray(1)
      val slave = Pty.open(out) ?: break
      slots.add(Slot(ParcelFileDescriptor.adoptFd(out[0]), slave, "ttyUSB$i"))
    }
    if (slots.isEmpty()) {
      Kmsg.log("usb-serial: PTY pool creation failed (no /dev/ttyUSB*)")
      return null
    }
    val arr = slots.toTypedArray()
    pool = arr
    startControlServer()
    Kmsg.log("usb-serial: ready — pool of ${arr.size} ports /dev/ttyUSB0..${arr.size - 1} (hotplug)")
    return arr
  }

  /** (slavePath -> "/dev/ttyUSBn") pairs for ProotManager to bind at launch. */
  @Synchronized
  fun bindings(): List<Pair<String, String>> {
    val p = ensurePool() ?: return emptyList()
    return p.map { it.slavePath to "/dev/${it.ttyName}" }
  }

  // ── device recognition ──────────────────────────────────────────────────
  /** True if this device is a USB-serial chip we can drive (cheap, no open). */
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
    if (!NeoPreference.isUsbSerialEnabled()) return
    val id = "%04x:%04x".format(device.vendorId, device.productId)
    val p = ensurePool() ?: return
    if (p.any { it.deviceName == device.deviceName }) return  // already bridged
    val driver = driverFor(device) ?: return
    if (!usb.hasPermission(device)) {
      Kmsg.log("usb-serial: $id detected (${driver.javaClass.simpleName}) — waiting for USB permission")
      return
    }
    val slot = p.firstOrNull { it.deviceName == null }
    if (slot == null) {
      Kmsg.log("usb-serial: $id detected but pool full ($POOL ports in use)")
      return
    }
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
    slot.deviceName = device.deviceName
    slot.port = port
    slot.conn = conn
    slot.lastParams = null
    slot.running = true
    startPump(slot)
    val product = runCatching { device.productName }.getOrNull()?.takeIf { it.isNotBlank() } ?: ""
    Kmsg.log("usb-serial: ${slot.ttyName} <- $id ${product.ifEmpty { driver.javaClass.simpleName }} (${driver.javaClass.simpleName}) @9600 8N1")
    NLog.e(TAG, "attached ${slot.ttyName} for ${device.deviceName} ($id)")
  }

  @Synchronized
  fun onDetached(device: UsbDevice?) {
    val name = device?.deviceName ?: return
    val slot = pool?.firstOrNull { it.deviceName == name } ?: return
    releaseSlot(slot, "${slot.ttyName}: adapter disconnected")
  }

  @Synchronized
  fun stopAll() {
    pool?.forEach { slot ->
      if (slot.deviceName != null) releaseSlot(slot, "${slot.ttyName}: released (service stop)")
      runCatching { slot.pfd.close() }
    }
    pool = null
    runCatching { controlServer?.close() }
    controlServer = null
  }

  /** Stop pumping + close the chip, but keep the pool PTY bound for the next plug. */
  private fun releaseSlot(slot: Slot, msg: String) {
    slot.running = false
    slot.threads.forEach { it.interrupt() }
    slot.threads.clear()
    runCatching { slot.port?.close() }
    runCatching { slot.conn?.close() }
    slot.port = null
    slot.conn = null
    slot.deviceName = null
    slot.lastParams = null
    Kmsg.log("usb-serial: $msg")
  }

  // ── pump ────────────────────────────────────────────────────────────────
  private fun startPump(slot: Slot) {
    // chip -> pty (port.read has a 200ms timeout, so it exits promptly on stop).
    val rx = Thread({
      val buf = ByteArray(4096)
      while (slot.running) {
        val n = try {
          slot.port?.read(buf, 200) ?: break
        } catch (e: Exception) {
          if (slot.running) Kmsg.log("usb-serial: ${slot.ttyName} read error: ${e.message}")
          break
        }
        if (n > 0) writeFully(slot, buf, n)
      }
    }, "ttyusb-rx-${slot.ttyName}").apply { isDaemon = true }

    // pty -> chip, polled so it never blocks forever on the master (clean release).
    val tx = Thread({
      val pfd = StructPollfd().apply {
        fd = slot.masterJfd
        events = OsConstants.POLLIN.toShort()
      }
      val buf = ByteArray(4096)
      while (slot.running) {
        applyParamsIfChanged(slot)
        pfd.revents = 0
        val ready = try { Os.poll(arrayOf(pfd), 200) } catch (e: Exception) { break }
        if (!slot.running) break
        if (ready > 0 && (pfd.revents.toInt() and OsConstants.POLLIN) != 0) {
          val n = try { Os.read(slot.masterJfd, buf, 0, buf.size) } catch (e: Exception) { break }
          if (n <= 0) break
          runCatching { slot.port?.write(buf.copyOf(n), 1000) }
        }
      }
    }, "ttyusb-tx-${slot.ttyName}").apply { isDaemon = true }

    slot.threads.add(rx); slot.threads.add(tx)
    rx.start(); tx.start()
  }

  private fun writeFully(slot: Slot, buf: ByteArray, len: Int) {
    var off = 0
    while (slot.running && off < len) {
      val w = try { Os.write(slot.masterJfd, buf, off, len - off) } catch (e: Exception) { return }
      if (w <= 0) return
      off += w
    }
  }

  /** Read the guest's PTY termios and program the chip when it changes. */
  private fun applyParamsIfChanged(slot: Slot) {
    val port = slot.port ?: return
    val p = runCatching { Pty.serialParams(slot.masterFd) }.getOrNull() ?: return
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
}
