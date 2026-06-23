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
   * A disztróban beállított locale (LANG), amit a guest shell kapjon. Mivel a
   * környezetet `env -i`-vel építjük, a LANG-ot sehonnan nem örökli, és a PAM
   * (ami valódi loginnál a /etc/default/locale-t olvassa) sem fut le — ezért mi
   * olvassuk ki a disztró saját konfigjából:
   *  - Debian/Ubuntu/Kali: /etc/default/locale
   *  - Arch:               /etc/locale.conf
   * Az első nemüres `LANG=...` sort használjuk; ha nincs, C.UTF-8 a fallback.
   */
  internal fun guestLang(distro: Distro = selectedDistro()): String {
    val rootfs = distro.rootfsPath()
    for (path in listOf("$rootfs/etc/default/locale", "$rootfs/etc/locale.conf")) {
      val f = File(path)
      if (!f.isFile) continue
      val lang = runCatching {
        f.readLines()
          .map { it.trim().removePrefix("export ").trim() }
          .firstOrNull { it.startsWith("LANG=") }
          ?.substringAfter("LANG=")
          ?.trim()
          ?.trim('"', '\'')
      }.getOrNull()
      if (!lang.isNullOrEmpty()) return lang
    }
    return "C.UTF-8"
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

    // Gondoskodunk róla, hogy a login shell betöltse a ~/.bashrc-t (lásd lent).
    ensureLoginSourcesBashrc(distro)

    // Guest-oldali kompat-shimek (systemctl/dmesg/journalctl/loginctl) a
    // rootfs /usr/local/bin-jébe — minden indításkor frissítve.
    ProotShims.install(rootfs)

    val args = ArrayList<String>(48)
    args.add("proot")
    args.add("--kill-on-exit")     // a teljes process-fa meghal a tracee után
    args.add("--link2symlink")     // hardlinkek → symlinkek (apt/dpkg kompat)
    // Emulate System V IPC (shmget/semget/msgget) in user space. Android kernels
    // disable SysV IPC, so apps that use it fail with ENOSYS ("Function not
    // implemented") — e.g. PostgreSQL's postmaster interlock segment. The Termux
    // proot fork implements this; harmless for apps that don't use SysV IPC.
    args.add("--sysvipc")
    args.add("-0")                 // fake root: a guest uid/gid 0-t lát
    args.add("-r"); args.add(rootfs)

    // Kernel- és pszeudo-fájlrendszerek átkötése a guestbe.
    bind(args, "/dev")
    bind(args, "/proc")
    bind(args, "/sys")
    // Hide Android's SELinux from the guest. Android mounts selinuxfs at
    // /sys/fs/selinux, so the distro's SELinux-aware tools (su / PAM, runuser,
    // login, sudo, cron, sshd) think SELinux is enabled and abort in libselinux's
    // AVC ("avc_context_to_sid_raw: Assertion `avc_running' failed"). Masking it
    // with an empty dir (no `enforce`) makes is_selinux_enabled() false, so they
    // skip SELinux entirely. Bound after /sys so it overrides.
    val selinuxMask = File("${NeoTermPath.PROOT_ROOT_PATH}/selinux-mask").apply { mkdirs() }
    bind(args, selinuxMask.absolutePath, "/sys/fs/selinux")
    bind(args, "/dev/pts")
    bind(args, "/proc/self/fd", "/dev/fd")
    bind(args, "/proc/self/fd/0", "/dev/stdin")
    bind(args, "/proc/self/fd/1", "/dev/stdout")
    bind(args, "/proc/self/fd/2", "/dev/stderr")
    // Android's /dev has no shm; browsers and many apps need a writable
    // /dev/shm (POSIX shared memory) or their child processes crash.
    val shmDir = File("${NeoTermPath.PROOT_ROOT_PATH}/shm").apply { mkdirs() }
    bind(args, shmDir.absolutePath, "/dev/shm")
    bind(args, "/dev/urandom", "/dev/random")
    // Writable /dev/kmsg buffer: Android blocks the real kernel ring buffer, so
    // bind a regular file the guest can write to and our dmesg shim reads back.
    // The proot patch forces O_APPEND on this path (kernel-ring-buffer semantics).
    val kmsgBuf = File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata").apply { mkdirs() }.let { File(it, "kmsg") }
    runCatching {
      if (!kmsgBuf.exists()) {
        kmsgBuf.writeText("")
      } else if (kmsgBuf.length() > 256 * 1024) {
        // O_APPEND-del a puffer nőne; induláskor sapkázzuk az utolsó ~64 KB-ra.
        val all = kmsgBuf.readBytes()
        kmsgBuf.writeBytes(all.copyOfRange((all.size - 64 * 1024).coerceAtLeast(0), all.size))
      }
    }
    bind(args, kmsgBuf.absolutePath, "/dev/kmsg")

    // USB-serial bridge: bind each active adapter's PTY slave onto /dev/ttyUSB*
    // (the chip is driven app-side by usb-serial-for-android). Bound per launch;
    // plug the adapter before opening the session (or open a new tab) to see it.
    io.neoterm.setup.usbserial.UsbSerialBridge.bindings().forEach { (slave, tty) ->
      if (File(slave).exists()) bind(args, slave, tty)
    }

    // Fake /proc fájlok (proot-distro sysdata mintájára): az Android korlátozott
    // /proc-ja miatt a ps/top/uptime/free hibára futna ("Unable to get system
    // boot time"). A /proc bind UTÁN kötjük, hogy a konkrétabb bind felülírja.
    ProotSysData.bindings().forEach { (fake, real) -> bind(args, fake, real) }

    // Külső tároló átkötése, ha elérhető — kényelmi /sdcard.
    System.getenv("EXTERNAL_STORAGE")?.let { ext ->
      if (File(ext).isDirectory) bind(args, ext, "/sdcard")
    }

    // X11: megosztott socket-könyvtár, hogy a guest GUI-appjai elérjék az
    // (Android-oldali) X-szervert a DISPLAY=:0-n. A beágyazott X-szerver a
    // TMPDIR=${PROOT_ROOT_PATH}/x11 alatt hozza létre a unix socketjét
    // (.X11-unix/X0), ezért pontosan azt a könyvtárat kötjük a guest
    // /tmp/.X11-unix-jára (lásd X11Manager).
    val x11SocketDir = File("${NeoTermPath.PROOT_ROOT_PATH}/x11/.X11-unix").apply { mkdirs() }
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
    // Honor the distro's configured locale instead of forcing C.UTF-8: we build
    // the guest env with `env -i`, so nothing inherits LANG and PAM (which reads
    // /etc/default/locale on a real login) never runs. Read it ourselves.
    args.add("LANG=${guestLang(distro)}")
    args.add("PS1=\\u@neoterm:\\w\\$ ")
    args.add("TMPDIR=/tmp")
    // Export the login shell so GUI terminals (xterm) launched under X11 start
    // it instead of falling back to /bin/sh (which shows the bash PS1 literally).
    args.add("SHELL=${loginShell ?: distro.defaultShell}")
    // X11 / GUI: az Android-oldali X-szerver a :0 kijelzőn érhető el, az audio a
    // PulseAudio TCP-n. Ártalmatlan CLI-használatnál is (a nem-GUI appok nem
    // nyúlnak hozzá).
    args.add("DISPLAY=:0")
    args.add("PULSE_SERVER=127.0.0.1:4713")
    // Default recording source: NeoTerm's Android-side microphone (AAudio
    // input). Only exported when the user enabled the microphone in Settings —
    // then the source module is loaded and RECORD_AUDIO has been requested.
    if (NeoPreference.isMicrophoneEnabled()) {
      args.add("PULSE_SOURCE=neoterm_mic")
    }
    // Camera: NeoTerm's Android-side MJPEG bridge (CameraBridge). Only exported when the user
    // enabled the camera in Settings — then the bridge serves the stream and CAMERA was
    // requested. URL-aware apps (ffmpeg/OpenCV/mpv) read it; it is not a /dev/video0 device.
    if (NeoPreference.isCameraEnabled()) {
      args.add("NEOTERM_CAMERA_URL=http://127.0.0.1:4715/video.mjpeg")
    }
    // GPS: NeoTerm provides a built-in gpsd on 127.0.0.1:2947 (GpsBridge speaks the gpsd client
    // protocol), so distro clients (cgps, gpspipe, …) work with no gpsd installed — they default
    // to localhost:2947, no env needed. Export a hint for scripts when enabled.
    if (NeoPreference.isGpsEnabled()) {
      args.add("NEOTERM_GPSD=127.0.0.1:2947")
    }
    args.add("XDG_RUNTIME_DIR=/tmp")
    // Firefox's content sandbox can't work under proot (ptrace + no user
    // namespaces) and SIGSEGVs its child processes; disable it so the browser
    // is usable. RDD is Firefox's media-decoder process — its sandbox blocks
    // libavcodec/ffmpeg from loading under proot ("no decoder found" for AAC/
    // H.264), so disable that too. (Chromium needs --no-sandbox on the cmdline.)
    args.add("MOZ_DISABLE_CONTENT_SANDBOX=1")
    args.add("MOZ_DISABLE_RDD_SANDBOX=1")
    // Chromium's sandbox can't work under proot — Debian/Kali's launcher honors
    // CHROMIUM_FLAGS, so it runs without needing --no-sandbox by hand.
    args.add("CHROMIUM_FLAGS=--no-sandbox --disable-gpu")
    // No xdg-desktop-portal in the container; without this many GTK apps stall
    // or crash trying to reach it.
    args.add("GTK_USE_PORTAL=0")
    args.add("NO_AT_BRIDGE=1")
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
   * Login shells (`bash --login`) read ~/.bash_profile/.profile, NOT ~/.bashrc.
   * On some distros (e.g. Kali's root) nothing sources ~/.bashrc for a login
   * shell, so PATH additions appended there — like `~/.local/bin` for tools
   * installed via pip/npm (claude, etc.) — never take effect ("command not
   * found"). Drop in a minimal ~/.bash_profile that loads ~/.bashrc, but only
   * when the user has neither ~/.bash_profile nor ~/.bash_login (so we never
   * clobber an explicit user setup). Idempotent and cheap (a couple of stats).
   */
  internal fun ensureLoginSourcesBashrc(distro: Distro) {
    if (!distro.defaultShell.endsWith("bash")) return
    runCatching {
      val home = File(distro.rootfsPath(), "root")
      if (!home.isDirectory) return
      if (File(home, ".bash_profile").exists() || File(home, ".bash_login").exists()) return
      File(home, ".bash_profile").writeText(
        "# Created by NeoTerm: load ~/.bashrc in login shells so PATH additions\n" +
          "# (e.g. ~/.local/bin for pip/npm/claude installs) take effect.\n" +
          "if [ -f \"\$HOME/.bashrc\" ]; then . \"\$HOME/.bashrc\"; fi\n"
      )
    }
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
