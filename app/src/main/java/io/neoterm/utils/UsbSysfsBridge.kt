package io.neoterm.utils

import android.content.Context
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbManager
import android.system.Os
import io.neoterm.component.config.NeoTermPath
import io.neoterm.setup.proot.Kmsg
import java.io.ByteArrayOutputStream
import java.io.File

/**
 * Fake `/sys/bus/usb` tree so the distro's **unmodified** libusb/libudev enumerate
 * the phone's USB devices — `lsusb`, pyusb, libftdi, rtl-sdr, … with no patched
 * libusb and no preload. Same idea as [io.neoterm.setup.usbserial.BlockSysfsBridge]
 * for `/sys/block`.
 *
 * The catch this solves: systemd's libudev refuses any device dir not on a real
 * sysfs filesystem (`fd_is_fs_type(SYSFS_MAGIC)`), so a bound fake `/sys` is
 * normally rejected. ProotManager exports `SYSTEMD_DEVICE_VERIFY_SYSFS=0` (a
 * systemd-internal gate) which makes libudev skip that check — then this tree is
 * accepted. `libusb_init`'s netlink hotplug monitor is handled separately by the
 * proot UK_USB shim.
 *
 * Per device we write exactly the files libudev + libusb read:
 * `uevent`, `busnum`, `devnum`, `speed`, `bConfigurationValue`, `bNumConfigurations`,
 * `idVendor`, `idProduct`, the binary `descriptors` blob, and a `subsystem`
 * symlink resolving to `…/bus/usb`. The device dir lives directly under
 * `/sys/bus/usb/devices/<name>` (a real dir, not a `/sys/devices` symlink) so we
 * never overlay the real `/sys/devices`; libudev applies lax rules to such paths.
 *
 * Bound (empty) from proot launch; [refresh] (re)fills it on attach/detach/permission.
 */
object UsbSysfsBridge {
  private const val TAG = "usb-sysfs"
  // Two trees: /sys/bus/usb holds the per-device symlinks; the device dirs live
  // under /sys/devices/neoterm-usb (a NEW subdir, so the real /sys/devices is left
  // intact). libudev requires a device's canonical path to be under /sys/devices.
  private val usbDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-bus-usb")
  private val devDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-devices-usb")
  // A readable overlay for the PARENT /sys/bus. The real Android /sys/bus is
  // `drwx------` (EACCES for the app uid), so `readdir(/sys/bus)` fails — and
  // libudev's enumerator readdir()s /sys/bus to discover bus names BEFORE it ever
  // looks at /sys/bus/usb/devices, so without this it finds 0 USB devices. We bind
  // a writable dir over /sys/bus that physically contains the `usb` (and the
  // sensor bridge's `iio`) subdirs as mount points; the per-subsystem binds map
  // their real trees on top (proot longest-prefix match), so /sys/bus/usb and
  // /sys/bus/iio still resolve while /sys/bus itself becomes listable.
  private val busDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-bus")

  /** Host dir → guest path binds. */
  fun sysfsBinds(): List<Pair<String, String>> {
    usbDir.mkdirs(); File(usbDir, "devices").mkdirs(); devDir.mkdirs()
    // Placeholders so readdir(/sys/bus) lists the buses the per-subsystem binds
    // overlay (proot doesn't add a bound child's name to the parent's getdents).
    busDir.mkdirs(); File(busDir, "usb").mkdirs(); File(busDir, "iio").mkdirs()
    return listOf(
      busDir.absolutePath to "/sys/bus",
      usbDir.absolutePath to "/sys/bus/usb",
      devDir.absolutePath to "/sys/devices/neoterm-usb"
    )
  }

  private fun w(dir: File, name: String, value: String) {
    val f = File(dir, name); f.parentFile?.mkdirs()
    runCatching { f.writeText(value) }
  }
  private fun wb(dir: File, name: String, value: ByteArray) {
    val f = File(dir, name); f.parentFile?.mkdirs()
    runCatching { f.writeBytes(value) }
  }
  private fun symlink(link: File, target: String) {
    runCatching { if (link.exists() || isSymlink(link)) link.delete() }
    runCatching { Os.symlink(target, link.absolutePath) }
  }
  private fun isSymlink(f: File) =
    runCatching { Os.lstat(f.absolutePath).st_mode and 0xF000 == 0xA000 }.getOrDefault(false)

  @Synchronized
  fun clear() {
    runCatching { usbDir.deleteRecursively() }
    runCatching { devDir.deleteRecursively() }
    usbDir.mkdirs(); File(usbDir, "devices").mkdirs(); devDir.mkdirs()
  }

  /** Parse busnum/devnum out of a device name like "/dev/bus/usb/002/005". */
  private fun busDev(name: String): Pair<Int, Int>? {
    val m = Regex("/dev/bus/usb/(\\d+)/(\\d+)").find(name) ?: return null
    return m.groupValues[1].toInt() to m.groupValues[2].toInt()
  }

  @Synchronized
  fun refresh(context: Context) {
    clear()
    val usb = runCatching { context.getSystemService(Context.USB_SERVICE) as UsbManager }.getOrNull() ?: return
    val linksDir = File(usbDir, "devices").apply { mkdirs() }
    var n = 0
    for (device in usb.deviceList.values) {
      val (bus, dev) = busDev(device.deviceName) ?: continue
      val name = "$bus-$dev"                              // synthetic sysfs name (unique)
      val ddir = File(devDir, name).apply { mkdirs() }   // canonical dir under /sys/devices/neoterm-usb
      val desc = UsbBridge.rawDescriptors(device.deviceName) ?: synthesize(device)
      fun b(i: Int) = if (desc.size > i) desc[i].toInt() and 0xff else 0
      val cls = if (desc.size >= 5) b(4) else device.deviceClass
      val subCls = b(5); val proto = b(6)
      val mps0 = if (b(7) > 0) b(7) else 64
      val bcdUsb = b(2) or (b(3) shl 8)
      val bcdDev = b(12) or (b(13) shl 8)
      val numCfg = if (desc.size >= 18) b(17) else 1
      // config descriptor follows the 18-byte device descriptor
      val co = 18
      val numIf = if (desc.size > co + 4) b(co + 4) else 1
      val cfgVal = if (desc.size > co + 5) b(co + 5) else 1
      val bmAttr = if (desc.size > co + 7) b(co + 7) else 0x80
      w(ddir, "uevent",
        "DEVTYPE=usb_device\nBUSNUM=%03d\nDEVNUM=%03d\nDEVNAME=/dev/bus/usb/%03d/%03d\n"
          .format(bus, dev, bus, dev) +
          "PRODUCT=%x/%x/%x\nTYPE=%d/%d/%d\n".format(device.vendorId, device.productId, bcdDev, cls, subCls, proto))
      w(ddir, "busnum", "$bus\n"); w(ddir, "devnum", "$dev\n"); w(ddir, "speed", "480\n")
      w(ddir, "bConfigurationValue", "$cfgVal\n"); w(ddir, "bNumConfigurations", "$numCfg\n")
      w(ddir, "idVendor", "%04x\n".format(device.vendorId))
      w(ddir, "idProduct", "%04x\n".format(device.productId))
      w(ddir, "bDeviceClass", "%02x\n".format(cls))
      w(ddir, "bDeviceSubClass", "%02x\n".format(subCls))
      w(ddir, "bDeviceProtocol", "%02x\n".format(proto))
      w(ddir, "bMaxPacketSize0", "$mps0\n")
      w(ddir, "bcdDevice", "%04x\n".format(bcdDev))
      w(ddir, "version", "%2x.%02x\n".format(bcdUsb shr 8, bcdUsb and 0xff))
      w(ddir, "bNumInterfaces", "%2d\n".format(numIf))
      w(ddir, "bmAttributes", "%02x\n".format(bmAttr))
      // lsusb -t and other topology-aware tools read these; we are never a hub and
      // only model USB2, so maxchild=0 and a single rx/tx lane.
      w(ddir, "maxchild", "0\n"); w(ddir, "rx_lanes", "1\n"); w(ddir, "tx_lanes", "1\n")
      w(ddir, "configuration", "\n")
      wb(ddir, "descriptors", desc)
      // device's subsystem -> /sys/bus/usb (from /sys/devices/neoterm-usb/<name>)
      symlink(File(ddir, "subsystem"), "../../../bus/usb")
      // /sys/bus/usb/devices/<name> -> /sys/devices/neoterm-usb/<name>
      symlink(File(linksDir, name), "../../../devices/neoterm-usb/$name")
      n++
    }
    Kmsg.log("usb-sysfs: /sys/bus/usb populated with $n device(s)")
  }

  /** Build a USB descriptor blob (device + configs) from UsbDevice metadata when we
   *  don't hold an open connection (e.g. devices owned by the serial/block bridges
   *  or not permitted). Enough for `lsusb` to list and for `lsusb -v` topology. */
  private fun synthesize(d: UsbDevice): ByteArray {
    val out = ByteArrayOutputStream()
    fun le16(v: Int) { out.write(v and 0xff); out.write((v shr 8) and 0xff) }
    // device descriptor (18 bytes)
    out.write(18); out.write(1); le16(0x0200)
    out.write(d.deviceClass); out.write(d.deviceSubclass); out.write(d.deviceProtocol)
    out.write(64); le16(d.vendorId); le16(d.productId); le16(0)
    out.write(0); out.write(0); out.write(0)
    val nCfg = maxOf(1, d.configurationCount)
    out.write(nCfg)
    for (ci in 0 until nCfg) {
      val cfg = runCatching { d.getConfiguration(ci) }.getOrNull()
      val body = ByteArrayOutputStream()
      val nIf = cfg?.interfaceCount ?: 0
      for (ii in 0 until nIf) {
        val iface = cfg!!.getInterface(ii)
        body.write(9); body.write(4)
        body.write(iface.id); body.write(iface.alternateSetting); body.write(iface.endpointCount)
        body.write(iface.interfaceClass); body.write(iface.interfaceSubclass); body.write(iface.interfaceProtocol); body.write(0)
        for (ei in 0 until iface.endpointCount) {
          val ep = iface.getEndpoint(ei)
          body.write(7); body.write(5); body.write(ep.address); body.write(ep.attributes)
          body.write(ep.maxPacketSize and 0xff); body.write((ep.maxPacketSize shr 8) and 0xff)
          body.write(ep.interval)
        }
      }
      val total = 9 + body.size()
      out.write(9); out.write(2); le16(total); out.write(maxOf(1, nIf))
      out.write(cfg?.id ?: 1); out.write(0)
      out.write(if (cfg?.isSelfPowered == true) 0xC0 else 0x80)
      out.write(((cfg?.maxPower ?: 100) / 2) and 0xff)
      out.write(body.toByteArray())
    }
    return out.toByteArray()
  }
}
