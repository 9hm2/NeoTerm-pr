package io.neoterm.setup.proot

import io.neoterm.component.config.NeoTermPath

/** A package-manager művelet, disztrótól függetlenül. */
enum class PackageAction { UPDATE, UPGRADE, INSTALL, SEARCH }

/**
 * A proot futtatókörnyezet által támogatott Linux-disztrók.
 *
 * A séma a Claude-repo `proot/distros.json` manifesztjét tükrözi. Minden
 * disztró egy önálló rootfs-könyvtárba települ (`ROOTFS_PATH/<id>`), és van
 * egy alapértelmezett bejelentkező shellje (guest-útvonal) a hozzá tartozó
 * login-kapcsolóval.
 *
 * @author kiva
 */
enum class Distro(
  val id: String,
  val displayName: String,
  val defaultShell: String,
  val loginArgs: Array<String>,
  /**
   * A rootfs-archív tömörítése/kiterjesztése. A proot-distro mintájára az
   * upstream tarballt VÁLTOZATLANUL tükrözzük a release-be (nincs CI-újra-
   * csomagolás), így a kiterjesztés disztrónként eltér.
   */
  val archiveExt: String,
  /** A disztró csomagkezelője: "apt", "apk" vagy "pacman". */
  val packageManager: String
) {
  UBUNTU("ubuntu", "Ubuntu 24.04 LTS", "/bin/bash", arrayOf("--login"), "tar.gz", "apt"),
  ALPINE("alpine", "Alpine Linux 3.20", "/bin/ash", arrayOf("-l"), "tar.gz", "apk"),
  KALI("kali", "Kali Linux (rolling)", "/bin/bash", arrayOf("--login"), "tar.xz", "apt"),
  ARCH("arch", "Arch Linux", "/bin/bash", arrayOf("--login"), "tar.gz", "pacman");

  /**
   * X11 kliens-környezet csomagjai (a megfelelő csomagkezelőhöz). A natív
   * X-szerver a Termux:X11, ezért a disztróba csak kliensek + egy könnyű WM
   * (openbox) + betűk kellenek, NEM teljes X-szerver.
   */
  val x11Packages: String
    get() = when (packageManager) {
      // xkeyboard-config / xkb-data ships /usr/share/X11/xkb, which the embedded
      // X server needs (XKB_CONFIG_ROOT) or it refuses to start. The PulseAudio
      // *client* (libpulse + pactl/paplay) lets apps reach NeoTerm's Android-side
      // PulseAudio server on PULSE_SERVER=127.0.0.1:4713 — no distro server.
      // ffmpeg/libavcodec-extra provide browser media codecs (AAC/H.264).
      // `dbus` ships dbus-daemon so GUI apps get a session bus (see
      // ProotManager.ensureDbusSession); apt's dbus-x11 already pulls it in.
      "apk" -> "xterm openbox font-dejavu xrandr xkeyboard-config pulseaudio-utils ffmpeg dbus"
      "pacman" -> "xterm openbox xorg-xrandr ttf-dejavu xkeyboard-config libpulse ffmpeg dbus"
      else -> "xterm openbox x11-xserver-utils dbus-x11 fonts-dejavu xkb-data pulseaudio-utils libavcodec-extra"
    }

  /** A disztró kibontott rootfs-ének gyökere a hoston. */
  fun rootfsPath(): String = "${NeoTermPath.ROOTFS_PATH}/$id"

  /**
   * A megadott csomag-művelet parancsa a disztró saját csomagkezelőjével
   * (apt/apk/pacman). Az [arg] az érintett csomag neve (INSTALL/SEARCH esetén).
   */
  fun packageCommand(action: PackageAction, arg: String = ""): List<String> = when (packageManager) {
    "apk" -> when (action) {
      PackageAction.UPDATE -> listOf("apk", "update")
      PackageAction.UPGRADE -> listOf("apk", "upgrade")
      PackageAction.INSTALL -> listOf("apk", "add", arg)
      PackageAction.SEARCH -> listOf("apk", "search", "-v", arg)
    }
    "pacman" -> when (action) {
      PackageAction.UPDATE -> listOf("pacman", "-Sy", "--noconfirm")
      PackageAction.UPGRADE -> listOf("pacman", "-Syu", "--noconfirm")
      PackageAction.INSTALL -> listOf("pacman", "-S", "--noconfirm", arg)
      PackageAction.SEARCH -> listOf("pacman", "-Ss", arg)
    }
    else /* apt */ -> when (action) {
      PackageAction.UPDATE -> listOf("apt-get", "update")
      PackageAction.UPGRADE -> listOf("apt-get", "-y", "upgrade")
      PackageAction.INSTALL -> listOf("apt-get", "-y", "install", arg)
      PackageAction.SEARCH -> listOf("apt-cache", "search", arg)
    }
  }

  /**
   * A rootfs-tarball letöltési URL-je az adott archra. A GitHub Release
   * asset-nevek laposak: `<base>/rootfs-<id>-<arch>.<archiveExt>`
   */
  fun rootfsUrl(baseUrl: String, arch: String): String =
    "$baseUrl/rootfs-$id-$arch.$archiveExt"

  fun rootfsSha256Url(baseUrl: String, arch: String): String =
    "${rootfsUrl(baseUrl, arch)}.sha256"

  companion object {
    val DEFAULT = UBUNTU

    fun fromId(id: String?): Distro =
      values().firstOrNull { it.id == id } ?: DEFAULT

    /**
     * A proot bináris letöltési URL-je (fallback, ha nincs APK-ba csomagolva).
     * A proot disztró-független, csak az archtól függ.
     * Layout: `<base>/proot-<arch>`
     */
    fun prootUrl(baseUrl: String, arch: String): String =
      "$baseUrl/proot-$arch"
  }
}
