package io.neoterm.utils

import android.app.PendingIntent
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.os.Build
import io.neoterm.setup.proot.ProotManager
import java.io.File

/**
 * Android-side USB host integration, wired up *exclusively* through a
 * dynamically-registered [BroadcastReceiver] — deliberately NOT via a manifest
 * `<intent-filter>` + `device_filter` (which would auto-launch the app on every
 * matching plug-in). While NeoTerm runs we listen for attach/detach, request the
 * runtime USB permission via [UsbManager], and publish the granted devices to
 * the distro at `/tmp/neoterm-usb` (bus/device path + VID:PID + name) so Linux
 * USB tooling (lsusb/libusb) has something to work with.
 *
 * Registered/unregistered by NeoTermService for the app's lifetime.
 */
object UsbBridge {
  const val ACTION_USB_PERMISSION = "io.neoterm.action.USB_PERMISSION"

  private var receiver: BroadcastReceiver? = null

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
    // Devices already plugged in when we start get the same treatment.
    runCatching { requestForConnected(app) }
  }

  fun unregister(context: Context) {
    receiver?.let { runCatching { context.applicationContext.unregisterReceiver(it) } }
    receiver = null
  }

  fun usbManager(context: Context): UsbManager =
    context.getSystemService(Context.USB_SERVICE) as UsbManager

  /** Ask for permission for every currently-connected device. */
  fun requestForConnected(context: Context) {
    val usb = usbManager(context)
    for (device in usb.deviceList.values) requestPermission(context, usb, device)
    publish(context)
  }

  /** Request the runtime USB permission (or publish immediately if we have it). */
  fun requestPermission(context: Context, usb: UsbManager, device: UsbDevice) {
    if (usb.hasPermission(device)) {
      publish(context)
      return
    }
    val flags = PendingIntent.FLAG_UPDATE_CURRENT or
      (if (Build.VERSION.SDK_INT >= 31) PendingIntent.FLAG_MUTABLE else 0)
    val intent = Intent(ACTION_USB_PERMISSION).setPackage(context.packageName)
    val pi = PendingIntent.getBroadcast(context.applicationContext, 0, intent, flags)
    usb.requestPermission(device, pi)
  }

  /**
   * Write the connected devices (with permission state) to the selected distro's
   * /tmp/neoterm-usb, so a guest shell can `cat /tmp/neoterm-usb` to see them.
   */
  fun publish(context: Context) {
    runCatching {
      val usb = usbManager(context)
      val sb = StringBuilder()
      for (d in usb.deviceList.values) {
        sb.append(d.deviceName) // /dev/bus/usb/00X/00Y
          .append("  ")
          .append(String.format("%04x:%04x", d.vendorId, d.productId))
        runCatching { d.productName }.getOrNull()?.let { if (it.isNotEmpty()) sb.append("  ").append(it) }
        sb.append(if (usb.hasPermission(d)) "  [granted]" else "  [no-permission]")
        sb.append('\n')
      }
      val rootfs = ProotManager.selectedDistro().rootfsPath()
      val f = File("$rootfs/tmp/neoterm-usb")
      f.parentFile?.mkdirs()
      f.writeText(sb.toString())
      NLog.e("UsbBridge", "USB devices: ${usb.deviceList.size}")
    }
  }
}

/**
 * Receives USB attach/detach and our permission-result broadcast. Pure
 * BroadcastReceiver flow — no activity intent-filter / device_filter.
 */
class UsbReceiver : BroadcastReceiver() {
  override fun onReceive(context: Context, intent: Intent) {
    val usb = UsbBridge.usbManager(context)
    @Suppress("DEPRECATION")
    val device: UsbDevice? = intent.getParcelableExtra(UsbManager.EXTRA_DEVICE)
    when (intent.action) {
      UsbManager.ACTION_USB_DEVICE_ATTACHED -> {
        if (device != null) UsbBridge.requestPermission(context, usb, device)
      }
      UsbBridge.ACTION_USB_PERMISSION -> {
        val granted = intent.getBooleanExtra(UsbManager.EXTRA_PERMISSION_GRANTED, false)
        NLog.e("UsbBridge", "Permission ${if (granted) "granted" else "denied"} for ${device?.deviceName}")
        UsbBridge.publish(context)
      }
      UsbManager.ACTION_USB_DEVICE_DETACHED -> {
        NLog.e("UsbBridge", "Detached ${device?.deviceName}")
        UsbBridge.publish(context)
      }
    }
  }
}
