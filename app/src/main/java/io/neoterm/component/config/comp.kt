package io.neoterm.component.config

import android.content.Context
import android.content.SharedPreferences
import android.preference.PreferenceManager
import android.system.ErrnoException
import android.system.Os
import android.util.TypedValue
import io.neolang.frontend.ConfigVisitor
import io.neolang.frontend.NeoLangParser
import io.neoterm.App
import io.neoterm.R
import io.neoterm.backend.TerminalSession
import io.neoterm.component.NeoComponent
import io.neoterm.services.NeoTermService
import io.neoterm.setup.proot.ProotManager
import io.neoterm.utils.NLog
import java.io.File
import java.nio.file.Files

class ConfigureComponent : NeoComponent {
  override fun onServiceInit() {
  }

  override fun onServiceDestroy() {
  }

  override fun onServiceObtained() {
  }

  fun getLoaderVersion(): Int {
    return CONFIG_LOADER_VERSION
  }

  fun newLoader(configFile: File): IConfigureLoader {
    return when (configFile.extension) {
      "nl" -> NeoLangConfigureLoader(configFile)
      else -> OldConfigureLoader(configFile)
    }
  }

  companion object {
    private const val CONFIG_LOADER_VERSION = 20
  }
}

open class NeoConfigureFile(val configureFile: File) {
  private val configParser = NeoLangParser()
  protected open var configVisitor: ConfigVisitor? = null

  fun getVisitor() = configVisitor ?: throw IllegalStateException("Configure file not loaded or parse failed.")

  open fun parseConfigure() = kotlin.runCatching {
    val programCode = String(Files.readAllBytes(configureFile.toPath()))
    configParser.setInputSource(programCode)

    val ast = configParser.parse()
    val astVisitor = ast.visit().getVisitor(ConfigVisitor::class.java) ?: return false
    astVisitor.start()
    configVisitor = astVisitor.getCallback()
  }.isSuccess
}

object NeoPreference {
  const val KEY_HAPPY_EGG = "neoterm_fun_happy"
  const val KEY_FONT_SIZE = "neoterm_general_font_size"
  const val KEY_CURRENT_SESSION = "neoterm_service_current_session"
  const val KEY_SYSTEM_SHELL = "neoterm_core_system_shell"
  const val KEY_SOURCES = "neoterm_package_enabled_sources"
  const val KEY_PROOT_ENABLED = "neoterm_core_proot_enabled"
  const val KEY_PROOT_DISTRO = "neoterm_core_proot_distro"
  const val KEY_PROOT_SOURCE = "neoterm_core_proot_source"
  /** "proot" (default, no root) or "chroot" (rooted devices, real kernel access). */
  const val KEY_RUNTIME_MODE = "neoterm_core_runtime_mode"

  const val VALUE_HAPPY_EGG_TRIGGER = 8

  var MIN_FONT_SIZE: Int = 0
    private set
  var MAX_FONT_SIZE: Int = 0
    private set

  private var preference: SharedPreferences? = null

  fun init(context: Context) {
    preference = PreferenceManager.getDefaultSharedPreferences(context)

    // This is a bit arbitrary and sub-optimal. We want to give a sensible default for minimum font size
    // to prevent invisible text due to zoom be mistake:
    val dipInPixels = TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 1f, context.resources.displayMetrics)
    MIN_FONT_SIZE = (4f * dipInPixels).toInt()
    MAX_FONT_SIZE = 256

    // load apt source
    val sourceFile = File(NeoTermPath.SOURCE_FILE)
    kotlin.runCatching {
      Files.readAllBytes(sourceFile.toPath())?.let {
        val source = String(it).trim().trimEnd()
        val array = source.split(" ")
        if (array.size >= 2 && array[0] == "deb") {
          store(R.string.key_package_source, array[1])
        }
      }
    }
  }

  fun store(key: Int, value: Any) {
    store(App.get().getString(key), value)
  }

  fun store(key: String, value: Any) {
    when (value) {
      is Int -> preference!!.edit().putInt(key, value).apply()
      is String -> preference!!.edit().putString(key, value).apply()
      is Boolean -> preference!!.edit().putBoolean(key, value).apply()
    }
  }

  fun loadInt(key: Int, defaultValue: Int): Int {
    return loadInt(App.get().getString(key), defaultValue)
  }

  fun loadString(key: Int, defaultValue: String?): String {
    return loadString(App.get().getString(key), defaultValue)
  }

  fun loadBoolean(key: Int, defaultValue: Boolean): Boolean {
    return loadBoolean(App.get().getString(key), defaultValue)
  }

  fun loadInt(key: String?, defaultValue: Int): Int {
    return preference!!.getInt(key, defaultValue)
  }

  fun loadString(key: String?, defaultValue: String?): String {
    return preference!!.getString(key, defaultValue) ?: ""
  }

  fun loadBoolean(key: String?, defaultValue: Boolean): Boolean {
    return preference!!.getBoolean(key, defaultValue)
  }

  fun storeCurrentSession(session: TerminalSession) {
    preference!!.edit()
      .putString(KEY_CURRENT_SESSION, session.mHandle)
      .apply()
  }

  fun getCurrentSession(termService: NeoTermService?): TerminalSession? {
    val sessionHandle = PreferenceManager.getDefaultSharedPreferences(termService!!)
      .getString(KEY_CURRENT_SESSION, "")

    return termService.sessions
      .singleOrNull { it.mHandle == sessionHandle }
  }

  fun setLoginShellName(loginProgramName: String?): Boolean {
    if (loginProgramName == null) {
      return false
    }

    val loginProgramPath = findLoginProgram(loginProgramName) ?: return false

    store(R.string.key_general_shell, loginProgramName)
    symlinkLoginShell(loginProgramPath)
    return true
  }

  fun getLoginShellName(): String {
    return loadString(R.string.key_general_shell, DefaultValues.loginShell)
  }

  /**
   * Proot mód: valódi disztró-rootfs futtatása proot alatt a Termux-stílusú
   * natív bootstrap helyett. Az átállás óta ez az alapértelmezett.
   */
  fun isProotEnabled(): Boolean {
    return loadBoolean(KEY_PROOT_ENABLED, DefaultValues.enableProot)
  }

  fun setProotEnabled(enabled: Boolean) {
    store(KEY_PROOT_ENABLED, enabled)
  }

  /** A kiválasztott proot disztró azonosítója (ubuntu/alpine/kali/arch). */
  fun getProotDistro(): String {
    return loadString(KEY_PROOT_DISTRO, DefaultValues.prootDistro)
  }

  fun setProotDistro(distroId: String) {
    store(KEY_PROOT_DISTRO, distroId)
  }

  /** Runtime mode: "proot" (default) or "chroot" (rooted devices). */
  fun getRuntimeMode(): String = loadString(KEY_RUNTIME_MODE, DefaultValues.runtimeMode)

  fun setRuntimeMode(mode: String) = store(KEY_RUNTIME_MODE, mode)

  /** True when the distro is run via real-root chroot instead of proot. */
  fun isChroot(): Boolean = getRuntimeMode() == "chroot"

  /**
   * A proot bináris + rootfs-ek kiszolgáló base-URL-je. Alapból az app saját
   * GitHub Release-eire mutat ([NeoTermPath.DEFAULT_PROOT_SOURCE]); fork esetén
   * felülírható.
   */
  fun getProotSource(): String {
    return loadString(KEY_PROOT_SOURCE, NeoTermPath.DEFAULT_PROOT_SOURCE)
  }

  fun setProotSource(url: String) {
    store(KEY_PROOT_SOURCE, url)
  }

  fun getLoginShellPath(): String {
    val loginProgramName = getLoginShellName()

    val loginProgramPath = findLoginProgram(loginProgramName) ?: {
      setLoginShellName(DefaultValues.loginShell)
      "${NeoTermPath.USR_PATH}/bin/${DefaultValues.loginShell}"
    }()

    // The legacy login-shell symlink is only used by the Termux-style native
    // bootstrap. In proot mode the shell is launched directly via ProotManager,
    // and loginProgramPath is a guest path (e.g. /usr/bin/zsh) that doesn't
    // exist on the host — symlinking it would just dangle and throw EEXIST on
    // every session. So only maintain the symlink in non-proot mode.
    if (!isProotEnabled()) {
      val shell = File(NeoTermPath.NEOTERM_LOGIN_SHELL_PATH)
      if (!shell.exists()) {
        symlinkLoginShell(loginProgramPath)
      }
    }

    return loginProgramPath
  }

  fun validateFontSize(fontSize: Int): Int {
    return Math.max(MIN_FONT_SIZE, Math.min(fontSize, MAX_FONT_SIZE))
  }

  private fun symlinkLoginShell(loginProgramPath: String) {
    File(NeoTermPath.CUSTOM_PATH).mkdirs()
    try {
      // Remove any existing link first. File.exists() returns false for a
      // dangling symlink, so use Os.remove (ignoring ENOENT) which works
      // regardless of whether the target exists.
      runCatching { Os.remove(NeoTermPath.NEOTERM_LOGIN_SHELL_PATH) }
      Os.symlink(loginProgramPath, NeoTermPath.NEOTERM_LOGIN_SHELL_PATH)
      Os.chmod(NeoTermPath.NEOTERM_LOGIN_SHELL_PATH, 448 /* Decimal of 0700 */)
    } catch (e: ErrnoException) {
      NLog.e("Preference", "Failed to symlink login shell: ${e.localizedMessage}")
    }
  }

  fun findLoginProgram(loginProgramName: String): String? {
    // Proot módban a shellek a disztró rootfs-ében élnek (pl. /usr/bin/zsh),
    // nem a Termux-stílusú usr/bin-ben — ott kell keresni, különben a telepített
    // shellt (pl. zsh) tévesen "nincs telepítve"-ként jelzi.
    if (isProotEnabled()) {
      return ProotManager.findShell(loginProgramName)
    }
    val file = File("${NeoTermPath.USR_PATH}/bin", loginProgramName)
    return if (file.canExecute()) file.absolutePath else null
  }

  fun getFontSize(): Int {
    return loadInt(
      KEY_FONT_SIZE,
      DefaultValues.fontSize
    )
  }

  fun getInitialCommand(): String {
    return loadString(
      R.string.key_general_initial_command,
      DefaultValues.initialCommand
    )
  }

  fun isBellEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_bell,
      DefaultValues.enableBell
    )
  }

  fun isVibrateEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_vibrate,
      DefaultValues.enableVibrate
    )
  }

  fun isExecveWrapperEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_use_execve_wrapper,
      DefaultValues.enableExecveWrapper
    )
  }

  fun isSpecialVolumeKeysEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_volume_as_control,
      DefaultValues.enableSpecialVolumeKeys
    )
  }

  fun isMicrophoneEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_microphone,
      DefaultValues.enableMicrophone
    )
  }

  fun isCameraEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_camera,
      DefaultValues.enableCamera
    )
  }

  /** Requested camera capture resolution as "WIDTHxHEIGHT"; the bridge maps it to the closest
   *  size the camera actually supports. */
  fun getCameraResolution(): String {
    return loadString(R.string.key_general_camera_resolution, DefaultValues.cameraResolution)
  }

  /** When on (default), the V4L2 /dev/video0 stream is delivered in the camera's
   *  native landscape orientation (like a USB webcam). When off, frames are rotated
   *  upright to the phone's orientation (matching the HTTP MJPEG stream). */
  fun isCameraV4l2Landscape(): Boolean {
    return loadBoolean(R.string.key_general_camera_landscape, DefaultValues.cameraV4l2Landscape)
  }

  fun isGpsEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_gps,
      DefaultValues.enableGps
    )
  }

  /** When on, known USB-serial chips (FTDI/CP210x/CH34x/PL2303/CDC-ACM) are
   *  driven app-side by usb-serial-for-android and exposed to the distro as
   *  /dev/ttyUSB*. Runtime toggle — takes effect on the next device attach. */
  fun isUsbSerialEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_usb_serial,
      DefaultValues.enableUsbSerial
    )
  }

  /** When on, the device battery and motion/environment sensors are exposed to
   *  the distro as a fake /sys/class/power_supply and /sys/bus/iio/devices tree,
   *  so upower/acpi/iio_info and IIO-aware tools work. Runtime toggle. */
  fun isSensorsEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_sensors,
      DefaultValues.enableSensors
    )
  }

  /** When on, a connected USB pendrive is exposed to the distro as a raw block
   *  device /dev/uksd0 (SCSI Bulk-Only-Transport app-side), so dd, fdisk, parted,
   *  mkfs and fsck and any filesystem work with no root. Runtime toggle. */
  fun isUsbStorageEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_usb_storage,
      DefaultValues.enableUsbStorage
    )
  }

  /** When on, a connected USB Wi-Fi adapter is brought up app-side by the uKernel
   *  Wi-Fi framework (libukwifid.so over io.neoterm.wifi): the guest loads the
   *  vendor driver with `modprobe`, and `iw`/`wpa_supplicant` drive it. Chip
   *  drivers ship separately. Runtime toggle. */
  fun isUsbWifiEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_usb_wifi,
      DefaultValues.enableUsbWifi
    )
  }

  /** The user's default cursor shape: 0 = block, 1 = underline, 2 = bar (TerminalEmulator
   *  CURSOR_STYLE_*). Apps can still override it at runtime via DECSCUSR. */
  fun getCursorStyle(): Int {
    return when (loadString(R.string.key_general_cursor_style, DefaultValues.cursorStyle)) {
      "underline" -> 1
      "bar" -> 2
      else -> 0
    }
  }

  fun isOscNotificationEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_osc_notification,
      DefaultValues.enableOscNotification
    )
  }

  fun isOscNotificationSoundEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_osc_notification_sound,
      DefaultValues.enableOscNotificationSound
    )
  }

  fun isOscNotificationUrgencyEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_osc_notification_urgency,
      DefaultValues.enableOscNotificationUrgency
    )
  }

  fun isAutoCompletionEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_auto_completion,
      DefaultValues.enableAutoCompletion
    )
  }

  fun isBackButtonBeMappedToEscapeEnabled(): Boolean {
    return loadBoolean(
      R.string.key_generaL_backspace_map_to_esc,
      DefaultValues.enableBackButtonBeMappedToEscape
    )
  }

  fun isExtraKeysEnabled(): Boolean {
    return loadBoolean(
      R.string.key_ui_eks_enabled,
      DefaultValues.enableExtraKeys
    )
  }

  fun isExplicitExtraKeysWeightEnabled(): Boolean {
    return loadBoolean(
      R.string.key_ui_eks_weight_explicit,
      DefaultValues.enableExplicitExtraKeysWeight
    )
  }

  fun isFullScreenEnabled(): Boolean {
    return loadBoolean(
      R.string.key_ui_fullscreen,
      DefaultValues.enableFullScreen
    )
  }

  fun isHideToolbarEnabled(): Boolean {
    return loadBoolean(
      R.string.key_ui_hide_toolbar,
      DefaultValues.enableAutoHideToolbar
    )
  }

  fun isNextTabEnabled(): Boolean {
    return loadBoolean(
      R.string.key_ui_next_tab_anim,
      DefaultValues.enableSwitchNextTab
    )
  }

  fun isWordBasedImeEnabled(): Boolean {
    return loadBoolean(
      R.string.key_general_enable_word_based_ime,
      DefaultValues.enableWordBasedIme
    )
  }

  /**
   * Note: to show the running command / cwd in the terminal title bar, set in
   * the guest shell (the title is read from the OSC 0 escape we already parse):
   *   $ trap 'echo -ne "\e]0;${BASH_COMMAND%% *}\x07"' DEBUG
   *   $ PS1='$(echo -ne "\e]0;$PWD\x07")\$ '
   */
}
