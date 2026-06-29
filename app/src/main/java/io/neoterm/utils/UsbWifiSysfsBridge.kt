package io.neoterm.utils

import io.neoterm.component.config.NeoTermPath
import java.io.File

/**
 * Fake `/sys/class/net` + `/sys/class/ieee80211` trees so the distro's `ip link`,
 * `if_nametoindex`, `ifconfig -a` and readdir of `/sys/class/net` see the Wi-Fi
 * interface. The real Android `/sys/class/net` is restricted for the app uid and
 * its netdevs aren't usable under proot anyway, so we overlay it (same idea as
 * [UsbSysfsBridge] for `/sys/bus/usb` and the sensor/block sysfs bridges).
 *
 * These trees are **populated by the daemon** (`libukwifid.so`), not here: the
 * daemon knows the real netdev/wiphy (only after a chip's driver is `modprobe`d),
 * and writes the per-interface/phy files into these bound dirs via its `wsysfs`
 * writer (target paths handed to it as `UK_WIFI_SYSFS_NET` / `UK_WIFI_SYSFS_PHY`).
 * Here we just provide the bound mount points + a static `lo`, so the dirs exist
 * and are listable from the start; `wlanN`/`phyN` appear when a driver comes up.
 */
object UsbWifiSysfsBridge {
  private val netDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-class-net")
  private val phyDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-class-ieee80211")

  /** Host paths the daemon writes into (passed to ukwifid as env). */
  fun netDirPath(): String = netDir.absolutePath
  fun phyDirPath(): String = phyDir.absolutePath

  /** Host dir → guest path binds. */
  fun sysfsBinds(): List<Pair<String, String>> {
    netDir.mkdirs(); phyDir.mkdirs()
    seedLoopback()
    return listOf(
      netDir.absolutePath to "/sys/class/net",
      phyDir.absolutePath to "/sys/class/ieee80211"
    )
  }

  private fun w(dir: File, name: String, value: String) {
    val f = File(dir, name); f.parentFile?.mkdirs()
    runCatching { f.writeText(value) }
  }

  /** A minimal `lo` so tools that expect loopback in /sys/class/net don't choke
   *  (the overlay hides the real netdevs, which the guest can't use regardless). */
  private fun seedLoopback() {
    val lo = File(netDir, "lo")
    if (lo.isDirectory) return
    lo.mkdirs()
    w(lo, "address", "00:00:00:00:00:00\n")
    w(lo, "addr_len", "6\n")
    w(lo, "ifindex", "1\n")
    w(lo, "type", "772\n")          // ARPHRD_LOOPBACK
    w(lo, "flags", "0x9\n")         // IFF_UP | IFF_LOOPBACK
    w(lo, "mtu", "65536\n")
    w(lo, "operstate", "unknown\n")
    w(lo, "carrier", "1\n")
    w(lo, "uevent", "INTERFACE=lo\nIFINDEX=1\n")
  }
}
