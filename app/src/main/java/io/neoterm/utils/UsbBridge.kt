package io.neoterm.utils

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.Build
import android.system.Os
import io.neoterm.component.config.NeoPreference
import io.neoterm.setup.proot.Kmsg
import io.neoterm.setup.usbserial.UsbSerialBridge
import java.io.FileDescriptor

/**
 * Android-side USB host integration, wired up *exclusively* through a
 * dynamically-registered [BroadcastReceiver] (no manifest device_filter), and
 * exposed to the proot distro *only* over a socket — nothing is written into the
 * distro's filesystem.
 *
 * Under proot there's no root, no kernel modules and no direct /dev/bus/usb
 * access, so the only way to talk to a USB device is libusb on the file
 * descriptor that [UsbManager.openDevice] returns. We keep the connection open
 * and serve, over an abstract-namespace unix socket ("io.neoterm.usb"):
 *   - "LIST\n"            → text list of all connected devices (+ permission),
 *   - "<deviceName>\n"    → that device's fd via SCM_RIGHTS + an "OK …" header.
 * The abstract socket is reachable from the proot (it shares the net namespace);
 * a guest client wraps the received fd with libusb_wrap_sys_device.
 *
 * Registered/unregistered by NeoTermService for the app's lifetime.
 */
object UsbBridge {
  const val ACTION_USB_PERMISSION = "io.neoterm.action.USB_PERMISSION"
  private const val SOCKET_NAME = "io.neoterm.usb"

  private var receiver: BroadcastReceiver? = null
  private var server: Thread? = null
  @Volatile private var serverSocket: LocalServerSocket? = null

  // Open connections by device name (/dev/bus/usb/BBB/DDD), kept open so their
  // fd stays valid for serving to the distro.
  private val connections = HashMap<String, UsbDeviceConnection>()

  fun register(context: Context) {
    if (receiver != null) return
    val app = context.applicationContext
    val r = UsbReceiver()
    val filter = IntentFilter().apply {
      addAction(UsbManager.ACTION_USB_DEVICE_ATTACHED)
      addAction(UsbManager.ACTION_USB_DEVICE_DETACHED)
      addAction(ACTION_USB_PERMISSION)
    }
    // targetSdk 28 → no RECEIVER_EXPORTED/NOT_EXPORTED flag required.
    app.registerReceiver(r, filter)
    receiver = r
    startServer(app)
    runCatching { requestForConnected(app) }
  }

  fun unregister(context: Context) {
    receiver?.let { runCatching { context.applicationContext.unregisterReceiver(it) } }
    receiver = null
    runCatching { serverSocket?.close() }
    serverSocket = null
    server = null
    synchronized(connections) {
      connections.values.forEach { runCatching { it.close() } }
      connections.clear()
    }
  }

  fun usbManager(context: Context): UsbManager =
    context.getSystemService(Context.USB_SERVICE) as UsbManager

  /** Request permission for everything currently connected (so all become servable). */
  fun requestForConnected(context: Context) {
    val usb = usbManager(context)
    for (device in usb.deviceList.values) requestPermission(context, usb, device)
  }

  fun requestPermission(context: Context, usb: UsbManager, device: UsbDevice) {
    // A known USB-serial chip + the toggle on -> the app-side serial bridge owns
    // it (pty -> /dev/ttyUSB*), not the raw fd-server (avoids a double claim).
    val serial = NeoPreference.isUsbSerialEnabled() && UsbSerialBridge.isSerial(device)
    if (usb.hasPermission(device)) {
      if (serial) UsbSerialBridge.attach(usb, device) else openAndStore(usb, device)
      return
    }
    val flags = PendingIntent.FLAG_UPDATE_CURRENT or
      (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
    val intent = Intent(ACTION_USB_PERMISSION).setPackage(context.packageName)
    val pi = PendingIntent.getBroadcast(context.applicationContext, 0, intent, flags)
    usb.requestPermission(device, pi)
  }

  /** Open the device (if permitted) and keep the connection so its fd is servable. */
  private fun openAndStore(usb: UsbManager, device: UsbDevice) {
    synchronized(connections) {
      if (connections.containsKey(device.deviceName)) return
      runCatching { usb.openDevice(device) }.getOrNull()?.let {
        detachKernelDrivers(device, it)
        connections[device.deviceName] = it
      }
    }
  }

  /**
   * Detach the Android kernel driver from every interface as soon as the device
   * is opened, WITHOUT keeping the interface claimed. Mass-storage devices are
   * grabbed by the kernel's usb-storage driver (auto-mount), so a libusb claim
   * fails with LIBUSB_ERROR_BUSY (-6).
   *
   * [UsbDeviceConnection.claimInterface] with force=true issues USBDEVFS_DISCONNECT
   * (detach the kernel driver) and then claims the interface; we immediately
   * release it again. Releasing only clears the claim bit — the kernel does NOT
   * re-bind the driver (that only happens on a USB reset/re-plug) — so the
   * interface is left with no kernel driver AND not held by us. That way the
   * device is free for whoever opens it next (the guest libusb over our shared
   * fd, or any other opener) instead of us re-grabbing it ourselves. Done for
   * every interface whether or not a driver was attached.
   */
  private fun detachKernelDrivers(device: UsbDevice, conn: UsbDeviceConnection) {
    for (i in 0 until device.interfaceCount) {
      val iface = device.getInterface(i)
      val claimed = runCatching { conn.claimInterface(iface, true) }.getOrDefault(false)
      if (claimed) runCatching { conn.releaseInterface(iface) }
      NLog.e("UsbBridge", "detach iface ${iface.id} on ${device.deviceName}: $claimed")
    }
  }

  fun onDetached(device: UsbDevice?) {
    if (device != null) synchronized(connections) {
      connections.remove(device.deviceName)?.let { runCatching { it.close() } }
    }
  }

  // ---- fd server (abstract unix socket; nothing touches the distro fs) ----

  private fun startServer(context: Context) {
    if (server != null) return
    val t = Thread({
      runCatching {
        val ss = LocalServerSocket(SOCKET_NAME)
        serverSocket = ss
        while (true) {
          val client = ss.accept() ?: continue
          runCatching { handleClient(context, client) }
          runCatching { client.close() }
        }
      }
    }, "usb-fd-server").apply { isDaemon = true }
    server = t
    t.start()
  }

  private fun handleClient(context: Context, client: LocalSocket) {
    val req = readLine(client) ?: return
    val usb = usbManager(context)
    if (req == "LIST") {
      val sb = StringBuilder()
      for (d in usb.deviceList.values) {
        sb.append(d.deviceName).append("  ")
          .append(String.format("%04x:%04x", d.vendorId, d.productId))
        runCatching { d.productName }.getOrNull()?.let { if (it.isNotEmpty()) sb.append("  ").append(it) }
        sb.append(if (usb.hasPermission(d)) "  [granted]" else "  [no-permission]").append('\n')
      }
      client.outputStream.write(sb.toString().toByteArray())
      client.outputStream.flush()
      return
    }
    // Otherwise req is a device name — serve its fd.
    val device = usb.deviceList.values.firstOrNull { it.deviceName == req }
    if (device != null && usb.hasPermission(device)) openAndStore(usb, device)
    val conn = synchronized(connections) { connections[req] }
    if (conn == null || device == null) {
      client.outputStream.write("ERR no-permission\n".toByteArray())
      client.outputStream.flush()
      return
    }
    // Dup the connection's fd so the peer owns an independent copy (we keep ours).
    val dup = Os.dup(fdFromInt(conn.fileDescriptor))
    try {
      client.setFileDescriptorsForSend(arrayOf(dup))
      val header = "OK ${device.vendorId} ${device.productId} ${device.deviceName}\n"
      client.outputStream.write(header.toByteArray())
      client.outputStream.flush()
    } finally {
      runCatching { Os.close(dup) }
    }
  }

  private fun readLine(client: LocalSocket): String? {
    val input = client.inputStream
    val sb = StringBuilder()
    while (true) {
      val b = input.read()
      if (b < 0) return if (sb.isEmpty()) null else sb.toString()
      if (b == '\n'.code) return sb.toString()
      sb.append(b.toChar())
      if (sb.length > 256) return sb.toString()
    }
  }

  /** Wrap a raw int fd in a FileDescriptor (for Os.dup). */
  private fun fdFromInt(fd: Int): FileDescriptor {
    val f = FileDescriptor()
    val m = FileDescriptor::class.java.getDeclaredMethod("setInt\$", Int::class.javaPrimitiveType)
    m.isAccessible = true
    m.invoke(f, fd)
    return f
  }
}

/** USB attach/detach + permission-result broadcasts (no device_filter). */
class UsbReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    val usb = UsbBridge.usbManager(context)
    @Suppress("DEPRECATION")
    val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
    when (intent.action) {
      UsbManager.ACTION_USB_DEVICE_ATTACHED ->
        if (device != null) {
          Kmsg.log(usbKmsgLine("new USB device", device))
          if (UsbSerialBridge.isSerial(device) && !NeoPreference.isUsbSerialEnabled()) {
            Kmsg.log(
              "usb-serial: ${"%04x:%04x".format(device.vendorId, device.productId)} is a serial " +
                "adapter — enable Settings > General > 'USB serial' to expose /dev/ttyUSB*"
            )
          }
          UsbBridge.requestPermission(context, usb, device)
        }
      UsbBridge.ACTION_USB_PERMISSION -> {
        val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
        NLog.e("UsbBridge", "Permission ${if (granted) "granted" else "denied"} for ${device?.deviceName}")
        if (granted && device != null) UsbBridge.requestPermission(context, usb, device)
      }
      UsbManager.ACTION_USB_DEVICE_DETACHED -> {
        NLog.e("UsbBridge", "Detached ${device?.deviceName}")
        if (device != null) Kmsg.log(usbKmsgLine("USB disconnect,", device))
        UsbSerialBridge.onDetached(device)
        UsbBridge.onDetached(device)
      }
    }
  }
}

/** Kernel-szerű kmsg-sor egy USB-eszközről (a guest dmesg-jébe). */
private fun usbKmsgLine(what: String, d: UsbDevice): String {
  val product = runCatching { d.productName }.getOrNull()?.takeIf { it.isNotBlank() }
  return buildString {
    append("usb ").append(d.deviceName).append(": ").append(what)
    append(" idVendor=").append(String.format("%04x", d.vendorId))
    append(", idProduct=").append(String.format("%04x", d.productId))
    if (product != null) append(", Product=").append(product)
  }
}
