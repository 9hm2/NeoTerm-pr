package io.neoterm.ui.term

import android.Manifest
import android.content.*
import android.content.pm.PackageManager
import android.content.res.Configuration
import android.os.Bundle
import android.os.Handler
import android.os.IBinder
import android.preference.PreferenceManager
import android.view.*
import android.view.inputmethod.InputMethodManager
import androidx.appcompat.app.AlertDialog
import androidx.appcompat.app.AppCompatActivity
import androidx.appcompat.widget.Toolbar
import androidx.core.app.ActivityCompat
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils
import androidx.core.view.OnApplyWindowInsetsListener
import androidx.core.view.ViewCompat
import de.mrapp.android.tabswitcher.*
import io.neoterm.App
import io.neoterm.BuildConfig
import io.neoterm.R
import io.neoterm.backend.TerminalSession
import io.neoterm.component.ComponentManager
import io.neoterm.component.colorscheme.ColorSchemeComponent
import io.neoterm.backend.TerminalColors
import io.neoterm.component.config.NeoPreference
import io.neoterm.component.profile.ProfileComponent
import io.neoterm.component.session.ShellParameter
import io.neoterm.component.session.ShellProfile
import io.neoterm.component.session.ShellTermSession
import io.neoterm.component.session.XParameter
import io.neoterm.component.session.XSession
import io.neoterm.frontend.session.terminal.*
import io.neoterm.services.NeoTermService
import io.neoterm.setup.SetupHelper
import io.neoterm.ui.other.SetupActivity
import android.widget.Toast
import io.neoterm.setup.proot.PackageAction
import io.neoterm.setup.proot.ProotManager
import io.neoterm.ui.pm.PackageManagerActivity
import io.neoterm.ui.settings.SettingActivity
import io.neoterm.utils.FullScreenHelper
import io.neoterm.utils.NeoPermission
import io.neoterm.utils.RangedInt
import io.neoterm.utils.UpdateManager
import io.neoterm.utils.X11Manager
import io.neoterm.utils.runPackageManager
import org.greenrobot.eventbus.EventBus
import org.greenrobot.eventbus.Subscribe
import org.greenrobot.eventbus.ThreadMode


class NeoTermActivity : AppCompatActivity(), ServiceConnection, SharedPreferences.OnSharedPreferenceChangeListener {
  companion object {
    const val KEY_NO_RESTORE = "no_restore"
    const val REQUEST_SETUP = 22313
    private const val KEY_LAST_UPDATE_CHECK = "last_update_check"

    /** Delay before opening the first session after setup, so the rootfs is
     * settled and the content view is laid out (otherwise the terminal opens
     * blank). */
    private const val FIRST_SESSION_DELAY_MS = 350L

    /** Live instance, so the service's "Exit" action can finish the activity
     * (and remove its task) — otherwise the still-bound/recents activity would
     * re-create the service and its notification right after Exit. Cleared in
     * onDestroy, so it holds no reference past the activity's lifetime. */
    @Volatile
    private var instance: NeoTermActivity? = null

    fun getInstance(): NeoTermActivity? = instance
  }

  lateinit var tabSwitcher: TabSwitcher
  private lateinit var fullScreenHelper: FullScreenHelper
  lateinit var toolbar: Toolbar
  private lateinit var tabDots: TabDotsIndicator

  var addSessionListener = createAddSessionListener()
  private var termService: NeoTermService? = null

  // First-launch startup is gated on BOTH the service being connected AND the
  // runtime permissions having been handled, so setup never races ahead of the
  // permission prompts (and the prompts come first).
  private var serviceConnected = false
  private var permissionsHandled = false
  private var startupProceeded = false
  private var updateChecked = false

  override fun onCreate(savedInstanceState: Bundle?) {
    super.onCreate(savedInstanceState)
    instance = this

    // Keep the screen on while the terminal is in front (on by default).
    window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)

    val fullscreen = NeoPreference.isFullScreenEnabled()
    if (fullscreen) {
      window.setFlags(
        WindowManager.LayoutParams.FLAG_FULLSCREEN,
        WindowManager.LayoutParams.FLAG_FULLSCREEN
      )
    }

    setContentView(R.layout.ui_main)

    toolbar = findViewById(R.id.terminal_toolbar)
    setSupportActionBar(toolbar)

    fullScreenHelper = FullScreenHelper.injectActivity(this, fullscreen, peekRecreating())
    fullScreenHelper.setKeyBoardListener(object : FullScreenHelper.KeyBoardListener {
      override fun onKeyboardChange(isShow: Boolean, keyboardHeight: Int) {
        if (tabSwitcher.selectedTab is TermTab) {
          val tab = tabSwitcher.selectedTab as TermTab
          // isShow -> toolbarHide
          toggleToolbar(tab.toolbar, !isShow)
        }
      }
    })

    tabDots = findViewById(R.id.tab_dots)

    tabSwitcher = findViewById(R.id.tab_switcher)
    // Don't let the switcher persist tabs into instance state: restored tabs
    // come back as plain Tab (the NeoTab subclass is lost in the Parcelable),
    // which crashed lifecycle casts after fold/unfold recreation. Tabs are
    // rebuilt from the service's live sessions anyway (enterMain).
    tabSwitcher.isSaveEnabled = false
    tabSwitcher.decorator = NeoTabDecorator(this)
    ViewCompat.setOnApplyWindowInsetsListener(tabSwitcher, createWindowInsetsListener())
    tabSwitcher.showToolbars(false)

    val serviceIntent = Intent(this, NeoTermService::class.java)
    startService(serviceIntent)
    bindService(serviceIntent, this, 0)

    // Ask for the runtime permissions up front, before any setup runs.
    requestStartupPermissions()
  }

  /**
   * Request the storage and (on Android 13+) notification permissions first, so
   * the user grants them before the setup/download starts and before the
   * foreground-service notification needs them.
   */
  private fun requestStartupPermissions() {
    val needed = ArrayList<String>()
    if (ContextCompat.checkSelfPermission(this, Manifest.permission.WRITE_EXTERNAL_STORAGE)
      != PackageManager.PERMISSION_GRANTED
    ) {
      needed.add(Manifest.permission.WRITE_EXTERNAL_STORAGE)
      needed.add(Manifest.permission.READ_EXTERNAL_STORAGE)
    }
    // POST_NOTIFICATIONS is only a runtime permission on Android 13 (API 33)+.
    // The constants aren't in compileSdk 28 (kept at 28 on purpose for proot's
    // exec behaviour), so use the literals.
    if (android.os.Build.VERSION.SDK_INT >= 33 &&
      ContextCompat.checkSelfPermission(this, "android.permission.POST_NOTIFICATIONS")
      != PackageManager.PERMISSION_GRANTED
    ) {
      needed.add("android.permission.POST_NOTIFICATIONS")
    }
    // RECORD_AUDIO lets the bundled PulseAudio's AAudio source capture the mic
    // for recording apps in the distro. Only requested when the user enabled the
    // microphone in Settings; declining it just means no input (output stays).
    if (NeoPreference.isMicrophoneEnabled() &&
      ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
      != PackageManager.PERMISSION_GRANTED
    ) {
      needed.add(Manifest.permission.RECORD_AUDIO)
    }

    if (needed.isEmpty()) {
      onStartupPermissionsHandled()
    } else {
      ActivityCompat.requestPermissions(this, needed.toTypedArray(), NeoPermission.REQUEST_APP_PERMISSION)
    }
  }

  private fun onStartupPermissionsHandled() {
    permissionsHandled = true
    maybeStartup()
  }

  /**
   * Proceed to setup (or open the first session) only once the service is
   * connected *and* the permissions have been handled. Runs at most once.
   */
  private fun maybeStartup() {
    if (!serviceConnected || !permissionsHandled || startupProceeded || isRecreating()) {
      return
    }
    startupProceeded = true
    if (SetupHelper.needSetup()) {
      startActivityForResult(Intent(this, SetupActivity::class.java), REQUEST_SETUP)
    } else {
      enterMain()
      update_colors()
    }
  }

  private fun toggleToolbar(toolbar: Toolbar?, visible: Boolean) {
    if (toolbar == null) {
      return
    }

    if (NeoPreference.isFullScreenEnabled()
      || NeoPreference.isHideToolbarEnabled()
    ) {
      val toolbarHeight = toolbar.height.toFloat()
      val translationY = if (visible) 0.toFloat() else -toolbarHeight
      if (visible) {
        toolbar.visibility = View.VISIBLE
        toolbar.animate()
          .translationY(translationY)
          .start()
      } else {
        toolbar.animate()
          .translationY(translationY)
          .withEndAction {
            toolbar.visibility = View.GONE
          }
          .start()
      }
    }
  }

  override fun onCreateOptionsMenu(menu: Menu?): Boolean {
    menuInflater.inflate(R.menu.menu_main, menu)

    TabSwitcher.setupWithMenu(tabSwitcher, toolbar.menu, {
      if (!tabSwitcher.isSwitcherShown) {
        val imm = this@NeoTermActivity.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
        if (imm.isActive && tabSwitcher.selectedTab is TermTab) {
          val tab = tabSwitcher.selectedTab as TermTab
          tab.requireHideIme()
        }
        toggleSwitcher(showSwitcher = true, easterEgg = true)
      } else {
        toggleSwitcher(showSwitcher = false, easterEgg = true)
      }
    })
    return true
  }

  override fun onOptionsItemSelected(item: MenuItem): Boolean {
    return when (item.itemId) {
      R.id.menu_item_settings -> {
        startActivity(Intent(this, SettingActivity::class.java))
        true
      }
      R.id.menu_item_package_settings -> {
        startActivity(Intent(this, PackageManagerActivity::class.java))
        true
      }
      R.id.menu_item_new_session -> {
        addNewSession()
        true
      }
      R.id.menu_item_new_session_with_profile -> {
        showProfileDialog()
        true
      }
      R.id.menu_item_new_system_session -> {
        forceAddSystemSession()
        true
      }
      R.id.menu_item_new_x_session -> {
        addXSession()
        true
      }
      R.id.menu_item_x11_start -> {
        startX11()
        true
      }
      R.id.menu_item_x11_stop -> {
        stopX11()
        true
      }
      R.id.menu_item_x11_install_env -> {
        installX11Environment()
        true
      }
      else -> super.onOptionsItemSelected(item)
    }
  }

  /**
   * Start the native X11 server. It's built into this APK (the :x11 module), so
   * there's nothing to install — just spawn the server and open the display.
   */
  private fun startX11() {
    X11Manager.startServer(this)
    Toast.makeText(this, R.string.x11_started_hint, Toast.LENGTH_LONG).show()
  }

  /** Stop the embedded X11 server and close its window. */
  private fun stopX11() {
    X11Manager.stopServer(this)
    Toast.makeText(this, R.string.x11_stopped_hint, Toast.LENGTH_SHORT).show()
  }

  /** Install the X11 client environment (xterm + openbox + fonts) into the distro. */
  private fun installX11Environment() {
    Toast.makeText(this, R.string.x11_install_env_started, Toast.LENGTH_SHORT).show()
    runPackageManager(PackageAction.INSTALL, ProotManager.selectedDistro().x11Packages) { }
  }

  override fun onPause() {
    super.onPause()
    val tab = tabSwitcher.selectedTab as? NeoTab
    tab?.onPause()
  }

  override fun onResume() {
    super.onResume()
    PreferenceManager.getDefaultSharedPreferences(this)
      .registerOnSharedPreferenceChangeListener(this)
    tabSwitcher.addListener(object : TabSwitcherListener {
      override fun onSwitcherShown(tabSwitcher: TabSwitcher) {
        toolbar.setNavigationIcon(R.drawable.ic_add_box_white_24dp)
        toolbar.setNavigationOnClickListener(addSessionListener)
        toolbar.setBackgroundResource(android.R.color.transparent)
        toolbar.animate().alpha(0f).setDuration(300).withEndAction {
          toolbar.alpha = 1f
        }.start()
        updateTabDots()
      }

      override fun onSwitcherHidden(tabSwitcher: TabSwitcher) {
        toolbar.navigationIcon = null
        toolbar.setNavigationOnClickListener(null)
        // Match the title bar + system bars to the terminal background.
        applyTerminalSystemColors()
        updateTabDots()
        // Returned to a single terminal: focus it and raise the keyboard.
        raiseKeyboardForSelectedTab()
      }

      override fun onSelectionChanged(tabSwitcher: TabSwitcher, selectedTabIndex: Int, selectedTab: Tab?) {
        if (selectedTab is TermTab && selectedTab.termData.termSession != null) {
          NeoPreference.storeCurrentSession(selectedTab.termData.termSession!!)
        }
        updateTabDots()
      }

      override fun onTabAdded(tabSwitcher: TabSwitcher, index: Int, tab: Tab, animation: Animation) {
        update_colors()
        updateTabDots()
      }

      override fun onTabRemoved(tabSwitcher: TabSwitcher, index: Int, tab: Tab, animation: Animation) {
        if (tab is TermTab) {
          SessionRemover.removeSession(termService, tab)
        } else if (tab is XSessionTab) {
          SessionRemover.removeXSession(termService, tab)
        }
        updateTabDots()
      }

      override fun onAllTabsRemoved(tabSwitcher: TabSwitcher, tabs: Array<out Tab>, animation: Animation) {
      }
    })
    // Safe cast: after activity recreation (e.g. fold/unfold, theme change) the
    // TabSwitcher can restore plain Tab instances from saved state that are not
    // our NeoTab subclasses — a forced cast crashes onResume.
    val tab = tabSwitcher.selectedTab as? NeoTab
    tab?.onResume()
    // Match the title bar + system bars to the terminal background on every
    // resume (incl. the very first launch), so it applies without having to
    // change the color scheme.
    if (!tabSwitcher.isSwitcherShown) {
      applyTerminalSystemColors()
    }
    // Returning from recents/background: re-raise the keyboard for the terminal.
    raiseKeyboardForSelectedTab()
    maybeCheckForUpdate()
  }

  /**
   * Check GitHub for a newer release once per launch (and at most daily), and
   * offer to download + install it if one is available.
   */
  private fun maybeCheckForUpdate() {
    if (updateChecked) return
    updateChecked = true
    val prefs = PreferenceManager.getDefaultSharedPreferences(this)
    val now = System.currentTimeMillis()
    if (now - prefs.getLong(KEY_LAST_UPDATE_CHECK, 0L) < 24 * 60 * 60 * 1000L) {
      return
    }
    UpdateManager.checkForUpdate { info ->
      if (isFinishing) return@checkForUpdate
      prefs.edit().putLong(KEY_LAST_UPDATE_CHECK, now).apply()
      if (info != null && !tabSwitcher.isSwitcherShown) {
        AlertDialog.Builder(this)
          .setTitle(getString(R.string.update_available_title, info.tag))
          .setMessage(info.notes.ifBlank { getString(R.string.update_available_message) })
          .setPositiveButton(R.string.update_download_install) { _, _ -> startUpdate(info) }
          .setNeutralButton(R.string.update_open_releases) { _, _ -> UpdateManager.openReleasesPage(this) }
          .setNegativeButton(android.R.string.cancel, null)
          .show()
      }
    }
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

  override fun onStart() {
    super.onStart()
    EventBus.getDefault().register(this)
    val tab = tabSwitcher.selectedTab as? NeoTab
    tab?.onStart()
  }

  override fun onStop() {
    super.onStop()
    // After stopped, window locations may changed
    // Rebind it at next time.
    forEachTab<TermTab> { it.resetAutoCompleteStatus() }
    val tab = tabSwitcher.selectedTab as? NeoTab
    tab?.onStop()
    EventBus.getDefault().unregister(this)
  }

  override fun onDestroy() {
    super.onDestroy()
    if (instance === this) instance = null
    val tab = tabSwitcher.selectedTab as? NeoTab
    tab?.onDestroy()
    PreferenceManager.getDefaultSharedPreferences(this)
      .unregisterOnSharedPreferenceChangeListener(this)

    if (termService != null) {
      if (termService!!.sessions.isEmpty()) {
        termService!!.stopSelf()
      }
      termService = null
    }
    unbindService(this)
  }

  override fun onWindowFocusChanged(hasFocus: Boolean) {
    super.onWindowFocusChanged(hasFocus)
    val tab = tabSwitcher.selectedTab as? NeoTab
    tab?.onWindowFocusChanged(hasFocus)
    if (hasFocus) {
      raiseKeyboardForSelectedTab()
      // Re-measure + redraw the active terminal whenever the window regains
      // focus. This is exactly what makes returning from recents recover a
      // blank first session, so doing it here also covers the first launch.
      (tab as? TermTab)?.resetStatus()
    }
  }

  fun raiseKeyboard(view: View) {
    // Delay slightly so it runs after the window has settled (e.g. when
    // returning from recents), otherwise requestFocus/showSoftInput no-op.
    view.postDelayed({
      if (tabSwitcher.isSwitcherShown) return@postDelayed
      view.requestFocus()
      val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
      // Re-query the terminal's InputConnection so the IME adopts the terminal's
      // input config (word- vs char-based, no autocorrect, …). On first launch
      // the IME can connect before the session exists, so without this restart
      // the keyboard only adapts after returning from recents (which restarts
      // input for us).
      imm.restartInput(view)
      imm.showSoftInput(view, InputMethodManager.SHOW_IMPLICIT)
    }, 100)
  }

  private fun raiseKeyboardForSelectedTab() {
    val tab = tabSwitcher.selectedTab as? TermTab ?: return
    val view = tab.termData.termView ?: return
    raiseKeyboard(view)
  }

  /** The current terminal background color (from the active color scheme). */
  private fun currentTerminalBackgroundColor(): Int {
    return try {
      val scheme = ComponentManager.getComponent<ColorSchemeComponent>().getCurrentColorScheme()
      TerminalColors.parse(scheme.backgroundColor ?: "#14181c")
    } catch (e: Exception) {
      ContextCompat.getColor(this, R.color.terminal_background)
    }
  }

  /** The current terminal foreground color (from the active color scheme). */
  private fun currentTerminalForegroundColor(): Int {
    return try {
      val scheme = ComponentManager.getComponent<ColorSchemeComponent>().getCurrentColorScheme()
      TerminalColors.parse(scheme.foregroundColor ?: "#ffffff")
    } catch (e: Exception) {
      ContextCompat.getColor(this, android.R.color.white)
    }
  }

  /**
   * Refresh the page-dots indicator: one dot per tab, the active one
   * highlighted. Hidden while the switcher overview is shown (the cards already
   * show every tab) and for a single tab.
   */
  fun updateTabDots() {
    if (!::tabDots.isInitialized) {
      return
    }
    if (tabSwitcher.isSwitcherShown) {
      tabDots.visibility = View.GONE
      return
    }
    tabDots.setBaseColor(currentTerminalForegroundColor())
    tabDots.setBackgroundColor(currentTerminalBackgroundColor())
    tabDots.setTabs(tabSwitcher.count, tabSwitcher.selectedTabIndex)
  }

  /**
   * Match the title bar (toolbar) and the system status/navigation bars to the
   * terminal background color. The toolbar is only recolored while the switcher
   * overview is hidden (the overview keeps it transparent).
   */
  fun applyTerminalSystemColors() {
    val bg = currentTerminalBackgroundColor()
    if (!tabSwitcher.isSwitcherShown) {
      toolbar.setBackgroundColor(bg)
    }
    // Match the switcher container too, so tab switches don't flash a different
    // color behind the terminal.
    tabSwitcher.setBackgroundColor(bg)
    window.statusBarColor = bg
    window.navigationBarColor = bg

    // Use dark status/navigation icons on a light background and light icons on
    // a dark one, so they stay legible whatever color scheme is active.
    val lightBackground = ColorUtils.calculateLuminance(bg) > 0.5
    val lightBarFlags = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or
      View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
    var flags = window.decorView.systemUiVisibility
    flags = if (lightBackground) flags or lightBarFlags else flags and lightBarFlags.inv()
    window.decorView.systemUiVisibility = flags

    // Keep the page-dots strip in sync with the terminal colors.
    updateTabDots()
  }

  override fun onKeyDown(keyCode: Int, event: KeyEvent?): Boolean {
    when (keyCode) {
      KeyEvent.KEYCODE_BACK -> {
        if (event?.action == KeyEvent.ACTION_DOWN && tabSwitcher.isSwitcherShown && tabSwitcher.count > 0) {
          toggleSwitcher(showSwitcher = false, easterEgg = false)
          return true
        }
      }
      KeyEvent.KEYCODE_MENU -> {
        if (toolbar.isOverflowMenuShowing) {
          toolbar.hideOverflowMenu()
        } else {
          toolbar.showOverflowMenu()
        }
        return true
      }
    }
    return super.onKeyDown(keyCode, event)
  }

  override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
    when (requestCode) {
      NeoPermission.REQUEST_APP_PERMISSION -> {
        // If the mic permission was just granted, restart the audio bridge so
        // PulseAudio's AAudio source module loads (it failed to load at app
        // start when the permission wasn't held yet).
        val micGranted = permissions.indexOf(Manifest.permission.RECORD_AUDIO)
          .let { it >= 0 && grantResults.getOrNull(it) == PackageManager.PERMISSION_GRANTED }
        if (micGranted) {
          io.neoterm.utils.PulseAudioBridge.restart(this)
        }
        // Storage/notifications are best-effort: the rootfs lives in internal
        // storage, so the app still works if they are declined. Proceed with
        // startup regardless of the result instead of killing the app.
        onStartupPermissionsHandled()
        return
      }
    }
  }

  override fun onSharedPreferenceChanged(sharedPreferences: SharedPreferences?, key: String?) {
    when (key) {
      getString(R.string.key_ui_fullscreen) ->
        setFullScreenMode(NeoPreference.isFullScreenEnabled())

      getString(R.string.key_customization_color_scheme) -> {
        (tabSwitcher.selectedTab as? TermTab)?.updateColorScheme()
        applyTerminalSystemColors()
      }

      getString(R.string.key_general_microphone) -> {
        // Toggle the mic: when enabling, make sure RECORD_AUDIO is granted (the
        // grant callback restarts audio); otherwise restart now so PulseAudio
        // loads/unloads its AAudio source module to match the new setting.
        if (NeoPreference.isMicrophoneEnabled() &&
          ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO)
          != PackageManager.PERMISSION_GRANTED
        ) {
          ActivityCompat.requestPermissions(
            this, arrayOf(Manifest.permission.RECORD_AUDIO),
            NeoPermission.REQUEST_APP_PERMISSION
          )
        } else {
          io.neoterm.utils.PulseAudioBridge.restart(this)
        }
      }

      getString(R.string.key_ui_eks_enabled) -> {
        // Show/hide the extra keys on the open tabs immediately.
        val enabled = NeoPreference.isExtraKeysEnabled()
        forEachTab<TermTab> { tab ->
          (tab.termData.termSession as? ShellTermSession)?.shellProfile?.enableExtraKeys = enabled
          tab.termData.extraKeysView?.visibility = if (enabled) View.VISIBLE else View.GONE
          if (enabled) {
            tab.termData.viewClient?.updateExtraKeys(tab.termData.termSession?.title, true)
          }
        }
      }

      getString(R.string.key_general_bell) -> {
        // Apply the bell toggle to already-open sessions, not just new ones.
        val enabled = NeoPreference.isBellEnabled()
        forEachTab<TermTab> { tab ->
          (tab.termData.termSession as? ShellTermSession)?.shellProfile?.enableBell = enabled
        }
      }

      getString(R.string.key_general_vibrate) -> {
        // Apply the vibrate toggle to already-open sessions, not just new ones.
        val enabled = NeoPreference.isVibrateEnabled()
        forEachTab<TermTab> { tab ->
          (tab.termData.termSession as? ShellTermSession)?.shellProfile?.enableVibrate = enabled
        }
      }

      getString(R.string.key_general_volume_as_control) -> {
        // Volume-as-special-keys is read live from the profile, so update the
        // open sessions' profiles to make the toggle take effect immediately.
        val enabled = NeoPreference.isSpecialVolumeKeysEnabled()
        forEachTab<TermTab> { tab ->
          (tab.termData.termSession as? ShellTermSession)?.shellProfile?.enableSpecialVolumeKeys = enabled
        }
      }

      getString(R.string.key_generaL_backspace_map_to_esc) -> {
        // Back-key-to-Esc is read live from the profile; update open sessions.
        val enabled = NeoPreference.isBackButtonBeMappedToEscapeEnabled()
        forEachTab<TermTab> { tab ->
          (tab.termData.termSession as? ShellTermSession)?.shellProfile?.enableBackKeyToEscape = enabled
        }
      }
    }
  }

  override fun onServiceDisconnected(name: ComponentName?) {
    if (termService != null) {
      finish()
    }
  }

  override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
    termService = (service as NeoTermService.NeoTermBinder).service
    if (termService == null) {
      finish()
      return
    }

    serviceConnected = true
    maybeStartup()
  }

  override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
    when (requestCode) {
      REQUEST_SETUP -> {
        when (resultCode) {
          // onActivityResult runs before the activity is resumed and the tab
          // switcher is laid out, so creating the first session right here gives
          // it a zero-sized view and the terminal stays blank (just the
          // background). Defer it briefly so the rootfs that was just extracted
          // is settled and the content view is measured before the shell opens.
          AppCompatActivity.RESULT_OK ->
            tabSwitcher.postDelayed({ enterMain() }, FIRST_SESSION_DELAY_MS)
          AppCompatActivity.RESULT_CANCELED -> {
            setSystemShellMode(true)
            tabSwitcher.postDelayed({ forceAddSystemSession() }, FIRST_SESSION_DELAY_MS)
          }
        }
      }
    }
    super.onActivityResult(requestCode, resultCode, data)
  }

  override fun onConfigurationChanged(newConfig: Configuration) {
    super.onConfigurationChanged(newConfig)
    if (newConfig == null) {
      return
    }

    // When rotate the screen, extra keys may get updated.
    forEachTab<NeoTab> {
      it.onConfigurationChanged(newConfig)
      if (it is TermTab) {
        it.resetStatus()
      }
    }
  }

  private fun forceAddSystemSession() {
    // Open a system shell like any other session: no switcher reveal/animation,
    // routed through the normal add path so the current profile (extra keys,
    // etc.) applies.
    addNewSession(null, true)
  }

  private fun enterMain() {
    setSystemShellMode(false)

    if (!termService!!.sessions.isEmpty()) {
      val lastSession = getStoredCurrentSessionOrLast()

      for (session in termService!!.sessions) {
        addNewSessionFromExisting(session)
      }

      for (session in termService!!.xSessions) {
        addXSession(session)
      }

      if (intent?.action == Intent.ACTION_RUN) {
        // app shortcuts
        addNewSession(null, false)
      } else {
        switchToSession(lastSession)
      }

    } else {
      // Fore system shell mode to be disabled. No switcher reveal -> the first
      // session opens directly with the keyboard up.
      addNewSession(null, false)
    }

    // Make sure the active terminal actually renders. On first launch the tab's
    // view can be created before it gets a real size, so the emulator is never
    // built and the terminal stays blank until an unrelated relayout (returning
    // from recents). Nudge a layout pass and refresh, bounded so it can never
    // loop or hang.
    ensureSelectedTerminalRendered(0)
  }

  private fun ensureSelectedTerminalRendered(attempt: Int) {
    val view = (tabSwitcher.selectedTab as? TermTab)?.termData?.termView
    if (view != null && view.width > 0 && view.height > 0) {
      view.updateSize()
      view.onScreenUpdated()
      // The first session is now laid out: bind the keyboard to it (focus +
      // IME restart). onWindowFocusChanged fires before the first session
      // exists, so this is what makes the keyboard adapt to the terminal on
      // first launch instead of only after returning from recents.
      raiseKeyboardForSelectedTab()
      return
    }
    if (attempt < 12) {
      // What returning from recents effectively does: force a full layout pass.
      window.decorView.requestLayout()
      tabSwitcher.postDelayed({ ensureSelectedTerminalRendered(attempt + 1) }, 120)
    }
  }

  override fun recreate() {
    NeoPreference.store(KEY_NO_RESTORE, true)
    saveCurrentStatus()
    super.recreate()
  }

  private fun isRecreating(): Boolean {
    val result = peekRecreating()
    if (result) {
      NeoPreference.store(KEY_NO_RESTORE, !result)
    }
    return result
  }

  private fun saveCurrentStatus() {
    setSystemShellMode(getSystemShellMode())
  }

  private fun peekRecreating(): Boolean {
    return NeoPreference.loadBoolean(KEY_NO_RESTORE, false)
  }

  private fun setFullScreenMode(fullScreen: Boolean) {
    fullScreenHelper.fullScreen = fullScreen
    if (tabSwitcher.selectedTab is TermTab) {
      val tab = tabSwitcher.selectedTab as TermTab
      tab.requireHideIme()
      tab.onFullScreenModeChanged(fullScreen)
    }
    NeoPreference.store(R.string.key_ui_fullscreen, fullScreen)
    this@NeoTermActivity.recreate()
  }

  private fun showProfileDialog() {
    val profileComponent = ComponentManager.getComponent<ProfileComponent>()
    val profiles = profileComponent.getProfiles(ShellProfile.PROFILE_META_NAME)
    val profilesShell = profiles.filterIsInstance<ShellProfile>()

    if (profiles.isEmpty()) {
      AlertDialog.Builder(this)
        .setTitle(R.string.error)
        .setMessage(R.string.no_profile_available)
        .setPositiveButton(android.R.string.yes, null)
        .show()
      return
    }

    AlertDialog.Builder(this)
      .setTitle(R.string.new_session_with_profile)
      .setItems(profiles.map { it.profileName }.toTypedArray(), { dialog, which ->
        val selectedProfile = profilesShell[which]
        addNewSessionWithProfile(selectedProfile)
      })
      .setPositiveButton(android.R.string.no, null)
      .show()
  }

  private fun addNewSession() = addNewSessionWithProfile(ShellProfile.create())

  private fun addNewSession(sessionName: String?, systemShell: Boolean) =
    addNewSessionWithProfile(sessionName, systemShell, ShellProfile.create())

  private fun addNewSessionWithProfile(profile: ShellProfile) {
    // No switcher reveal: open the new session directly (no animation).
    addNewSessionWithProfile(null, getSystemShellMode(), profile)
  }

  private fun addNewSessionWithProfile(
    sessionName: String?, systemShell: Boolean,
    profile: ShellProfile
  ) {
    val sessionCallback = TermSessionCallback()
    val viewClient = TermViewClient(this)

    val parameter = ShellParameter()
      .callback(sessionCallback)
      .systemShell(systemShell)
      .profile(profile)
    val session = termService!!.createTermSession(parameter)

    session.mSessionName = sessionName ?: generateSessionName("NeoTerm")

    val tab = createTab(session.mSessionName) as TermTab
    tab.termData.initializeSessionWith(session, sessionCallback, viewClient)

    addNewTab(tab)
    switchToSession(tab)
  }

  private fun addNewSessionFromExisting(session: TerminalSession?) {
    if (session == null) {
      return
    }

    // Do not add the same session again
    // Or app will crash when rotate
    val tabCount = tabSwitcher.count
    (0..(tabCount - 1))
      .map { tabSwitcher.getTab(it) }
      .filter { it is TermTab && it.termData.termSession == session }
      .forEach { return }

    val sessionCallback = session.sessionChangedCallback as TermSessionCallback
    val viewClient = TermViewClient(this)

    val tab = createTab(session.title) as TermTab
    tab.termData.initializeSessionWith(session, sessionCallback, viewClient)

    addNewTab(tab)
    switchToSession(tab)
  }

  private fun addXSession() {
    if (!BuildConfig.DEBUG) {
      AlertDialog.Builder(this)
        .setTitle(R.string.error)
        .setMessage(R.string.sorry_for_development)
        .setPositiveButton(android.R.string.yes, null)
        .show()
      return
    }

    if (!tabSwitcher.isSwitcherShown) {
      toggleSwitcher(showSwitcher = true, easterEgg = false)
    }

    val parameter = XParameter()
    val session = termService!!.createXSession(this, parameter)

    session.mSessionName = generateXSessionName("X")
    val tab = createXTab(session.mSessionName) as XSessionTab
    tab.session = session

    addNewTab(tab)
    switchToSession(tab)
  }

  private fun addXSession(session: XSession?) {
    if (session == null) {
      return
    }

    // Do not add the same session again
    // Or app will crash when rotate
    val tabCount = tabSwitcher.count
    (0..(tabCount - 1))
      .map { tabSwitcher.getTab(it) }
      .filter { it is XSessionTab && it.session == session }
      .forEach { return }

    val tab = createXTab(session.mSessionName) as XSessionTab

    addNewTab(tab)
    switchToSession(tab)
  }

  private fun generateSessionName(prefix: String): String {
    return "$prefix #${termService!!.sessions.size}"
  }

  private fun generateXSessionName(prefix: String): String {
    return "$prefix #${termService!!.xSessions.size}"
  }

  private fun switchToSession(session: TerminalSession?) {
    if (session == null) {
      return
    }

    for (i in 0 until tabSwitcher.count) {
      val tab = tabSwitcher.getTab(i)
      if (tab is TermTab && tab.termData.termSession == session) {
        switchToSession(tab)
        break
      }
    }
  }

  private fun switchToSession(tab: Tab?) {
    if (tab == null) {
      return
    }
    tabSwitcher.selectTab(tab)
  }

  private fun addNewTab(tab: Tab) {
    // Add without animation and keep the switcher hidden: the tab is added and
    // then selected (switchToSession) instantly, with no reveal/swipe animation.
    tabSwitcher.addTab(tab, 0)
  }

  private fun getStoredCurrentSessionOrLast(): TerminalSession? {
    val stored = NeoPreference.getCurrentSession(termService)
    if (stored != null) return stored
    val numberOfSessions = termService!!.sessions.size
    if (numberOfSessions == 0) return null
    return termService!!.sessions[numberOfSessions - 1]
  }

  private fun createAddSessionListener(): View.OnClickListener {
    return View.OnClickListener {
      addNewSession()
    }
  }

  private fun createTab(tabTitle: String?): Tab {
    return postTabCreated(TermTab(tabTitle ?: "NeoTerm"))
  }

  private fun createXTab(tabTitle: String?): Tab {
    return postTabCreated(XSessionTab(tabTitle ?: "NeoTerm"))
  }

  private fun <T : NeoTab> postTabCreated(tab: T): T {
    // We must create a Bundle for each tab
    // tabs can use them to store status.
    tab.parameters = Bundle()

    // Use the terminal background for the tab card too, so switching tabs (and
    // the fade-in) doesn't flash a different color behind the terminal.
    tab.setBackgroundColor(currentTerminalBackgroundColor())
    tab.setTitleTextColor(currentTerminalForegroundColor())
    return tab
  }

  private fun createWindowInsetsListener(): OnApplyWindowInsetsListener {
    return OnApplyWindowInsetsListener { _, insets ->
      tabSwitcher.setPadding(
        insets.systemWindowInsetLeft,
        insets.systemWindowInsetTop, insets.systemWindowInsetRight,
        insets.systemWindowInsetBottom
      )
      insets
    }
  }

  private fun toggleSwitcher(showSwitcher: Boolean, easterEgg: Boolean) {
    if (tabSwitcher.count == 0 && easterEgg) {
      App.get().easterEgg(this, "Stop! You don't know what you are doing!")
      return
    }

    if (showSwitcher) {
      tabSwitcher.showSwitcher()
    } else {
      tabSwitcher.hideSwitcher()
    }
  }

  private fun setSystemShellMode(systemShell: Boolean) {
    NeoPreference.store(NeoPreference.KEY_SYSTEM_SHELL, systemShell)
  }

  private fun getSystemShellMode(): Boolean {
    return NeoPreference.loadBoolean(NeoPreference.KEY_SYSTEM_SHELL, true)
  }

  private inline fun <reified T> forEachTab(callback: (T) -> Unit) {
    (0 until tabSwitcher.count)
      .map { tabSwitcher.getTab(it) }
      .filterIsInstance(T::class.java)
      .forEach(callback)
  }

  @Suppress("unused")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onTabCloseEvent(tabCloseEvent: TabCloseEvent) {
    val tab = tabCloseEvent.termTab

    // Closing the last tab exits the app. Removing the tab cleans up its session
    // (onTabRemoved -> SessionRemover -> service.removeTermSession), and once no
    // sessions remain the service tears everything down: it finishes this
    // activity, closes the embedded X11 window and unbinds the X11 server. We do
    // NOT call X11Manager.stopServer here: it routes through
    // startService(ACTION_X11_STOP), which would re-create the just-stopped
    // service and bring its notification back with zero sessions.
    if (tabSwitcher.count <= 1) {
      tabSwitcher.removeTab(tab)
      finish()
      return
    }

    // Work out which neighbour to switch to *before* removing the closing tab.
    var index = tabSwitcher.indexOf(tab)
    if (NeoPreference.isNextTabEnabled()) {
      // 关闭当前窗口后，向下一个窗口切换
      if (--index < 0) index = tabSwitcher.count - 1
    } else {
      // 关闭当前窗口后，向上一个窗口切换
      if (++index >= tabSwitcher.count) index = 0
    }
    val target = tabSwitcher.getTab(index)

    // Select the neighbour first, then remove the closing tab. With the
    // switcher hidden, mrapp's removeTab only re-inflates the auto-selected
    // neighbour when its index differs from the previously selected index.
    // Closing the front tab (index 0, the common case) keeps the selected
    // index at 0, so the neighbour is never inflated and the terminal stays
    // blank until the next layout pass (which is why backgrounding/foreground
    // or opening the switcher "fixed" it). Selecting the neighbour up front
    // forces its view to inflate and show; the subsequent removeTab then just
    // drops the now-unselected closing tab.
    if (target !== tab) {
      switchToSession(target)
    }
    tabSwitcher.removeTab(tab)
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onToggleFullScreenEvent(toggleFullScreenEvent: ToggleFullScreenEvent) {
    val fullScreen = fullScreenHelper.fullScreen
    setFullScreenMode(!fullScreen)
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onToggleImeEvent(toggleImeEvent: ToggleImeEvent) {
    if (!tabSwitcher.isSwitcherShown) {
      val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
      imm.toggleSoftInput(InputMethodManager.SHOW_IMPLICIT, 0)
    }
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onTitleChangedEvent(titleChangedEvent: TitleChangedEvent) {
    if (!tabSwitcher.isSwitcherShown) {
      toolbar.title = titleChangedEvent.title
    }
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onCreateNewSessionEvent(createNewSessionEvent: CreateNewSessionEvent) {
    val cmd = createNewSessionEvent.initialCommand
    if (cmd.isNullOrEmpty()) {
      addNewSession()
    } else {
      // Open a fresh session that runs the given custom command on startup.
      val profile = ShellProfile.create().apply { initialCommand = cmd }
      addNewSessionWithProfile(profile)
    }
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onSwitchSessionEvent(switchSessionEvent: SwitchSessionEvent) {
    if (tabSwitcher.count < 2) {
      return
    }

    val rangedInt = RangedInt(tabSwitcher.selectedTabIndex, (0 until tabSwitcher.count))
    val nextIndex = if (switchSessionEvent.toNext) rangedInt.inc() else rangedInt.dec()
    // Switch directly without revealing the switcher overview, so there is no
    // tab-switch animation.
    switchToSession(tabSwitcher.getTab(nextIndex))
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onSwitchIndexedSessionEvent(switchIndexedSessionEvent: SwitchIndexedSessionEvent) {
    val nextIndex = switchIndexedSessionEvent.index - 1
    if (nextIndex in (0 until tabSwitcher.count) && nextIndex != tabSwitcher.selectedTabIndex) {
      // Do not show animation here, users may get tired
      switchToSession(tabSwitcher.getTab(nextIndex))
    }
  }

  fun update_colors() {
    // Simple fix to bug on custom color
    Handler().postDelayed({

      if (tabSwitcher.count > 0) {
        val tab = tabSwitcher.selectedTab
        if (tab is TermTab) {
          tab.updateColorScheme()
        }
      }
      // Keep the title bar + system bars in sync with the terminal background.
      applyTerminalSystemColors()

    }, 500)
  }

}
