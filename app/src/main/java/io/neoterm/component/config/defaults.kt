package io.neoterm.component.config

import android.annotation.SuppressLint

object DefaultValues {
  const val fontSize = 30

  const val enableBell = false
  const val enableVibrate = false
  const val enableExecveWrapper = true
  const val enableAutoCompletion = false
  const val enableFullScreen = false
  const val enableAutoHideToolbar = false
  const val enableSwitchNextTab = false
  const val enableExtraKeys = true
  const val enableExplicitExtraKeysWeight = false
  const val enableBackButtonBeMappedToEscape = false
  const val enableSpecialVolumeKeys = false
  const val enableMicrophone = false
  const val enableCamera = false
  const val cameraResolution = "1280x720"
  const val enableGps = false
  const val enableUsbSerial = false
  const val cursorStyle = "block"
  const val enableWordBasedIme = false

  // OSC-escape desktop notifications (OSC 9 / 99 kitty / 777 urxvt). Off by
  // default: any terminal output (incl. remote SSH) could otherwise post a
  // system notification. The sub-options only apply when the master is on.
  const val enableOscNotification = false
  const val enableOscNotificationSound = true
  const val enableOscNotificationUrgency = true

  const val loginShell = "bash"
  const val initialCommand = ""
  const val defaultFont = "SourceCodePro"

  // proot futtatókörnyezet — az átállás óta ez az alapértelmezett mód
  const val enableProot = true
  const val prootDistro = "ubuntu"
  // "proot" (no root) or "chroot" (offered on rooted devices at setup)
  const val runtimeMode = "proot"
}

object NeoTermPath {
  @SuppressLint("SdCardPath")
  const val ROOT_PATH = "/data/data/io.neoterm/files"
  const val USR_PATH = "$ROOT_PATH/usr"
  const val HOME_PATH = "$ROOT_PATH/home"
  const val APT_BIN_PATH = "$USR_PATH/bin/apt"
  const val LIB_PATH = "$USR_PATH/lib"

  const val CUSTOM_PATH = "$HOME_PATH/.neoterm"
  const val NEOTERM_LOGIN_SHELL_PATH = "$CUSTOM_PATH/shell"
  const val EKS_PATH = "$CUSTOM_PATH/eks"
  const val EKS_DEFAULT_FILE = "$EKS_PATH/default.nl"
  const val FONT_PATH = "$CUSTOM_PATH/font"
  const val COLORS_PATH = "$CUSTOM_PATH/color"
  const val USER_SCRIPT_PATH = "$CUSTOM_PATH/script"
  const val PROFILE_PATH = "$CUSTOM_PATH/profile"

  const val SOURCE_FILE = "$USR_PATH/etc/apt/sources.list"
  const val PACKAGE_LIST_DIR = "$USR_PATH/var/lib/apt/lists"

  // ── proot futtatókörnyezet ────────────────────────────────────────────
  // A proot bináris és a disztró-rootfs-ek a Termux-stílusú usr/-től külön
  // élnek, hogy a két modell ne keveredjen. A proot maga az app UID-jával
  // fut, ezért egy app-saját, írható tmp-könyvtárat kap.
  const val PROOT_ROOT_PATH = "$ROOT_PATH/proot"
  const val PROOT_BIN_PATH = "$PROOT_ROOT_PATH/proot"
  const val PROOT_TMP_PATH = "$PROOT_ROOT_PATH/tmp"
  const val ROOTFS_PATH = "$ROOT_PATH/rootfs"

  private const val SOURCE = "https://raw.githubusercontent.com/NeoTerm/NeoTerm-repo/main"

  // A proot bináris + disztró-rootfs-ek kiszolgáló base-URL-je: az app saját
  // GitHub Release-ei. A "latest/download" mindig a legutóbbi release-re mutat;
  // az asset-nevek laposak (a release-ek nem támogatnak alkönyvtárat):
  //   <base>/proot-<arch>
  //   <base>/rootfs-<distro>-<arch>.tar.xz
  private const val PROOT_SOURCE = "https://github.com/9hm2/NeoTerm-pr/releases/latest/download"

  val DEFAULT_MAIN_PACKAGE_SOURCE: String
  val DEFAULT_PROOT_SOURCE: String

  init {
    DEFAULT_MAIN_PACKAGE_SOURCE = SOURCE
    DEFAULT_PROOT_SOURCE = PROOT_SOURCE
  }
}
