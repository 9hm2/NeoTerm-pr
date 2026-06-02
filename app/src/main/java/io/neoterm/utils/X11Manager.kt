package io.neoterm.utils

import android.content.Context
import android.content.Intent
import android.os.Build
import android.system.ErrnoException
import android.system.Os
import java.io.File

/**
 * Drives the embedded Termux:X11 native X server, which is built directly into
 * the NeoTerm APK (the :x11 module — single APK, no separate package to install).
 *
 * Two pieces cooperate:
 *  - [startServer] spawns `com.termux.x11.CmdEntryPoint` in a host process via
 *    app_process with NeoTerm's own APK on the classpath. That process creates
 *    the abstract X socket on display :0 and runs libXlorie.so. proot sessions
 *    already export DISPLAY=:0, so GUI apps in the distro connect to it.
 *  - [launchDisplay] opens `com.termux.x11.MainActivity` (the LorieView surface)
 *    in-process; it connects to the socket and renders.
 *
 * Because the X server and the GUI now live in the same package as NeoTerm, the
 * CmdEntryPoint broadcast is retargeted at our own package via
 * TERMUX_X11_OVERRIDE_PACKAGE.
 *
 * @author kiva
 */
object X11Manager {
  private const val ACTIVITY = "com.termux.x11.MainActivity"
  private const val CMD_ENTRY = "com.termux.x11.CmdEntryPoint"

  /** Always true now: the X server ships inside the NeoTerm APK. */
  fun isServerInstalled(context: Context): Boolean = true

  /** Launch the X11 display activity (the window that shows the X server output). */
  fun launchDisplay(context: Context) {
    runCatching {
      context.startActivity(
        Intent().setClassName(context.packageName, ACTIVITY)
          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
      )
    }.onFailure {
      NLog.e("X11Manager", "Failed to open X11 display: ${it.localizedMessage}")
    }
  }

  /**
   * Start the X server (CmdEntryPoint) as a host process bound to :0, then show
   * the display. The server creates the abstract X socket that proot apps reach
   * via DISPLAY=:0. Runs from NeoTerm's own base.apk; the ACTION_START broadcast
   * is retargeted at our package so the embedded MainActivity receives it.
   */
  fun startServer(context: Context) {
    runCatching {
      val apk = context.applicationInfo.sourceDir
      // CmdEntryPoint runs in this separate app_process (not the GUI process),
      // so it can't rely on the already-loaded libXlorie.so. It resolves the
      // native lib with getResource("lib/<abi>/libXlorie.so") + System.load(),
      // which needs the resource to be a REAL file. Our APK uses
      // extractNativeLibs=true (proot needs nativeLibraryDir), so the lib is on
      // disk — we expose it through a tiny classpath dir laid out as
      // lib/<abi>/libXlorie.so and put it first on the classpath.
      val libClasspath = buildNativeLibClasspath(context)
      val classpath = if (libClasspath != null) "$libClasspath:$apk" else apk

      val builder = ProcessBuilder(
        "/system/bin/app_process",
        "-Djava.class.path=$classpath",
        "/system/bin",
        CMD_ENTRY,
        ":0"
      )
      val env = builder.environment()
      env["CLASSPATH"] = classpath
      env["TERMUX_X11_OVERRIDE_PACKAGE"] = context.packageName
      // app_process must not inherit a proot/distro LD_* environment.
      env.remove("LD_LIBRARY_PATH")
      env.remove("LD_PRELOAD")
      builder.redirectErrorStream(true)
      builder.start()
    }.onFailure {
      NLog.e("X11Manager", "Failed to start X server: ${it.localizedMessage}")
    }
    launchDisplay(context)
  }

  /**
   * Build a classpath entry directory `…/x11/classpath` containing
   * `lib/<abi>/libXlorie.so` symlinked to the extracted native lib, so
   * CmdEntryPoint's getResource() lookup resolves to a loadable file path.
   * Returns the dir, or null if the lib can't be located.
   */
  private fun buildNativeLibClasspath(context: Context): String? {
    val abi = Build.SUPPORTED_ABIS.firstOrNull() ?: return null
    val nativeLib = File(context.applicationInfo.nativeLibraryDir, "libXlorie.so")
    if (!nativeLib.exists()) return null

    val root = File(context.filesDir, "x11/classpath")
    val abiDir = File(root, "lib/$abi").apply { mkdirs() }
    val link = File(abiDir, "libXlorie.so")
    if (link.exists()) link.delete()
    return try {
      Os.symlink(nativeLib.absolutePath, link.absolutePath)
      root.absolutePath
    } catch (e: ErrnoException) {
      // Fall back to a plain copy if symlinks aren't permitted here.
      runCatching { nativeLib.copyTo(link, overwrite = true) }
        .map { root.absolutePath }
        .getOrNull()
    }
  }
}
