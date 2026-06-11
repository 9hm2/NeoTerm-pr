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
import androidx.viewpager2.widget.ViewPager2
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

    /** How far (in real-count blocks) the virtual position may drift from the
     * middle before we silently re-seat it. With LOOP_COUNT = 100_000 the runway
     * is tens of thousands of blocks, so this is just hygiene against pathologically
     * long single-direction swiping; re-seating sooner keeps us far from the ends. */
    private const val RECENTER_DRIFT_BLOCKS = 1000

    /** Live instance, so the service's "Exit" action can finish the activity
     * (and remove its task) — otherwise the still-bound/recents activity would
     * re-create the service and its notification right after Exit. Cleared in
     * onDestroy, so it holds no reference past the activity's lifetime. */
    @Volatile
    private var instance: NeoTermActivity? = null

    fun getInstance(): NeoTermActivity? = instance
  }

  // The pager replaces the old chrome-tabs TabSwitcher: one page per open
  // session, with adjacent pages laid out & live (offscreenPageLimit = 1) so a
  // horizontal drag peeks the destination tab's real content and snaps back if
  // the swipe is not committed.
  lateinit var viewPager: ViewPager2
  private lateinit var pagerAdapter: TerminalPagerAdapter
  /** The model the adapter renders: the open tabs, terminal and X, in order. */
  private val tabs = ArrayList<NeoTab>()

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

  // ---- Pager model helpers (mirror the old TabSwitcher accessors) ----
  //
  // In infinite/circular mode (realCount >= 3) the pager's currentItem is a
  // *virtual* position; the real tab it shows is currentItem % realCount. Every
  // user-facing index below goes through realIndex(...) so it keeps meaning the
  // real tab whether or not looping is active. selectedVirtualIndex is the raw
  // pager position (used when we need to set/animate the pager itself).

  /** The real tab index the pager is currently showing. */
  private val selectedTabIndex: Int
    get() = pagerAdapter.realIndex(viewPager.currentItem)

  /** The raw (possibly virtual) pager position. */
  private val selectedVirtualIndex: Int
    get() = viewPager.currentItem

  /** The currently visible tab (or null when there are no tabs). */
  private val selectedTab: NeoTab?
    get() = tabs.getOrNull(selectedTabIndex)

  private val tabCount: Int
    get() = tabs.size

  private fun getTab(index: Int): NeoTab? = tabs.getOrNull(index)

  private fun indexOf(tab: NeoTab): Int = tabs.indexOf(tab)

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
        val tab = selectedTab
        if (tab is TermTab) {
          // isShow -> toolbarHide
          toggleToolbar(tab.toolbar, !isShow)
          // The keyboard changed the usable height; re-measure once it settles so
          // the terminal fills the new visible area exactly (no leftover gap).
          scheduleTerminalRemeasure()
        }
      }
    })

    // tabDots is bound later from the app-bar action view (onCreateOptionsMenu).

    viewPager = findViewById(R.id.terminal_pager)
    pagerAdapter = TerminalPagerAdapter(this, tabs)
    viewPager.adapter = pagerAdapter
    // Keep the neighbours laid out & LIVE so the user can peek the adjacent
    // tab's real terminal content while dragging (this is what makes the swipe
    // a live page rather than a snapshot).
    viewPager.offscreenPageLimit = 1
    ViewCompat.setOnApplyWindowInsetsListener(viewPager, createWindowInsetsListener())
    viewPager.registerOnPageChangeCallback(pageChangeCallback)

    val serviceIntent = Intent(this, NeoTermService::class.java)
    startService(serviceIntent)
    bindService(serviceIntent, this, 0)

    // Ask for the runtime permissions up front, before any setup runs.
    requestStartupPermissions()
  }

  /**
   * Reacts to the user paging to another tab: persist the new current session,
   * update the title + dots, and raise the keyboard for the now-active terminal.
   */
  private val pageChangeCallback = object : ViewPager2.OnPageChangeCallback() {
    override fun onPageSelected(position: Int) {
      // In looping mode [position] is a virtual position; resolve the real tab.
      when (val tab = tabs.getOrNull(pagerAdapter.realIndex(position))) {
        is TermTab -> {
          toolbar.visibility = View.VISIBLE
          tab.termData.termSession?.let { NeoPreference.storeCurrentSession(it) }
          toolbar.title = tab.termData.termSession?.title ?: tab.title
        }
        is XSessionTab -> {
          // The X (SDL) surface owns the whole screen; hide the toolbar like the
          // old decorator did.
          toolbar.visibility = View.GONE
        }
        else -> {}
      }
      updateTabDots()
      applyTerminalSystemColors()
      raiseKeyboardForSelectedTab()
    }

    override fun onPageScrollStateChanged(state: Int) {
      // Once a swipe settles, re-center the virtual position back to the middle
      // block so the user can never drift toward the ends of the virtual range.
      // Done only while idle so we never jump mid-drag/mid-settle.
      if (state == ViewPager2.SCROLL_STATE_IDLE) {
        recenterIfNeeded()
      }
    }
  }

  /**
   * In looping mode, jump (without animation) from the current virtual position
   * to the equivalent position in the middle block of the virtual range, so
   * there is always a huge runway of pages on both sides. The jump is invisible
   * because the destination renders the same real tab (position % realCount is
   * unchanged). No-op in finite mode or when already comfortably centered.
   */
  private fun recenterIfNeeded() {
    if (!pagerAdapter.isLooping) return
    val current = viewPager.currentItem
    val realIndex = pagerAdapter.realIndex(current)
    val centered = pagerAdapter.startPosition(realIndex)
    // Only re-seat if we've drifted a long way from center, to avoid churn.
    val driftBlocks = Math.abs(current - centered) / pagerAdapter.realCount
    if (driftBlocks >= RECENTER_DRIFT_BLOCKS) {
      viewPager.setCurrentItem(centered, false)
    }
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

    // The old tab-count button is replaced by a page-indicator (dots) action
    // view in the app bar — just shows which tab is active, no overview popup.
    val dotsView = menu?.findItem(R.id.toggle_tab_switcher_menu_item)?.actionView
      ?.findViewById<TabDotsIndicator>(R.id.toolbar_tab_dots)
    if (dotsView != null) {
      tabDots = dotsView
      updateTabDots()
    }
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
      R.id.menu_item_close_session -> {
        // Close the active tab (its session is cleaned up in removeTabAt).
        closeTab(selectedTab)
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
    selectedTab?.onPause()
  }

  override fun onResume() {
    super.onResume()
    PreferenceManager.getDefaultSharedPreferences(this)
      .registerOnSharedPreferenceChangeListener(this)
    selectedTab?.onResume()
    // Match the title bar + system bars to the terminal background on every
    // resume (incl. the very first launch), so it applies without having to
    // change the color scheme.
    applyTerminalSystemColors()
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
      if (info != null) {
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
    selectedTab?.onStart()
  }

  override fun onStop() {
    super.onStop()
    // After stopped, window locations may changed
    // Rebind it at next time.
    forEachTab<TermTab> { it.resetAutoCompleteStatus() }
    selectedTab?.onStop()
    EventBus.getDefault().unregister(this)
  }

  override fun onDestroy() {
    super.onDestroy()
    if (instance === this) instance = null
    selectedTab?.onDestroy()
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
    val tab = selectedTab
    tab?.onWindowFocusChanged(hasFocus)
    if (hasFocus) {
      raiseKeyboardForSelectedTab()
      // Re-measure + redraw the active terminal whenever the window regains
      // focus. This is exactly what makes returning from recents recover a
      // blank first session, so doing it here also covers the first launch.
      (tab as? TermTab)?.resetStatus()
      // ...and again once the layout (insets, keyboard, recents animation)
      // settles, so the row count matches the final visible height instead of a
      // transient one (which would leave the terminal shorter than the view).
      scheduleTerminalRemeasure()
    }
  }

  private val remeasureRunnable = Runnable { (selectedTab as? TermTab)?.resetStatus() }

  /**
   * Re-measure the active terminal once the layout settles, so the emulator's
   * row/column count always matches the final visible size after keyboard or
   * recents transitions (otherwise the terminal can be left shorter than the
   * view, which the user would have to clear by toggling the keyboard).
   *
   * Debounced: any pending pass is cancelled and re-posted, so calling this on
   * every inset/animation frame stays cheap. updateSize() is itself a no-op when
   * the size hasn't changed, so it never causes a spurious SIGWINCH/redraw.
   */
  private fun scheduleTerminalRemeasure() {
    val view = (selectedTab as? TermTab)?.termData?.termView ?: return
    view.removeCallbacks(remeasureRunnable)
    view.postDelayed(remeasureRunnable, 120)
    view.postDelayed(remeasureRunnable, 350)
  }

  fun raiseKeyboard(view: View) {
    // Delay slightly so it runs after the window has settled (e.g. when
    // returning from recents), otherwise requestFocus/showSoftInput no-op.
    view.postDelayed({
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
    val tab = selectedTab as? TermTab ?: return
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
   * highlighted (hidden for a single tab).
   */
  fun updateTabDots() {
    if (!::tabDots.isInitialized) {
      return
    }
    // Dots live in the (terminal-colored) app bar, so colour them from the
    // terminal foreground; the toolbar provides the background.
    tabDots.setBaseColor(currentTerminalForegroundColor())
    // Always one dot per REAL tab, with the real (mapped) index selected — the
    // virtual looping positions must never leak into the indicator.
    tabDots.setTabs(tabCount, selectedTabIndex)
  }

  /**
   * Match the title bar (toolbar) and the system status/navigation bars to the
   * terminal background color.
   */
  fun applyTerminalSystemColors() {
    val bg = currentTerminalBackgroundColor()
    toolbar.setBackgroundColor(bg)
    // Match the pager container too, so tab switches don't flash a different
    // color behind the terminal.
    viewPager.setBackgroundColor(bg)
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
        (selectedTab as? TermTab)?.updateColorScheme()
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
          // onActivityResult runs before the activity is resumed and the pager
          // is laid out, so creating the first session right here gives it a
          // zero-sized view and the terminal stays blank (just the background).
          // Defer it briefly so the rootfs that was just extracted is settled
          // and the content view is measured before the shell opens.
          AppCompatActivity.RESULT_OK ->
            viewPager.postDelayed({ enterMain() }, FIRST_SESSION_DELAY_MS)
          AppCompatActivity.RESULT_CANCELED -> {
            setSystemShellMode(true)
            viewPager.postDelayed({ forceAddSystemSession() }, FIRST_SESSION_DELAY_MS)
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
    // Open a system shell like any other session, routed through the normal add
    // path so the current profile (extra keys, etc.) applies.
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
      // Force system shell mode to be disabled. The first session opens directly
      // with the keyboard up.
      addNewSession(null, false)
    }

    // Make sure the active terminal actually renders. On first launch the page's
    // view can be created before it gets a real size, so the emulator is never
    // built and the terminal stays blank until an unrelated relayout (returning
    // from recents). Nudge a layout pass and refresh, bounded so it can never
    // loop or hang.
    ensureSelectedTerminalRendered(0)
  }

  private fun ensureSelectedTerminalRendered(attempt: Int) {
    val view = (selectedTab as? TermTab)?.termData?.termView
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
      viewPager.postDelayed({ ensureSelectedTerminalRendered(attempt + 1) }, 120)
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
    val tab = selectedTab
    if (tab is TermTab) {
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
    switchToTab(tab)
  }

  private fun addNewSessionFromExisting(session: TerminalSession?) {
    if (session == null) {
      return
    }

    // Do not add the same session again (e.g. when re-entering after rotation).
    if (tabs.any { it is TermTab && it.termData.termSession == session }) {
      return
    }

    val sessionCallback = session.sessionChangedCallback as TermSessionCallback
    val viewClient = TermViewClient(this)

    val tab = createTab(session.title) as TermTab
    tab.termData.initializeSessionWith(session, sessionCallback, viewClient)

    addNewTab(tab)
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

    val parameter = XParameter()
    val session = termService!!.createXSession(this, parameter)

    session.mSessionName = generateXSessionName("X")
    val tab = createXTab(session.mSessionName) as XSessionTab
    tab.session = session

    addNewTab(tab)
    switchToTab(tab)
  }

  private fun addXSession(session: XSession?) {
    if (session == null) {
      return
    }

    // Do not add the same session again.
    if (tabs.any { it is XSessionTab && it.session == session }) {
      return
    }

    val tab = createXTab(session.mSessionName) as XSessionTab
    tab.session = session

    addNewTab(tab)
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
    val index = tabs.indexOfFirst { it is TermTab && it.termData.termSession == session }
    if (index >= 0) {
      // Map the real index to a centered virtual position when looping.
      viewPager.setCurrentItem(pagerAdapter.startPosition(index), false)
    }
  }

  private fun switchToTab(tab: NeoTab?) {
    if (tab == null) {
      return
    }
    val index = indexOf(tab)
    if (index >= 0) {
      // Animate to the newly added (or selected) page so the switch is visible
      // when it is not the immediate neighbour; ViewPager2 still snaps cleanly.
      // In looping mode, animating across the whole virtual range would scroll
      // thousands of pages, so step to the nearest virtual position of the
      // target real tab instead (and only animate when it's a near neighbour).
      val target = nearestVirtualPositionFor(index)
      val animate = Math.abs(target - selectedVirtualIndex) <= 1
      viewPager.setCurrentItem(target, animate)
    }
  }

  /**
   * The virtual pager position for real-tab [realIndex] that is closest to the
   * current position (so a switch never scrolls across the whole virtual range).
   * In finite mode this is just [realIndex].
   */
  private fun nearestVirtualPositionFor(realIndex: Int): Int {
    if (!pagerAdapter.isLooping) return realIndex
    val realCount = pagerAdapter.realCount
    val current = viewPager.currentItem
    val currentReal = pagerAdapter.realIndex(current)
    // Signed delta in [-(realCount-1) .. realCount-1] taking the shorter way
    // around the ring.
    var delta = realIndex - currentReal
    if (delta > realCount / 2) delta -= realCount
    if (delta < -realCount / 2) delta += realCount
    return current + delta
  }

  /**
   * Append a tab to the model and notify the adapter. The page is created lazily
   * by ViewPager2; offscreenPageLimit keeps the neighbours live.
   *
   * Adding a tab can flip the adapter between finite and looping mode (e.g. the
   * 2->3 transition turns looping ON, changing the item count from realCount to
   * LOOP_COUNT). When the mode flips, a plain notifyItemInserted is wrong (the
   * whole index space changed), so we rebuild and re-seat the current position.
   */
  private fun addNewTab(tab: NeoTab) {
    val wasLooping = pagerAdapter.isLooping
    // Remember which real tab is currently shown so we can keep it selected.
    val currentReal = selectedTabIndex.coerceIn(0, (tabCount - 1).coerceAtLeast(0))
    tabs.add(tab)
    // In looping mode getItemCount() stays LOOP_COUNT but every position's real
    // mapping shifts, so a positional notify would desync the adapter from its
    // (unchanged) item count. A finite<->looping flip also rewrites the whole
    // index space. In both cases rebuild + re-seat; only a pure finite insert is
    // a simple notifyItemInserted.
    if (pagerAdapter.isLooping || wasLooping) {
      reseatPagerKeepingReal(currentReal)
    } else {
      pagerAdapter.notifyItemInserted(tabs.size - 1)
    }
    update_colors()
    updateTabDots()
  }

  /**
   * Rebuild the pager after a finite<->looping mode flip and place the pager on
   * [realIndex]'s (centered, when looping) virtual position without animation.
   * notifyDataSetChanged is used because the entire virtual index space changed
   * meaning, not just one item.
   */
  private fun reseatPagerKeepingReal(realIndex: Int) {
    pagerAdapter.notifyDataSetChanged()
    val target = pagerAdapter.startPosition(realIndex.coerceIn(0, (tabCount - 1).coerceAtLeast(0)))
    viewPager.setCurrentItem(target, false)
  }

  /**
   * Close a tab: remove its session (cleanup via SessionRemover) and drop the
   * page. Closing the last tab exits the app, matching the old behaviour.
   */
  private fun closeTab(tab: NeoTab?) {
    if (tab == null) return
    val index = indexOf(tab)
    if (index < 0) return

    // Closing the last tab exits the app (the service tears everything down once
    // no sessions remain — see onTabCloseEvent's note).
    if (tabCount <= 1) {
      removeTabAt(index)
      finish()
      return
    }

    // Work out which neighbour to focus *before* removing the tab, honouring the
    // next/previous preference (matches the old close behaviour). This is a REAL
    // index into [tabs].
    var target = index
    if (NeoPreference.isNextTabEnabled()) {
      if (--target < 0) target = tabCount - 1
    } else {
      if (++target >= tabCount) target = 0
    }
    val targetTab = getTab(target)

    removeTabAt(index)

    // After removal the indices shifted; select the tab we picked by identity,
    // resolved to a (centered, when looping) virtual position. removeTabAt may
    // have flipped looping off (3->2 tabs) and already re-seated the pager; in
    // that case we still re-point at the intended real tab.
    val newReal = if (targetTab != null) indexOf(targetTab)
      else index.coerceAtMost(tabCount - 1)
    if (newReal >= 0) {
      viewPager.setCurrentItem(pagerAdapter.startPosition(newReal), false)
    }
    updateTabDots()
  }

  /**
   * Remove the tab at real [index], cleaning up its underlying session.
   *
   * Removing can flip the adapter from looping to finite (the 3->2 transition).
   * When it does, the virtual index space collapses to the real one, so we
   * rebuild the pager (keeping whatever real tab is current) rather than emitting
   * a plain notifyItemRemoved against an index space that no longer exists.
   */
  private fun removeTabAt(index: Int) {
    val tab = tabs.getOrNull(index) ?: return
    val wasLooping = pagerAdapter.isLooping
    // The real tab that will remain selected (best-effort) after the removal, so
    // a mode flip can re-seat onto it.
    val currentReal = selectedTabIndex
    when (tab) {
      is TermTab -> SessionRemover.removeSession(termService, tab)
      is XSessionTab -> SessionRemover.removeXSession(termService, tab)
    }
    tabs.removeAt(index)
    // Mirror addNewTab: a positional notifyItemRemoved is only valid for a pure
    // finite removal. In looping mode the item count stays LOOP_COUNT while every
    // position's real mapping shifts, and a looping->finite flip (3->2 tabs)
    // rewrites the whole index space — both need a rebuild + re-seat. Keep the
    // current real tab (shifted left if it sat after the removed one); closeTab
    // re-points to its chosen neighbour afterwards.
    if (pagerAdapter.isLooping || wasLooping) {
      val keepReal = (if (currentReal > index) currentReal - 1 else currentReal)
        .coerceIn(0, (tabCount - 1).coerceAtLeast(0))
      reseatPagerKeepingReal(keepReal)
    } else {
      pagerAdapter.notifyItemRemoved(index)
    }
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

  private fun createTab(tabTitle: String?): NeoTab {
    return postTabCreated(TermTab(tabTitle ?: "NeoTerm"))
  }

  private fun createXTab(tabTitle: String?): NeoTab {
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
      viewPager.setPadding(
        insets.systemWindowInsetLeft,
        insets.systemWindowInsetTop, insets.systemWindowInsetRight,
        insets.systemWindowInsetBottom
      )
      // The bottom inset (keyboard) just changed the usable height — even small
      // changes (suggestion bar, keyboard layout switches) that the keyboard
      // show/hide listener's threshold misses. Re-measure once the new padding is
      // laid out so the terminal always fills the visible area (no leftover gap
      // that the user would otherwise have to clear by toggling the keyboard).
      scheduleTerminalRemeasure()
      insets
    }
  }

  private fun setSystemShellMode(systemShell: Boolean) {
    NeoPreference.store(NeoPreference.KEY_SYSTEM_SHELL, systemShell)
  }

  private fun getSystemShellMode(): Boolean {
    return NeoPreference.loadBoolean(NeoPreference.KEY_SYSTEM_SHELL, true)
  }

  private inline fun <reified T> forEachTab(callback: (T) -> Unit) {
    tabs.filterIsInstance<T>().forEach(callback)
  }

  @Suppress("unused")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onTabCloseEvent(tabCloseEvent: TabCloseEvent) {
    val tab = tabCloseEvent.termTab

    // Closing the last tab exits the app. Removing the tab cleans up its session
    // (SessionRemover -> service.removeTermSession), and once no sessions remain
    // the service tears everything down: it finishes this activity, closes the
    // embedded X11 window and unbinds the X11 server. We do NOT call
    // X11Manager.stopServer here: it routes through
    // startService(ACTION_X11_STOP), which would re-create the just-stopped
    // service and bring its notification back with zero sessions.
    closeTab(tab)
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
    val imm = getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager
    imm.toggleSoftInput(InputMethodManager.SHOW_IMPLICIT, 0)
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onTitleChangedEvent(titleChangedEvent: TitleChangedEvent) {
    // Keep the toolbar title bound to the *active* page only, so a background
    // tab's title change (e.g. its shell prompt) doesn't clobber the visible
    // title. The active tab's own title-changed event always matches its tab
    // title (requireUpdateTitle set it just before posting).
    val tab = selectedTab as? TermTab ?: return
    if (tab.title == titleChangedEvent.title) {
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
    if (tabCount < 2) {
      return
    }

    // Step one position in the requested direction. In looping mode just move
    // the virtual position by +/-1 (the pager wraps via the modulo mapping); in
    // finite mode wrap the real index manually as before.
    if (pagerAdapter.isLooping) {
      val delta = if (switchSessionEvent.toNext) 1 else -1
      viewPager.setCurrentItem(selectedVirtualIndex + delta, true)
    } else {
      val rangedInt = RangedInt(selectedTabIndex, (0 until tabCount))
      val nextIndex = if (switchSessionEvent.toNext) rangedInt.inc() else rangedInt.dec()
      viewPager.setCurrentItem(nextIndex, true)
    }
  }

  @Suppress("unused", "UNUSED_PARAMETER")
  @Subscribe(threadMode = ThreadMode.MAIN)
  fun onSwitchIndexedSessionEvent(switchIndexedSessionEvent: SwitchIndexedSessionEvent) {
    val nextIndex = switchIndexedSessionEvent.index - 1
    if (nextIndex in (0 until tabCount) && nextIndex != selectedTabIndex) {
      // [nextIndex] is a REAL tab index; resolve to the nearest virtual position
      // when looping so we don't scroll across the whole virtual range.
      viewPager.setCurrentItem(nearestVirtualPositionFor(nextIndex), true)
    }
  }

  fun update_colors() {
    // Simple fix to bug on custom color
    Handler().postDelayed({

      if (tabCount > 0) {
        val tab = selectedTab
        if (tab is TermTab) {
          tab.updateColorScheme()
        }
      }
      // Keep the title bar + system bars in sync with the terminal background.
      applyTerminalSystemColors()

    }, 500)
  }

}
