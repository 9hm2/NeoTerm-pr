package io.neoterm.setup

import android.app.ProgressDialog
import android.content.Context
import android.os.Build
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import io.neoterm.App
import io.neoterm.R
import io.neoterm.component.config.NeoPreference
import io.neoterm.component.config.NeoTermPath
import io.neoterm.setup.proot.Distro
import io.neoterm.setup.proot.ProotInstaller
import io.neoterm.setup.proot.ProotManager
import java.io.File
import java.util.*

/**
 * @author kiva
 */
interface ResultListener {
  fun onResult(error: Exception?)
}

/**
 * @author kiva
 */
object SetupHelper {
  fun needSetup(): Boolean {
    // Proot módban a kiválasztott disztró rootfs-e + a proot bináris kell;
    // legacy (Termux-stílusú) módban a usr/ PREFIX könyvtár.
    return if (NeoPreference.isProotEnabled()) {
      !ProotManager.isInstalled()
    } else {
      !File(NeoTermPath.USR_PATH).isDirectory
    }
  }

  fun setup(
    activity: AppCompatActivity, connection: SourceConnection,
    resultListener: ResultListener
  ) {
    if (!needSetup()) {
      resultListener.onResult(null)
      return
    }

    val prefixFile = File(NeoTermPath.USR_PATH)

    val progress = makeProgressDialog(activity)
    progress.max = 100
    progress.show()

    SetupThread(activity, connection, prefixFile, resultListener, progress).start()
  }

  /**
   * A proot mód telepítése: a kiválasztott disztró rootfs-ének + a proot
   * binárisnak a letöltése a megadott base-URL-ről.
   */
  fun setupProot(
    activity: AppCompatActivity,
    resultListener: ResultListener,
    baseUrl: String = NeoPreference.getProotSource(),
    distro: Distro = ProotManager.selectedDistro(),
    forceReinstall: Boolean = false
  ) {
    val arch = determineArchName()
    if (!ProotManager.isArchSupported(arch)) {
      resultListener.onResult(
        RuntimeException(activity.getString(R.string.proot_unsupported_arch, arch))
      )
      return
    }

    if (!forceReinstall && !needSetup()) {
      resultListener.onResult(null)
      return
    }

    // Force reinstall (or distro switch): drop the old rootfs first so the
    // download replaces it cleanly and corrupted installs can be recovered.
    if (forceReinstall) {
      ProotManager.uninstall(distro)
    }

    val progress = makeProgressDialog(activity, activity.getString(R.string.installer_message))
    progress.max = 100
    progress.show()

    ProotInstaller(
      activity, distro, arch, baseUrl, resultListener, progress
    ).start()
  }

  private fun makeProgressDialog(context: Context): ProgressDialog {
    return makeProgressDialog(context, context.getString(R.string.installer_message))
  }

  fun makeProgressDialog(context: Context, message: String): ProgressDialog {
    val dialog = ProgressDialog(context)
    dialog.setMessage(message)
    dialog.isIndeterminate = false
    dialog.setCancelable(false)
    dialog.setProgressStyle(ProgressDialog.STYLE_HORIZONTAL)
    return dialog
  }

  fun makeErrorDialog(context: Context, messageId: Int): AlertDialog {
    return makeErrorDialog(context, context.getString(messageId))
  }

  fun makeErrorDialog(context: Context, message: String): AlertDialog {
    return AlertDialog.Builder(context)
      .setTitle(R.string.error)
      .setMessage(message)
      .setPositiveButton(android.R.string.yes, null)
      .setNeutralButton(R.string.show_help) { _, _ -> App.get().openHelpLink() }
      .create()
  }

  fun determineArchName(): String {
    for (androidArch in Build.SUPPORTED_ABIS) {
      when (androidArch) {
        "arm64-v8a" -> return "aarch64"
        "armeabi-v7a" -> return "arm"
        "x86_64" -> return "x86_64"
      }
    }
    throw RuntimeException(
      "Unable to determine arch from Build.SUPPORTED_ABIS =  "
        + Arrays.toString(Build.SUPPORTED_ABIS)
    )
  }
}
