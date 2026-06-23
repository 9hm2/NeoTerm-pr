package io.neoterm.setup.usbserial

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.os.ParcelFileDescriptor
import android.system.Os
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
 * USB-serial bridge: drives FTDI/CP210x/CH34x/PL2303/CDC-ACM adapters app-side
 * with usb-serial-for-android, and exposes each as a PTY whose slave path proot
 * binds onto /dev/ttyUSB*. Data flows PTY-native; the guest's termios (baud,
 * data/stop bits, parity) is read off the PTY master and programmed onto the
 * chip. Gated at runtime by [NeoPreference.isUsbSerialEnabled]; the device list
 * comes from the kernel-derived [UsbSerialIds] table (no manifest device_filter).
 *
 * Modem control lines (DTR/RTS/CTS/DSR/DCD/RI) and BREAK are not carried by a
 * PTY; those are handled by the proot control-ioctl proxy (separate piece).
 */
object UsbSerialBridge {

  private class Link(
    val port: UsbSerialPort,
    val conn: UsbDeviceConnection,
    val pfd: ParcelFileDescriptor,   // owns the PTY master fd
    val slavePath: String,
    val ttyName: String,
  ) {
    @Volatile var running = true
    val threads = ArrayList<Thread>()
    var lastParams: IntArray? = null
    val masterFd: Int get() = pfd.fd
    val masterJfd get() = pfd.fileDescriptor
  }

  private val links = LinkedHashMap<String, Link>()  // deviceName -> Link

  /** (slavePath -> "/dev/ttyUSBn") pairs for ProotManager to bind at launch. */
  @Synchronized
  fun bindings(): List<Pair<String, String>> =
    links.values.map { it.slavePath to "/dev/${it.ttyName}" }

  /** True if this device is a USB-serial chip we can drive (cheap, no open). */
  fun isSerial(device: UsbDevice): Boolean {
    if (UsbSerialIds.driverFor(device.vendorId, device.productId) != null) return true
    for (i in 0 until device.interfaceCount) {
      val c = device.getInterface(i).interfaceClass
      if (c == UsbConstants.USB_CLASS_COMM || c == UsbConstants.USB_CLASS_CDC_DATA) return true
    }
    return false
  }

  /** Open + bridge the device, if the toggle is on and we have a driver + permission. */
  @Synchronized
  fun attach(usb: UsbManager, device: UsbDevice) {
    if (!NeoPreference.isUsbSerialEnabled()) return
    if (links.containsKey(device.deviceName)) return
    val driver = driverFor(device) ?: return
    if (!usb.hasPermission(device)) return  // UsbBridge requests it; we re-attach on grant
    val conn = runCatching { usb.openDevice(device) }.getOrNull() ?: return
    val port = driver.ports.firstOrNull() ?: run { runCatching { conn.close() }; return }
    try {
      port.open(conn)
      port.setParameters(9600, 8, UsbSerialPort.STOPBITS_1, UsbSerialPort.PARITY_NONE)
    } catch (e: Exception) {
      NLog.e("UsbSerial", "open failed: ${e.message}")
      runCatching { port.close() }; runCatching { conn.close() }; return
    }
    val out = IntArray(1)
    val slave = Pty.open(out)
    if (slave == null) {
      NLog.e("UsbSerial", "openPty failed")
      runCatching { port.close() }; runCatching { conn.close() }; return
    }
    val pfd = ParcelFileDescriptor.adoptFd(out[0])
    val tty = "ttyUSB${links.size}"
    val link = Link(port, conn, pfd, slave, tty)
    links[device.deviceName] = link
    startPump(link)
    Kmsg.log("usb-serial: $tty <- ${"%04x:%04x".format(device.vendorId, device.productId)} (${driver.javaClass.simpleName})")
    NLog.e("UsbSerial", "attached $tty (${device.deviceName})")
  }

  @Synchronized
  fun onDetached(device: UsbDevice?) {
    teardown(links.remove(device?.deviceName ?: return) ?: return)
  }

  @Synchronized
  fun stopAll() {
    links.values.forEach { teardown(it) }
    links.clear()
  }

  private fun teardown(link: Link) {
    link.running = false
    runCatching { link.pfd.close() }       // unblocks the tx read on the master
    link.threads.forEach { it.interrupt() }
    runCatching { link.port.close() }
    runCatching { link.conn.close() }
    Kmsg.log("usb-serial: ${link.ttyName} disconnected")
  }

  private fun startPump(link: Link) {
    // chip -> pty ; this loop also polls the guest's termios (ticks every 200ms).
    val rx = Thread({
      val buf = ByteArray(4096)
      while (link.running) {
        applyParamsIfChanged(link)
        val n = try {
          link.port.read(buf, 200)
        } catch (e: Exception) {
          if (link.running) NLog.e("UsbSerial", "rx: ${e.message}"); break
        }
        if (n > 0) writeFully(link, buf, n)
      }
    }, "ttyusb-rx").apply { isDaemon = true }

    // pty -> chip
    val tx = Thread({
      val buf = ByteArray(4096)
      while (link.running) {
        val n = try {
          Os.read(link.masterJfd, buf, 0, buf.size)
        } catch (e: Exception) {
          break   // master closed (teardown) or slave gone
        }
        if (n <= 0) break
        runCatching { link.port.write(buf.copyOf(n), 1000) }
      }
    }, "ttyusb-tx").apply { isDaemon = true }

    link.threads.add(rx); link.threads.add(tx)
    rx.start(); tx.start()
  }

  /** Write all [len] bytes to the PTY master (handles partial writes). */
  private fun writeFully(link: Link, buf: ByteArray, len: Int) {
    var off = 0
    while (link.running && off < len) {
      val w = try { Os.write(link.masterJfd, buf, off, len - off) } catch (e: Exception) { return }
      if (w <= 0) return
      off += w
    }
  }

  /** Read the PTY termios and program the chip when baud/parity/etc. change. */
  private fun applyParamsIfChanged(link: Link) {
    val p = runCatching { Pty.serialParams(link.masterFd) }.getOrNull() ?: return
    if (p.contentEquals(link.lastParams)) return
    link.lastParams = p
    val parity = when (p[3]) {
      1 -> UsbSerialPort.PARITY_ODD
      2 -> UsbSerialPort.PARITY_EVEN
      3 -> UsbSerialPort.PARITY_MARK
      4 -> UsbSerialPort.PARITY_SPACE
      else -> UsbSerialPort.PARITY_NONE
    }
    runCatching { link.port.setParameters(p[0], p[1], p[2], parity) }
  }

  private fun driverFor(device: UsbDevice): UsbSerialDriver? {
    when (UsbSerialIds.driverFor(device.vendorId, device.productId)) {
      "FtdiSerialDriver" -> return FtdiSerialDriver(device)
      "Cp21xxSerialDriver" -> return Cp21xxSerialDriver(device)
      "Ch34xSerialDriver" -> return Ch34xSerialDriver(device)
      "ProlificSerialDriver" -> return ProlificSerialDriver(device)
    }
    for (i in 0 until device.interfaceCount) {
      val c = device.getInterface(i).interfaceClass
      if (c == UsbConstants.USB_CLASS_COMM || c == UsbConstants.USB_CLASS_CDC_DATA) {
        return CdcAcmSerialDriver(device)
      }
    }
    return null
  }
}
