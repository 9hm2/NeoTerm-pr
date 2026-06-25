package io.neoterm.setup.usbserial

import io.neoterm.App
import io.neoterm.component.config.NeoPreference
import io.neoterm.setup.proot.Kmsg
import java.io.File

/**
 * Launches and supervises the native **ukfsd** daemon (packaged as
 * `libukfsd.so`), which serves the abstract socket `io.neoterm.fs`.
 *
 * ukfsd embeds the real Linux filesystem drivers (vfat/exfat/ntfs3/ext4 via the
 * vendored uKernel shim). When the guest mounts `/dev/uksd0`, the proot
 * VFS-redirect (enter.c, `UK_FS=1`) routes the mount + every path syscall under
 * it to ukfsd over `io.neoterm.fs`; ukfsd in turn reads/writes raw sectors from
 * [BlockBridge] over `io.neoterm.block`. So the filesystem is parsed in this
 * app's address space — never by the Android kernel — and no root is required.
 *
 * Lifecycle mirrors [BlockBridge]: started lazily from ProotManager when the USB
 * storage toggle is on, torn down on service stop. Only active with the toggle.
 */
object FsBridge {
  @Volatile private var started = false
  private var proc: Process? = null

  /** The bundled ukfsd binary (jniLibs/<abi>/libukfsd.so -> nativeLibraryDir). */
  private fun binaryPath(): String? {
    val f = File(App.get().applicationInfo.nativeLibraryDir, "libukfsd.so")
    return if (f.canExecute()) f.absolutePath else null
  }

  /**
   * Kill any leaked ukfsd from a previous app process. ukfsd can be reparented
   * to init and survive an app upgrade/restart, keeping the abstract socket
   * @io.neoterm.fs bound — the freshly launched ukfsd would then fail to bind
   * and exit, leaving the stale (old-code) daemon serving requests. Scan /proc
   * for same-UID processes whose cmdline is our libukfsd.so and SIGKILL them.
   */
  private fun killStale() {
    val self = android.os.Process.myPid()
    runCatching {
      File("/proc").listFiles { f -> f.isDirectory && f.name.all(Char::isDigit) }?.forEach { p ->
        val pid = p.name.toIntOrNull() ?: return@forEach
        if (pid == self) return@forEach
        val cmd = runCatching { File(p, "cmdline").readText() }.getOrNull() ?: return@forEach
        if (cmd.contains("libukfsd.so")) {
          Kmsg.log("usb-fs: killing stale ukfsd pid=$pid")
          runCatching { android.os.Process.killProcess(pid) }
        }
      }
    }
  }

  @Synchronized
  fun ensureReady() {
    if (started) return
    if (!NeoPreference.isUsbStorageEnabled()) return
    val bin = binaryPath() ?: run {
      Kmsg.log("usb-fs: libukfsd.so not found / not executable — FS redirect disabled")
      return
    }
    killStale()   // clear any leaked ukfsd holding @io.neoterm.fs from a prior run
    // ukfsd defaults: listen on io.neoterm.fs, read sectors over io.neoterm.block.
    val log = File(App.get().filesDir, "ukfsd.log")
    proc = try {
      ProcessBuilder(bin)
        .redirectErrorStream(true)
        .redirectOutput(ProcessBuilder.Redirect.appendTo(log))
        .start()
    } catch (e: Exception) {
      Kmsg.log("usb-fs: ukfsd launch failed: ${e.message}")
      return
    }
    started = true
    Kmsg.log("usb-fs: ukfsd up — io.neoterm.fs (FS engine over io.neoterm.block)")
  }

  @Synchronized
  fun stopAll() {
    runCatching { proc?.destroy() }
    proc = null
    started = false
  }
}
