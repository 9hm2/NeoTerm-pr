package io.neoterm.ui.other

import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Bundle
import android.view.MenuItem
import android.view.View
import android.widget.TextView
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import de.psdev.licensesdialog.LicensesDialog
import de.psdev.licensesdialog.licenses.ApacheSoftwareLicense20
import de.psdev.licensesdialog.licenses.GnuGeneralPublicLicense20
import de.psdev.licensesdialog.licenses.GnuGeneralPublicLicense30
import de.psdev.licensesdialog.licenses.MITLicense
import de.psdev.licensesdialog.model.Notice
import de.psdev.licensesdialog.model.Notices
import android.widget.Toast
import io.neoterm.App
import io.neoterm.R
import io.neoterm.utils.TerminalColorTheme
import io.neoterm.utils.UpdateManager


/**
 * @author kiva
 */
class AboutActivity : AppCompatActivity() {
  override fun onResume() {
    super.onResume()
    // Match the terminal colors (background + foreground text).
    TerminalColorTheme.apply(this, supportActionBar, window?.decorView, null)
  }

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    setContentView(R.layout.ui_about)
    setSupportActionBar(findViewById(R.id.about_toolbar))
    supportActionBar?.setDisplayHomeAsUpEnabled(true)

    try {
      val version = packageManager.getPackageInfo(packageName, 0).versionName
      (findViewById<TextView>(R.id.app_version)).text = version
    } catch (ignored: PackageManager.NameNotFoundException) {
    }

    findViewById<View>(R.id.about_licenses_view).setOnClickListener {
      val notices = Notices()
      notices.addNotice(
        Notice(
          "PRoot",
          "https://github.com/termux/proot",
          "Copyright (C) 2010-2016 STMicroelectronics, 2015-2024 Termux",
          GnuGeneralPublicLicense20()
        )
      )
      notices.addNotice(
        Notice(
          "proot-distro",
          "https://github.com/termux/proot-distro",
          "Copyright (C) 2020-2024 Termux",
          GnuGeneralPublicLicense30()
        )
      )
      notices.addNotice(
        Notice(
          "Apache Commons Compress",
          "https://commons.apache.org/proper/commons-compress/",
          "Copyright 2002-2021 The Apache Software Foundation",
          ApacheSoftwareLicense20()
        )
      )
      notices.addNotice(
        Notice(
          "XZ for Java",
          "https://tukaani.org/xz/java.html",
          "Public Domain (Lasse Collin and others)",
          MITLicense()
        )
      )
      notices.addNotice(
        Notice(
          "Termux",
          "https://termux.com",
          "Copyright 2016-2024 Fredrik Fornwall",
          GnuGeneralPublicLicense30()
        )
      )
      notices.addNotice(
        Notice(
          "Android-Terminal-Emulator",
          "https://github.com/jackpal/Android-Terminal-Emulator",
          "Copyright (c) 2011-2016 Steven Luo",
          ApacheSoftwareLicense20()
        )
      )
      notices.addNotice(
        Notice(
          "ChromeLikeTabSwitcher",
          "https://github.com/michael-rapp/ChromeLikeTabSwitcher",
          "Copyright (c) 2016-2017 Michael Rapp",
          ApacheSoftwareLicense20()
        )
      )
      notices.addNotice(
        Notice(
          "Color-O-Matic",
          "https://github.com/GrenderG/Color-O-Matic",
          "Copyright 2016-2017 GrenderG",
          GnuGeneralPublicLicense30()
        )
      )
      notices.addNotice(
        Notice(
          "EventBus",
          "http://greenrobot.org",
          "Copyright (C) 2012-2016 Markus Junginger, greenrobot (http://greenrobot.org)",
          ApacheSoftwareLicense20()
        )
      )
      notices.addNotice(
        Notice(
          "RecyclerView-FastScroll",
          "https://github.com/timusus/RecyclerView-FastScroll",
          "Copyright (c) 2016, Tim Malseed",
          ApacheSoftwareLicense20()
        )
      )
      notices.addNotice(
        Notice(
          "SortedListAdapter",
          "https://wrdlbrnft.github.io/SortedListAdapter/",
          "Copyright (c) 2017 Wrdlbrnft",
          MITLicense()
        )
      )
      LicensesDialog.Builder(this)
        .setNotices(notices)
        .setIncludeOwnLicense(true)
        .build()
        .show()
    }

    findViewById<View>(R.id.about_version_view).setOnClickListener {
      App.get().easterEgg(this, "Emmmmmm...")
    }

    findViewById<View>(R.id.about_source_code_view).setOnClickListener {
      openUrl("https://github.com/9hm2/NeoTerm-pr")
    }

    findViewById<View>(R.id.about_keyboard_shortcuts_view).setOnClickListener {
      AlertDialog.Builder(this)
        .setTitle(R.string.keyboard_shortcuts_title)
        .setMessage(R.string.keyboard_shortcuts_content)
        .setPositiveButton(android.R.string.ok, null)
        .show()
    }

    findViewById<View>(R.id.about_check_update_view).setOnClickListener {
      Toast.makeText(this, R.string.update_checking, Toast.LENGTH_SHORT).show()
      UpdateManager.checkForUpdate { info ->
        if (isFinishing) return@checkForUpdate
        if (info == null) {
          Toast.makeText(this, R.string.update_up_to_date, Toast.LENGTH_SHORT).show()
        } else {
          showUpdateDialog(info)
        }
      }
    }

    findViewById<View>(R.id.about_reset_app_view).setOnClickListener {
      AlertDialog.Builder(this)
        .setMessage(R.string.reset_app_warning)
        .setPositiveButton(R.string.yes) { _, _ ->
          resetApp()
        }
        .setNegativeButton(android.R.string.no, null)
        .show()
    }
  }

  private fun showUpdateDialog(info: UpdateManager.UpdateInfo) {
    AlertDialog.Builder(this)
      .setTitle(getString(R.string.update_available_title, info.tag))
      .setMessage(info.notes.ifBlank { getString(R.string.update_available_message) })
      .setPositiveButton(R.string.update_download_install) { _, _ -> startUpdate(info) }
      .setNeutralButton(R.string.update_open_releases) { _, _ -> UpdateManager.openReleasesPage(this) }
      .setNegativeButton(android.R.string.cancel, null)
      .show()
  }

  private fun startUpdate(info: UpdateManager.UpdateInfo) {
    if (!UpdateManager.canInstall(this)) {
      AlertDialog.Builder(this)
        .setMessage(R.string.update_need_install_permission)
        .setPositiveButton(R.string.update_open_settings) { _, _ -> UpdateManager.requestInstallPermission(this) }
        .setNegativeButton(android.R.string.cancel, null)
        .show()
      return
    }
    UpdateManager.downloadAndInstall(this, info)
  }

  private fun resetApp() {
    // Force a fresh download/extract of the selected distro's rootfs.
    startActivity(
      Intent(this, SetupActivity::class.java)
        .putExtra(SetupActivity.EXTRA_FORCE_REINSTALL, true)
    )
  }

  private fun openUrl(url: String) {
    val intent = Intent(Intent.ACTION_VIEW)
    intent.data = Uri.parse(url)
    startActivity(intent)
  }

  override fun onOptionsItemSelected(item: MenuItem): Boolean {
    when (item.itemId) {
      android.R.id.home ->
        finish()
    }
    return super.onOptionsItemSelected(item)
  }
}
