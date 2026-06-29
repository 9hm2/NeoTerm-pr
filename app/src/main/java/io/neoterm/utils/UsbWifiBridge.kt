package io.neoterm.utils

import io.neoterm.App
import io.neoterm.component.config.NeoPreference
import io.neoterm.setup.proot.Kmsg
import java.io.File

/**
 * Launches and supervises the native **ukwifid** daemon (packaged as
 * `libukwifid.so`), the uKernel Wi-Fi framework, which serves the abstract socket
 * `@io.neoterm.wifi`.
 *
 * The daemon embeds the kernel-API shim + a custom cfg80211 + the usbfs HCD; it
 * starts **chip-agnostic with no driver**. The guest loads a vendor driver with
 * `modprobe <chip>` (→ the proot `UK_WIFI` redirect → `UK_OP_MODPROBE` → the
 * daemon `dlopen`s `$UK_WIFI_MODDIR/<chip>.so` + probes the device over
 * [UsbBridge]'s `io.neoterm.usb` fd). `lsmod`/`rmmod` and `iw`/`wpa_supplicant`
 * then drive it. No root.
 *
 * Lifecycle mirrors [io.neoterm.setup.usbserial.FsBridge]: started lazily from
 * ProotManager when the USB Wi-Fi toggle is on, torn down on service stop. Only
 * active with the toggle. Chip driver `.so`s ship separately into the module dir.
 */
object UsbWifiBridge {
  private const val SOCKET = "@io.neoterm.wifi"   // abstract namespace
  @Volatile private var started = false
  private var proc: Process? = null

  /** The bundled ukwifid binary (jniLibs/<abi>/libukwifid.so -> nativeLibraryDir). */
  private fun binaryPath(): String? {
    val f = File(App.get().applicationInfo.nativeLibraryDir, "libukwifid.so")
    return if (f.canExecute()) f.absolutePath else null
  }

  /** Kill any leaked ukwifid from a previous app process (it can be reparented to
   *  init and keep @io.neoterm.wifi bound, so a fresh launch would fail to bind). */
  private fun killStale() {
    val self = android.os.Process.myPid()
    runCatching {
      File("/proc").listFiles { f -> f.isDirectory && f.name.all(Char::isDigit) }?.forEach { p ->
        val pid = p.name.toIntOrNull() ?: return@forEach
        if (pid == self) return@forEach
        val cmd = runCatching { File(p, "cmdline").readText() }.getOrNull() ?: return@forEach
        if (cmd.contains("libukwifid.so")) {
          Kmsg.log("usb-wifi: killing stale ukwifid pid=$pid")
          runCatching { android.os.Process.killProcess(pid) }
        }
      }
    }
  }

  @Synchronized
  fun ensureReady() {
    if (started) return
    if (!NeoPreference.isUsbWifiEnabled()) return
    val bin = binaryPath() ?: run {
      Kmsg.log("usb-wifi: libukwifid.so not found / not executable — Wi-Fi framework disabled")
      return
    }
    killStale()
    // Serve mode, no driver module: the daemon waits for the guest's modprobe to
    // load a vendor .so. Drivers are resolved from the app lib dir by default
    // (UK_WIFI_MODDIR); the chip fd comes from io.neoterm.usb on probe.
    val log = File(App.get().filesDir, "ukwifid.log")
    proc = try {
      ProcessBuilder(bin, "--serve", "--sock", SOCKET, "--hcd", "usbfs")
        .apply {
          // nl80211 engine tracing into ukwifid.log (GETFAMILY/commands/events) —
          // invaluable while bringing a chip up; the log is app-private, not noisy
          // for the user.
          environment()["UK_NL_DEBUG"] = "1"
          // where the daemon writes the fake /sys/class/net + /sys/class/ieee80211
          environment()["UK_WIFI_SYSFS_NET"] = UsbWifiSysfsBridge.netDirPath()
          environment()["UK_WIFI_SYSFS_PHY"] = UsbWifiSysfsBridge.phyDirPath()
          environment()["UK_WIFI_PROCMOD"] = UsbWifiSysfsBridge.procModPath()
          // The chip's vendor driver .so and request_firmware() .bin live in the
          // GUEST distro rootfs (so they can be dropped in from the guest with cp):
          //   driver  -> /lib/ukwifi/<name>.so   (UK_WIFI_MODDIR)
          //   firmware-> /lib/firmware/...        (UK_WIFI_FW_DIR; firmware-realtek)
          // The daemon runs app-side, so it needs the absolute rootfs paths.
          runCatching {
            val rootfs = io.neoterm.setup.proot.ProotManager.selectedDistro().rootfsPath()
            File("$rootfs/lib/ukwifi").mkdirs()
            environment()["UK_WIFI_MODDIR"] = "$rootfs/lib/ukwifi"
            environment()["UK_WIFI_FW_DIR"] = "$rootfs/lib/firmware"
          }
        }
        .redirectErrorStream(true)
        .redirectOutput(ProcessBuilder.Redirect.to(log))
        .start()
    } catch (e: Exception) {
      Kmsg.log("usb-wifi: ukwifid launch failed: ${e.message}")
      return
    }
    started = true
    Kmsg.log("usb-wifi: ukwifid up on $SOCKET (modprobe a driver in the distro to bring a chip up)")
  }

  @Synchronized
  fun stopAll() {
    runCatching { proc?.destroy() }
    proc = null
    started = false
  }
}
