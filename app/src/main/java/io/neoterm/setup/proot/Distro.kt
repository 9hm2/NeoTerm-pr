package io.neoterm.setup.proot

import io.neoterm.component.config.NeoTermPath

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
  val archiveExt: String
) {
  UBUNTU("ubuntu", "Ubuntu 24.04 LTS", "/bin/bash", arrayOf("--login"), "tar.gz"),
  ALPINE("alpine", "Alpine Linux 3.20", "/bin/ash", arrayOf("-l"), "tar.gz"),
  KALI("kali", "Kali Linux (rolling)", "/bin/bash", arrayOf("--login"), "tar.xz"),
  ARCH("arch", "Arch Linux", "/bin/bash", arrayOf("--login"), "tar.gz");

  /** A disztró kibontott rootfs-ének gyökere a hoston. */
  fun rootfsPath(): String = "${NeoTermPath.ROOTFS_PATH}/$id"

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
