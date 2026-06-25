package io.neoterm

import android.annotation.SuppressLint
import android.app.Application
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.SystemClock
import android.view.Gravity
import android.widget.Toast
import androidx.appcompat.app.AlertDialog
import io.neoterm.component.NeoInitializer
import io.neoterm.component.config.NeoPreference
import io.neoterm.ui.other.BonusActivity
import io.neoterm.utils.CrashHandler

/**
 * @author kiva
 */
class App : Application() {
  override fun onCreate() {
    super.onCreate()
    app = this
    // Az app-folyamat indulási pillanata (elapsedRealtime alap). Ebből számoljuk
    // a guest /proc/uptime-ját: a NeoTerm futásidejét adja vissza, és amikor az app
    // teljesen leáll (a folyamat meghal), ez a statikus mező újrainicializálódik
    // → a „uptime" nullázódik a következő indításnál.
    startElapsedRealtimeMs = SystemClock.elapsedRealtime()
    NeoPreference.init(this)
    CrashHandler.init()
    NeoInitializer.init(this)
    freshLogs()
  }

  /** Start each app process with clean kmsg/ukfsd logs (no previous run), and
   *  stamp the app version so `dmesg` shows which build is running. */
  private fun freshLogs() {
    runCatching {
      java.io.File(io.neoterm.component.config.NeoTermPath.PROOT_ROOT_PATH, "sysdata/kmsg")
        .also { it.parentFile?.mkdirs() }.writeText("")
      java.io.File(filesDir, "ukfsd.log").writeText("")
      io.neoterm.setup.proot.Kmsg.log(
        "NeoTerm ${BuildConfig.VERSION_NAME} (build ${BuildConfig.VERSION_CODE}) — app start")
    }
  }

  fun errorDialog(context: Context, message: Int, dismissCallback: (() -> Unit)?) {
    errorDialog(context, getString(message), dismissCallback)
  }

  fun errorDialog(context: Context, message: String, dismissCallback: (() -> Unit)?) {
    AlertDialog.Builder(context)
      .setTitle(R.string.error)
      .setMessage(message)
      .setNegativeButton(android.R.string.no, null)
      .setPositiveButton(R.string.show_help) { _, _ ->
        openHelpLink()
      }
      .setOnDismissListener {
        dismissCallback?.invoke()
      }
      .show()
  }

  fun openHelpLink() {
    val intent = Intent(Intent.ACTION_VIEW, Uri.parse("https://neoterm.gitbooks.io/neoterm-wiki/content/"))
    intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    startActivity(intent)
  }

  fun easterEgg(context: Context, message: String) {
    val happyCount = NeoPreference.loadInt(NeoPreference.KEY_HAPPY_EGG, 0) + 1
    NeoPreference.store(NeoPreference.KEY_HAPPY_EGG, happyCount)

    val trigger = NeoPreference.VALUE_HAPPY_EGG_TRIGGER

    if (happyCount == trigger / 2) {
      @SuppressLint("ShowToast")
      val toast = Toast.makeText(this, message, Toast.LENGTH_LONG)
      toast.setGravity(Gravity.CENTER, 0, 0)
      toast.show()
    } else if (happyCount > trigger) {
      NeoPreference.store(NeoPreference.KEY_HAPPY_EGG, 0)
      context.startActivity(Intent(context, BonusActivity::class.java))
    }
  }

  companion object {
    private var app: App? = null

    /** Az app-folyamat indulási ideje (SystemClock.elapsedRealtime, ms). A guest
     *  uptime ehhez képest számolódik; a folyamat halálával 0-ra áll vissza. */
    @JvmStatic
    var startElapsedRealtimeMs: Long = 0L
      private set

    fun get(): App {
      return app!!
    }
  }
}
