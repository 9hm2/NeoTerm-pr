package io.neoterm.setup.proot

import io.neoterm.component.config.NeoTermPath
import io.neoterm.component.config.NeoPreference
import java.io.File

/**
 * A proot futtatókörnyezet központi belépési pontja.
 *
 * - eldönti, hogy a proot mód telepítve van-e ([isInstalled]),
 * - összeállítja azt a parancssort, amellyel a natív [io.neoterm.backend.JNI]
 *   réteg a bejelentkező shellt egy disztró-rootfs-ben futtatja ([buildLaunch]).
 *
 * A proot maga az app UID-jával fut (a beágyazott loader miatt nincs külső
 * `PROOT_LOADER` fájl); a guest oldalon `-0` révén root (uid/gid 0) látszik.
 *
 * @author kiva
 */
object ProotManager {

  /**
   * Egy proot-os indítás összes natív paramétere.
   *
   * @param executable a hoston futtatandó program (a proot bináris)
   * @param args       a teljes argv (args[0] = "proot", a végén a guest shell)
   * @param env        a proot processz (host-oldali) környezete
   * @param hostCwd    host-oldali munkakönyvtár, ahova a natív réteg chdir-el
   */
  class Launch(
    val executable: String,
    val args: Array<String>,
    val env: Array<String>,
    val hostCwd: String
  )

  fun selectedDistro(): Distro = Distro.fromId(NeoPreference.getProotDistro())

  /** A proot bináris megvan és futtatható. */
  fun isProotBinaryInstalled(): Boolean =
    File(NeoTermPath.PROOT_BIN_PATH).canExecute()

  /** A megadott disztró rootfs-e telepítve van (van benne legalább /etc). */
  fun isRootfsInstalled(distro: Distro): Boolean =
    File("${distro.rootfsPath()}/etc").isDirectory

  /** Teljes proot mód kész: proot bináris + a kiválasztott disztró rootfs-e. */
  fun isInstalled(distro: Distro = selectedDistro()): Boolean =
    isProotBinaryInstalled() && isRootfsInstalled(distro)

  /**
   * Összeállítja a proot indítási parancssorát.
   *
   * @param loginShell guest-oldali shell útvonal; ha null, a disztró
   *                   alapértelmezett shellje (pl. /bin/bash, /bin/ash)
   * @param guestCwd   guest-oldali munkakönyvtár (default: /root)
   * @param extraEnv   a guest `env -i`-be befűzendő extra változók
   */
  fun buildLaunch(
    distro: Distro = selectedDistro(),
    loginShell: String? = null,
    guestCwd: String = "/root",
    extraEnv: List<String> = emptyList()
  ): Launch {
    val rootfs = distro.rootfsPath()

    // A proot a saját, írható tmp-könyvtárát igényli; gondoskodunk róla, hogy
    // létezzen (a setup is létrehozza, de futásidőben is biztosítjuk).
    File(NeoTermPath.PROOT_TMP_PATH).mkdirs()

    val args = ArrayList<String>(48)
    args.add("proot")
    args.add("--kill-on-exit")     // a teljes process-fa meghal a tracee után
    args.add("--link2symlink")     // hardlinkek → symlinkek (apt/dpkg kompat)
    args.add("-0")                 // fake root: a guest uid/gid 0-t lát
    args.add("-r"); args.add(rootfs)

    // Kernel- és pszeudo-fájlrendszerek átkötése a guestbe.
    bind(args, "/dev")
    bind(args, "/proc")
    bind(args, "/sys")
    bind(args, "/dev/pts")
    bind(args, "/proc/self/fd", "/dev/fd")
    bind(args, "/proc/self/fd/0", "/dev/stdin")
    bind(args, "/proc/self/fd/1", "/dev/stdout")
    bind(args, "/proc/self/fd/2", "/dev/stderr")
    bind(args, "/dev/urandom", "/dev/random")

    // Külső tároló átkötése, ha elérhető — kényelmi /sdcard.
    System.getenv("EXTERNAL_STORAGE")?.let { ext ->
      if (File(ext).isDirectory) bind(args, ext, "/sdcard")
    }

    args.add("-w"); args.add(guestCwd)

    // A guest környezetét tiszta lappal (`env -i`) építjük, hogy a host
    // (Android) változói ne szivárogjanak be.
    args.add("/usr/bin/env")
    args.add("-i")
    args.add("HOME=/root")
    args.add("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
    args.add("TERM=xterm-256color")
    args.add("COLORTERM=truecolor")
    args.add("LANG=C.UTF-8")
    args.add("PS1=\\u@neoterm:\\w\\$ ")
    args.add("TMPDIR=/tmp")
    extraEnv.forEach { if (it.isNotEmpty()) args.add(it) }

    // A guest shell + login-kapcsoló(k).
    val shell = loginShell ?: distro.defaultShell
    args.add(shell)
    distro.loginArgs.forEach { args.add(it) }

    return Launch(
      executable = NeoTermPath.PROOT_BIN_PATH,
      args = args.toTypedArray(),
      env = buildHostEnv(),
      hostCwd = NeoTermPath.PROOT_ROOT_PATH
    )
  }

  private fun bind(args: ArrayList<String>, host: String, guest: String? = null) {
    args.add("-b")
    args.add(if (guest == null) host else "$host:$guest")
  }

  /**
   * A proot bináris (host-oldali) környezete. A proot statikusan linkelt, így
   * minimális env elég; a fontos a `PROOT_TMP_DIR`, amit egy app-saját,
   * írható könyvtárra állítunk.
   */
  private fun buildHostEnv(): Array<String> {
    return listOf(
      "PROOT_TMP_DIR=${NeoTermPath.PROOT_TMP_PATH}",
      "HOME=${NeoTermPath.HOME_PATH}",
      "TERM=xterm-256color",
      "PATH=/system/bin:/system/xbin",
      "ANDROID_ROOT=" + (System.getenv("ANDROID_ROOT") ?: "/system"),
      "ANDROID_DATA=" + (System.getenv("ANDROID_DATA") ?: "/data")
    ).toTypedArray()
  }
}
