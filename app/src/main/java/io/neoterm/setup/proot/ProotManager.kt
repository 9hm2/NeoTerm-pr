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

    // D-Bus session bus a GUI appoknak (rpi-imager, GTK/Qt eszközök, értesítések).
    ensureDbusSession(distro)

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
    // Enable the hidden_files extension (-H). It hides ".proot." files (we have
    // none — ownership is in xattrs), and crucially it carries our getdents
    // injection that makes the virtual /dev/ttyUSB* hotplug ports show up in
    // directory listings (ls /dev/ttyUSB*, pyserial's /dev/ttyUSB* glob).
    args.add("-H")
    args.add("-0")                 // fake root: a guest uid/gid 0-t lát
    args.add("-r"); args.add(rootfs)

    // Kernel- és pszeudo-fájlrendszerek átkötése a guestbe.
    bind(args, "/dev")
    bind(args, "/proc")
    bind(args, "/sys")
    // App-adatkönyvtár átkötése a guestbe AZONOS abszolút úton, hogy a proot-ból
    // ugyanazokat a fájlokat lássuk, amelyeket az app-oldali daemonok használnak:
    //  - a ukwifid/ukfsd logok (files/ukwifid.log),
    //  - a daemon abszolút útjai (pl. UK_WIFI_MODDIR=.../rootfs/<distro>/lib/ukwifi,
    //    UK_WIFI_FW_DIR=.../lib/firmware) — így a `modprobe` által a daemonba dlopen-
    //    elt .so helye a guestből is ellenőrizhető/írható, és a hibakeresés a guestből
    //    megy. A guest amúgy is az app uid-jával fut, ezért ez nem új jogosultság, csak
    //    láthatóság. Android: /data/data/<pkg> és /data/user/0/<pkg> ugyanaz (symlink),
    //    mindkét nevet kötjük, hogy bármelyik abszolút út feloldódjon.
    runCatching {
      val dataDir = io.neoterm.App.get().applicationInfo.dataDir   // /data/.../io.neoterm
      if (!dataDir.isNullOrEmpty() && File(dataDir).isDirectory) {
        val pkg = io.neoterm.App.get().packageName
        bind(args, dataDir, "/data/data/$pkg")
        bind(args, dataDir, "/data/user/0/$pkg")
      }
    }
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

    // USB storage: an empty marker file bound onto /dev/uksd0 makes it an openable,
    // seekable, ls-able node; the proot block proxy (enter.c, UK_BLOCK) overrides its
    // I/O with SCSI to the app-side BlockBridge. Only when the toggle is on.
    if (NeoPreference.isUsbStorageEnabled()) {
      val sysdata = File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata").apply { mkdirs() }
      val uksd = File(sysdata, "uksd0")
      if (!uksd.exists()) runCatching { uksd.writeText("") }
      bind(args, uksd.absolutePath, "/dev/uksd0")
      // Partition nodes /dev/uksd0p1..p4: a multi-partition device (Raspberry Pi
      // card: FAT boot + ext4 root) is mounted per-partition. The redirect routes
      // a mount of /dev/uksd0pN to ukfsd on io.neoterm.fs.pN (one daemon each, see
      // FsBridge). Bind empty markers so the guest `mount /dev/uksd0pN` resolves;
      // probing a non-existent partition simply yields a failed mount.
      for (n in 1..8) {
        val part = File(sysdata, "uksd0p$n")
        if (!part.exists()) runCatching { part.writeText("") }
        bind(args, part.absolutePath, "/dev/uksd0p$n")
      }
      // Loop devices: mount a DOWNLOADED IMAGE file like on a real Linux PC
      // (`losetup -fP disk.img; mount /dev/loop0p2 …` or `mount -o loop[,offset=N]
      // -t ext4 disk.img …`). The redirect emulates the loop ioctls (no kernel loop
      // on Android) and backs each loop with the image's host path. Bind markers for
      // /dev/loop-control, the loop devices and their partition nodes so the guest's
      // losetup/mount can open them; the redirect intercepts the ioctls/mounts.
      val mk = { rel: String ->
        val f = File(sysdata, rel.removePrefix("/dev/"))
        if (!f.exists()) runCatching { f.writeText("") }
        bind(args, f.absolutePath, rel)
      }
      mk("/dev/loop-control")
      for (n in 0..7) {
        mk("/dev/loop$n")
        for (p in 1..4) mk("/dev/loop${n}p$p")
      }
      // FUSE: an empty marker bound onto /dev/fuse. A guest libfuse daemon
      // (sshfs/rclone/gocryptfs/ntfs-3g/AppImage) open()s it; the proot redirect
      // (UK_FS) turns that open into a socket()+connect() to an in-tracer channel
      // driven by the `fused` engine, and routes mount(...,"fuse",...,"fd=N") plus
      // the mountpoint's path ops through it. No kernel FUSE / root needed.
      mk("/dev/fuse")
    }

    // USB-serial: /dev/ttyUSB* are VIRTUAL hotplug ports, not static binds — the
    // proot open-redirect (enter.c) maps them to the live PTY at open time. Just
    // make sure the app-side control/redirect server is up before the guest opens
    // them. A device can be (un)plugged any time during the session.
    io.neoterm.setup.usbserial.UsbSerialBridge.ensureReady()
    // USB storage: a pendrive is a VIRTUAL block device /dev/uksd0 — the proot
    // block proxy (enter.c) routes its I/O to the app-side SCSI bridge. Bring the
    // control server up; the device may be (un)plugged any time.
    io.neoterm.setup.usbserial.BlockBridge.ensureReady()
    // USB storage filesystem: launch the ukfsd daemon (io.neoterm.fs), the real
    // FS engine that mounts /dev/uksd0 (sectors over io.neoterm.block) and serves
    // the proot VFS-redirect (UK_FS). Without it, mount(/dev/uksd0) does nothing.
    io.neoterm.setup.usbserial.FsBridge.ensureReady()
    // USB Wi-Fi: launch the ukwifid daemon (@io.neoterm.wifi) — the uKernel Wi-Fi
    // framework. Chip-agnostic, no driver; the guest brings a chip up with
    // `modprobe` (routed by the UK_WIFI redirect to the daemon, which dlopens the
    // vendor .so and drives the chip over io.neoterm.usb). Only with the toggle.
    io.neoterm.utils.UsbWifiBridge.ensureReady()
    // Bind a writable fake /sys/class/tty so the guest can readdir it (Android
    // SELinux blocks the real one) — ls / pyserial enumerate the ports there.
    io.neoterm.setup.usbserial.UsbSerialBridge.sysfsBind()?.let { bind(args, it, "/sys/class/tty") }

    // Sensors + battery: bind the fake /sys/class/power_supply and /sys/bus/iio
    // trees (Android SELinux blocks the real /sys readdir), so upower/acpi/iio_info
    // and IIO-aware tools see the device sensors.
    io.neoterm.utils.SensorBridge.sysfsBinds().forEach { (host, guest) -> bind(args, host, guest) }

    // USB storage: bind the fake /sys/block + /sys/dev/block trees so lsblk and
    // disk GUIs (rpi-imager, gnome-disks, gparted) enumerate /dev/uksd0[pN]
    // (Android SELinux blocks the real /sys/dev/block). Empty until a drive attaches.
    io.neoterm.setup.usbserial.BlockSysfsBridge.sysfsBinds().forEach { (host, guest) -> bind(args, host, guest) }

    // USB host: a readable /sys/bus overlay + a fake /sys/bus/usb (populated by
    // UsbSysfsBridge from the Android UsbManager) so the distro's UNMODIFIED
    // libusb/libudev enumerate USB devices (lsusb, pyusb, …) — no patched libusb,
    // no preload. libudev readdir()s /sys/bus to find bus names before scanning
    // /sys/bus/usb/devices, and the real /sys/bus is EACCES for the app uid, so
    // the overlay (which also carries the sensor bridge's iio) is what makes the
    // scan find anything. Paired with SYSTEMD_DEVICE_VERIFY_SYSFS=0 + UK_USB.
    io.neoterm.utils.UsbSysfsBridge.sysfsBinds().forEach { (host, guest) -> bind(args, host, guest) }
    // Phase 2: empty /dev/bus/usb/BBB/DDD markers; the UK_USB shim turns an open()
    // of one into a proxied handle on the app's real usbfs fd (control/bulk/URBs),
    // so stock libusb's usbfs path works — lsusb -v, pyusb, libftdi, rtl-sdr, …
    io.neoterm.utils.UsbSysfsBridge.devfsBinds().forEach { (host, guest) -> bind(args, host, guest) }
    runCatching { io.neoterm.utils.UsbBridge.refreshSysfs() }   // fill from current UsbManager state
    // USB Wi-Fi: overlay /sys/class/net + /sys/class/ieee80211 (the daemon fills
    // them when a chip's driver comes up). Gated by the toggle — overlaying hides
    // the (app-inaccessible) real netdevs, so only do it when Wi-Fi is wanted.
    if (NeoPreference.isUsbWifiEnabled()) {
      io.neoterm.utils.UsbWifiSysfsBridge.sysfsBinds().forEach { (host, guest) -> bind(args, host, guest) }
    }

    // Camera: expose a REAL /dev/video0 V4L2 node (proot cam shim, UK_CAM). An empty
    // marker becomes the device; the shim (uknl_cam_redirect.c) proxies its V4L2
    // ioctls/read/mmap to CameraBridge over io.neoterm.camera, so OpenCV/ffmpeg/
    // GStreamer/cheese open it natively (not just URL-aware apps). A fake
    // /sys/dev/char/81:0/uevent (DEVNAME=video0) lets v4l2-ctl/libv4l classify it —
    // bound via the parent dir since proot's -b can't take a colon in the guest path.
    if (NeoPreference.isCameraEnabled()) {
      val sysdata = File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata").apply { mkdirs() }
      val video = File(sysdata, "video0")
      if (!video.exists()) runCatching { video.writeText("") }
      bind(args, video.absolutePath, "/dev/video0")
      val charDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-dev-char").apply { mkdirs() }
      val ue = File(charDir, "81:0").apply { mkdirs() }.let { File(it, "uevent") }
      runCatching { ue.writeText("MAJOR=81\nMINOR=0\nDEVNAME=video0\n") }
      bind(args, charDir.absolutePath, "/sys/dev/char")
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
    // Qt/GUI on the embedded X server (lorie) has no hardware OpenGL/GLX, so Qt5/6
    // apps (rpi-imager, gnome-disks-style tools, any QtQuick GUI) must render in
    // software and not try to bring up a GLX/EGL context — otherwise they exit with
    // no window ("Could not load the Qt platform plugin" / silent GL failure).
    // Default these so such apps work out of the box; non-Qt/CLI apps ignore them.
    args.add("QT_QUICK_BACKEND=software")
    args.add("QT_XCB_GL_INTEGRATION=none")
    // Mesa software rasteriser fallback for plain OpenGL apps (llvmpipe) — harmless
    // when GL isn't used; lets GL tools run instead of failing to find a GPU.
    args.add("LIBGL_ALWAYS_SOFTWARE=1")
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
      // Native V4L2 node too (proot cam shim) — apps hardcoded to /dev/video0.
      args.add("NEOTERM_CAMERA_V4L2=/dev/video0")
    }
    // GPS: NeoTerm provides a built-in gpsd on 127.0.0.1:2947 (GpsBridge speaks the gpsd client
    // protocol), so distro clients (cgps, gpspipe, …) work with no gpsd installed — they default
    // to localhost:2947, no env needed. Export a hint for scripts when enabled.
    if (NeoPreference.isGpsEnabled()) {
      args.add("NEOTERM_GPSD=127.0.0.1:2947")
    }
    args.add("XDG_RUNTIME_DIR=/tmp")
    // Make the distro's unmodified systemd libudev accept our faked /sys/bus/usb:
    // it otherwise refuses device dirs not on a real sysfs filesystem. This is a
    // GUEST env var (libudev runs in the guest), so it must go here — the guest is
    // launched with `env -i`, so proot's own environment is NOT inherited.
    args.add("SYSTEMD_DEVICE_VERIFY_SYSFS=0")
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
   * Provide a D-Bus **session** bus inside the distro. proot has no
   * systemd/user session to start one, so many GUI apps log "Failed to connect
   * to the bus" / "No D-Bus session bus available" (e.g. Raspberry Pi Imager) and
   * lose notifications, portals, gsettings, single-instance handling, etc.
   *
   * We drop a `/etc/profile.d` snippet that every login shell sources: it lazily
   * starts a private session daemon at a fixed socket under `$XDG_RUNTIME_DIR`,
   * reuses a live one across shells/tabs (pidfile liveness check), cleans a stale
   * socket, and exports `DBUS_SESSION_BUS_ADDRESS`. It is a complete no-op when
   * `dbus-daemon` isn't installed, so a CLI-only setup is unaffected. Apps
   * launched from the terminal inherit the address. Rewritten each launch so
   * improvements ship; cheap.
   */
  internal fun ensureDbusSession(distro: Distro) {
    runCatching {
      val dir = File(distro.rootfsPath(), "etc/profile.d")
      if (!dir.isDirectory && !dir.mkdirs()) return
      File(dir, "95-neoterm-dbus.sh").writeText(
        "# Created by NeoTerm: lazily provide a D-Bus session bus for GUI apps.\n" +
          "# No-op unless dbus-daemon is installed (then GUI apps get a working bus).\n" +
          "if command -v dbus-daemon >/dev/null 2>&1; then\n" +
          "  : \"\${XDG_RUNTIME_DIR:=/tmp}\"\n" +
          "  [ -d \"\$XDG_RUNTIME_DIR\" ] || mkdir -p \"\$XDG_RUNTIME_DIR\" 2>/dev/null\n" +
          "  _ntb=\"\$XDG_RUNTIME_DIR/dbus-session\"\n" +
          "  _ntp=\"\$XDG_RUNTIME_DIR/dbus-session.pid\"\n" +
          "  _ntpid=\"\"; [ -r \"\$_ntp\" ] && _ntpid=\"\$(cat \"\$_ntp\" 2>/dev/null)\"\n" +
          "  _ntok=0\n" +
          "  # Live only if the socket exists AND the pid is a running dbus-daemon\n" +
          "  # (the /proc/comm check rejects a recycled pid, so we never export a\n" +
          "  # stale address that would make apps fail to connect).\n" +
          "  if [ -S \"\$_ntb\" ] && [ -n \"\$_ntpid\" ] && kill -0 \"\$_ntpid\" 2>/dev/null; then\n" +
          "    case \"\$(cat \"/proc/\$_ntpid/comm\" 2>/dev/null)\" in dbus-daemon*) _ntok=1 ;; esac\n" +
          "  fi\n" +
          "  if [ \"\$_ntok\" != 1 ]; then\n" +
          "    rm -f \"\$_ntb\" \"\$_ntp\" 2>/dev/null\n" +
          "    command -v dbus-uuidgen >/dev/null 2>&1 && dbus-uuidgen --ensure >/dev/null 2>&1\n" +
          "    dbus-daemon --session --address=\"unix:path=\$_ntb\" --fork --print-pid >\"\$_ntp\" 2>/dev/null\n" +
          "  fi\n" +
          "  [ -S \"\$_ntb\" ] && export DBUS_SESSION_BUS_ADDRESS=\"unix:path=\$_ntb\"\n" +
          "  unset _ntb _ntp _ntpid _ntok\n" +
          "fi\n"
      )
    }
  }

  /**
   * A proot bináris (host-oldali) környezete. A proot statikusan linkelt, így
   * minimális env elég; a fontos a `PROOT_TMP_DIR`, amit egy app-saját,
   * írható könyvtárra állítunk.
   */
  private fun buildHostEnv(): Array<String> {
    val env = mutableListOf(
      "PROOT_TMP_DIR=${NeoTermPath.PROOT_TMP_PATH}",
      "HOME=${NeoTermPath.HOME_PATH}",
      "TERM=xterm-256color",
      "PATH=/system/bin:/system/xbin",
      "ANDROID_ROOT=" + (System.getenv("ANDROID_ROOT") ?: "/system"),
      "ANDROID_DATA=" + (System.getenv("ANDROID_DATA") ?: "/data")
    )
    // Only with the storage toggle on: proot then traps read/write/lseek/… for the
    // /dev/uksd0 block proxy (otherwise those syscalls aren't filtered at all).
    if (NeoPreference.isUsbStorageEnabled()) { env.add("UK_BLOCK=1"); env.add("UK_FS=1") }
    // Camera: trap the V4L2 syscalls (ioctl/read/poll/…) for the /dev/video0 shim.
    if (NeoPreference.isCameraEnabled()) { env.add("UK_CAM=1") }
    // USB Wi-Fi: gate the proot control-plane redirect (AF_NETLINK/AF_PACKET/wext
    // + init_module/finit_module/delete_module + /proc/modules -> io.neoterm.wifi).
    // The redirect itself (uknl_wifi_redirect.c) is W3; this wires the gate now.
    if (NeoPreference.isUsbWifiEnabled()) { env.add("UK_WIFI=1") }
    // USB (libusb): trap bind() so the netlink udev-hotplug monitor can't fail
    // libusb_init() (Android/SELinux blocks the netlink group bind). The USB host
    // bridge is always active, so this is always on. Scoped to netlink monitors.
    env.add("UK_USB=1")
    // (SYSTEMD_DEVICE_VERIFY_SYSFS is a GUEST env var — set in buildLaunch's `env -i`
    //  list, not here; proot's own environment isn't inherited by the guest.)
    return env.toTypedArray()
  }
}
