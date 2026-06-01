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
  const val enableWordBasedIme = false

  const val loginShell = "bash"
  const val initialCommand = ""
  const val defaultFont = "SourceCodePro"

  // proot futtatókörnyezet — az átállás óta ez az alapértelmezett mód
  const val enableProot = true
  const val prootDistro = "ubuntu"
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

  // A proot bináris + disztró-rootfs-ek kiszolgáló base-URL-je. A layoutot
  // a Claude-repo proot/ rendszere állítja elő:
  //   <base>/proot/<arch>/proot
  //   <base>/rootfs/<distro>/<arch>.tar.xz
  private const val PROOT_SOURCE = "https://raw.githubusercontent.com/NeoTerm/NeoTerm-proot/main"

  val DEFAULT_MAIN_PACKAGE_SOURCE: String
  val DEFAULT_PROOT_SOURCE: String

  init {
    DEFAULT_MAIN_PACKAGE_SOURCE = SOURCE
    DEFAULT_PROOT_SOURCE = PROOT_SOURCE
  }
}
