package io.neoterm.setup.proot

import io.neoterm.App
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

  /**
   * Az APK-ba csomagolt proot bináris (jniLibs/<abi>/libproot.so), ha létezik.
   * A natív könyvtárakat az Android a telepítéskor a nativeLibraryDir-be bontja
   * ki futtatható joggal, így itt közvetlenül exec-elhető — letöltés nélkül.
   */
  fun bundledProotPath(): String? {
    val f = File(App.get().applicationInfo.nativeLibraryDir, "libproot.so")
    return if (f.canExecute()) f.absolutePath else null
  }

  /** A használandó proot bináris: elsőként a beépített, különben a letöltött. */
  fun prootBinaryPath(): String = bundledProotPath() ?: NeoTermPath.PROOT_BIN_PATH

  /** A proot bináris megvan és futtatható (beépített vagy letöltött). */
  fun isProotBinaryInstalled(): Boolean =
    bundledProotPath() != null || File(NeoTermPath.PROOT_BIN_PATH).canExecute()

  /** A megadott disztró rootfs-e telepítve van (van benne legalább /etc). */
  fun isRootfsInstalled(distro: Distro): Boolean =
    File("${distro.rootfsPath()}/etc").isDirectory

  /** Teljes proot mód kész: proot bináris + a kiválasztott disztró rootfs-e. */
  fun isInstalled(distro: Distro = selectedDistro()): Boolean =
    isProotBinaryInstalled() && isRootfsInstalled(distro)

  /** A guest-rootfs szokásos bináris-könyvtárai, ahol egy shell lehet. */
  private val GUEST_BIN_DIRS = listOf("/usr/bin", "/bin", "/usr/local/bin", "/sbin", "/usr/sbin")

  /**
   * Megkeresi a megadott shellt (pl. "zsh") a disztró rootfs-én belül, és
   * visszaadja a GUEST-oldali útvonalát (pl. "/usr/bin/zsh"), vagy null-t, ha
   * nincs telepítve. A szimbolikus linkeket is elfogadja (a merged-/usr
   * disztrókban a /bin → /usr/bin link miatt).
   */
  fun findShell(name: String, distro: Distro = selectedDistro()): String? {
    if (name.isEmpty()) return null
    val rootfs = distro.rootfsPath()
    for (dir in GUEST_BIN_DIRS) {
      val hostFile = File("$rootfs$dir/$name")
      if (hostFile.exists()) {
        return "$dir/$name"
      }
    }
    return null
  }

  /**
   * Csak azokat az archokat támogatjuk, amikhez van proot bináris: a beépített
   * libproot.so (arm64) vagy a letölthető aarch64. Más archon (armeabi-v7a,
   * x86_64) nincs proot, ezért a setup értelmes hibát ad vissza, nem 404-et.
   */
  fun isArchSupported(arch: String): Boolean =
    bundledProotPath() != null || arch == "aarch64"

  /** A megadott disztró rootfs-ének (és staging-jének) törlése. */
  fun uninstall(distro: Distro) {
    deleteRecursively(File(distro.rootfsPath()))
    deleteRecursively(File("${distro.rootfsPath()}-staging"))
  }

  private fun deleteRecursively(file: File) {
    if (!file.exists()) return
    if (file.isDirectory) file.listFiles()?.forEach { deleteRecursively(it) }
    file.delete()
  }

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
    extraEnv: List<String> = emptyList(),
    /** Ha nem üres, a login shell helyett ezt a parancsot futtatja `sh -c`-vel
     *  (pl. csomagkezelő-művelet), majd a session véget ér. */
    command: List<String> = emptyList()
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

    // Fake /proc fájlok (proot-distro sysdata mintájára): az Android korlátozott
    // /proc-ja miatt a ps/top/uptime/free hibára futna ("Unable to get system
    // boot time"). A /proc bind UTÁN kötjük, hogy a konkrétabb bind felülírja.
    ProotSysData.bindings().forEach { (fake, real) -> bind(args, fake, real) }

    // Külső tároló átkötése, ha elérhető — kényelmi /sdcard.
    System.getenv("EXTERNAL_STORAGE")?.let { ext ->
      if (File(ext).isDirectory) bind(args, ext, "/sdcard")
    }

    // X11: megosztott socket-könyvtár, hogy a guest GUI-appjai elérjék az
    // (Android-oldali) X-szervert a DISPLAY=:0-n. A könyvtárat itt hozzuk létre;
    // az X-szerver ide teszi a unix socketjét (pl. /tmp/.X11-unix/X0).
    val x11SocketDir = File("${NeoTermPath.PROOT_ROOT_PATH}/x11").apply { mkdirs() }
    bind(args, x11SocketDir.absolutePath, "/tmp/.X11-unix")

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
    // X11 / GUI: az Android-oldali X-szerver a :0 kijelzőn érhető el, az audio a
    // PulseAudio TCP-n. Ártalmatlan CLI-használatnál is (a nem-GUI appok nem
    // nyúlnak hozzá).
    args.add("DISPLAY=:0")
    args.add("PULSE_SERVER=127.0.0.1:4713")
    args.add("XDG_RUNTIME_DIR=/tmp")
    extraEnv.forEach { if (it.isNotEmpty()) args.add(it) }

    // A guest shell + login-kapcsoló(k), vagy egy konkrét parancs `sh -c`-vel.
    val shell = loginShell ?: distro.defaultShell
    args.add(shell)
    if (command.isEmpty()) {
      distro.loginArgs.forEach { args.add(it) }
    } else {
      args.add("-c")
      args.add(command.joinToString(" "))
    }

    return Launch(
      executable = prootBinaryPath(),
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
