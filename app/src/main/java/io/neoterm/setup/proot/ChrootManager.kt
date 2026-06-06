package io.neoterm.setup.proot

import io.neoterm.component.config.NeoTermPath
import io.neoterm.utils.RootUtils
import java.io.File

/**
 * Real-root **chroot** runtime, offered instead of proot on rooted devices.
 *
 * Modeled on Kali NetHunter Terminal's launcher: a normal Android shell
 * (/system/bin/sh) runs on the PTY (so it's a session leader with a controlling
 * terminal), detects the su flavor, and runs the chroot in the GLOBAL mount
 * namespace (Magisk `-mm` / KernelSU `-M -p`). Inside the chroot we exec the
 * distro's own `/bin/su`, which sets up a proper login session + controlling
 * terminal — so job control works (Ctrl+C/Ctrl+Z/fg/bg). The mounts + chroot
 * live in a small root script file to avoid nested-quoting hell.
 *
 * Real kernel access means audio/USB go straight to the device, so the
 * Android-side bridges (PulseAudio/AAudio, mic, USB socket) are NOT used here
 * (see NeoTermService) and PULSE_* is not exported.
 *
 * @author kiva
 */
object ChrootManager {

  fun isUsable(): Boolean = RootUtils.isRooted()

  fun buildLaunch(
    distro: Distro = ProotManager.selectedDistro(),
    loginShell: String? = null,
    guestCwd: String = "/root",
    extraEnv: List<String> = emptyList(),
    command: List<String> = emptyList()
  ): ProotManager.Launch {
    val rootfs = distro.rootfsPath()
    val lang = ProotManager.guestLang(distro)
    val x11Sock = "${NeoTermPath.PROOT_ROOT_PATH}/x11/.X11-unix"
    File(x11Sock).apply { mkdirs() }
    val ext = System.getenv("EXTERNAL_STORAGE") ?: ""

    // What to exec inside the chroot.
    //
    // Interactive: we must give the guest shell its own session WITH a controlling
    // terminal, otherwise it isn't the foreground process group and Ctrl+C kills
    // the whole pipeline instead of the foreground job. The host su (Magisk -mm)
    // forks rather than execs, so the shell would not be the session leader on its
    // own. So inside the chroot we run `setsid -w --ctty <shell> -l`: setsid makes
    // a new session + sets the tty as its controlling terminal, `-w` keeps the
    // chain alive until the shell exits (else the host su would return early and
    // SIGHUP the shell), and `-l` makes it a login shell (proper /root cwd +
    // profile). The guest's setsid is util-linux and supports --ctty (the earlier
    // breakage was the host toybox setsid, which does not). We cd to $HOME first so
    // the login shell starts in /root.
    //
    // One-shot package command: a plain bash -c is enough (no job control needed).
    //
    // "$CH" is the resolved host chroot binary (the guest PATH we export below
    // would otherwise hide /system/bin/chroot).
    val shell = loginShell ?: "/bin/bash"
    val inChroot = if (command.isEmpty()) {
      "exec \"\$CH\" \"\$R\" /bin/bash -c 'cd \"\$HOME\" 2>/dev/null; if command -v setsid >/dev/null 2>&1; then exec setsid -w --ctty $shell -l; else exec $shell -l; fi'"
    } else {
      "exec \"\$CH\" \"\$R\" /bin/bash -c ${sq(command.joinToString(" "))}"
    }

    // Root boot script (run via `su … -c "sh <file>"`): bind the kernel fs,
    // export the guest env, then chroot. Written to a file so we don't have to
    // quote a whole script inside `su -c "…"`.
    val boot = buildString {
      // Host PATH first so chroot/mount/grep/mkdir resolve before we switch to
      // the guest PATH; remember the chroot binary while it's still reachable.
      append("export PATH=/sbin:/system/bin:/system/xbin:/vendor/bin:/odm/bin:/product/bin\n")
      append("CH=\$(command -v chroot 2>/dev/null); [ -z \"\$CH\" ] && CH=/system/bin/chroot\n")
      append("R=").append(sq(rootfs)).append('\n')
      append("for d in dev proc sys tmp dev/pts dev/shm tmp/.X11-unix root; do mkdir -p \"\$R/\$d\" 2>/dev/null; done\n")
      append(bindIfNeeded("/dev", "\$R/dev"))
      append(bindIfNeeded("/proc", "\$R/proc"))
      append(bindIfNeeded("/sys", "\$R/sys"))
      append("grep -q \" \$R/dev/pts \" /proc/mounts 2>/dev/null || mount -t devpts devpts \"\$R/dev/pts\" 2>/dev/null\n")
      append("grep -q \" \$R/dev/shm \" /proc/mounts 2>/dev/null || mount -t tmpfs tmpfs \"\$R/dev/shm\" 2>/dev/null\n")
      append("grep -q \" \$R/tmp/.X11-unix \" /proc/mounts 2>/dev/null || mount -o bind ").append(sq(x11Sock)).append(" \"\$R/tmp/.X11-unix\" 2>/dev/null\n")
      if (ext.isNotEmpty()) {
        append("if [ -d ").append(sq(ext)).append(" ]; then mkdir -p \"\$R/sdcard\" 2>/dev/null; grep -q \" \$R/sdcard \" /proc/mounts 2>/dev/null || mount -o bind ").append(sq(ext)).append(" \"\$R/sdcard\" 2>/dev/null; fi\n")
      }
      // We are already root with direct kernel access, so let apt download as root
      // instead of dropping to the _apt user (which can't traverse the chroot and
      // prints a harmless "Permission denied … unsandboxed as root" warning).
      append("if [ -d \"\$R/etc/apt/apt.conf.d\" ] && [ ! -e \"\$R/etc/apt/apt.conf.d/99neoterm\" ]; then printf 'APT::Sandbox::User \"root\";\\n' > \"\$R/etc/apt/apt.conf.d/99neoterm\" 2>/dev/null; fi\n")
      // Guest environment (no PULSE_* — audio is direct in chroot).
      append("export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin\n")
      append("export TERM=xterm-256color HOME=/root TMPDIR=/tmp USER=root LOGNAME=root\n")
      append("export LANG=").append(sq(lang)).append('\n')
      append("export DISPLAY=:0 XDG_RUNTIME_DIR=/tmp\n")
      append("export MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1\n")
      append("export CHROMIUM_FLAGS='--no-sandbox --disable-gpu' GTK_USE_PORTAL=0 NO_AT_BRIDGE=1\n")
      extraEnv.forEach { if (it.isNotEmpty()) append("export ").append(sq(it)).append('\n') }
      append(inChroot).append('\n')
    }

    val bootFile = File(
      NeoTermPath.PROOT_ROOT_PATH,
      if (command.isEmpty()) "chroot-boot.sh" else "chroot-exec.sh"
    )
    runCatching {
      bootFile.parentFile?.mkdirs()
      bootFile.writeText(boot)
    }

    // Launcher (runs as the Android shell on the PTY): detect su flavor and run
    // the boot script as root in the global mount namespace.
    val launcher = buildString {
      append("export PATH=/sbin:/system/bin:/system/xbin:/vendor/bin:/odm/bin:/product/bin:.\n")
      append("SU=\$(command -v su 2>/dev/null); [ -z \"\$SU\" ] && SU=/system/bin/su\n")
      append("MV=\$(magisk -V 2>/dev/null); case \"\$MV\" in ''|*[!0-9]*) MV=0;; esac\n")
      append("VER=\"\$(\$SU -V 2>/dev/null)\$(\$SU -v 2>/dev/null)\$(\$SU --version 2>/dev/null)\"\n")
      append("case \"\$VER\" in\n")
      append("  *KernelSU*) SUDO=\"\$SU -M -p -c\";;\n")
      append("  *MagiskSU*) if [ \"\$MV\" -gt 28100 ]; then SUDO=\"\$SU -i -mm -c\"; else SUDO=\"\$SU -mm -c\"; fi;;\n")
      append("  *) SUDO=\"\$SU -c\";;\n")
      append("esac\n")
      append("exec \$SUDO ").append(sq("sh ${sq(bootFile.absolutePath)}")).append('\n')
    }

    return ProotManager.Launch(
      executable = "/system/bin/sh",
      args = arrayOf("sh", "-c", launcher),
      env = arrayOf(
        "PATH=/system/bin:/system/xbin:/sbin",
        "TERM=xterm-256color"
      ),
      hostCwd = NeoTermPath.PROOT_ROOT_PATH
    )
  }

  private fun bindIfNeeded(src: String, dst: String): String =
    "grep -q \" $dst \" /proc/mounts 2>/dev/null || mount -o bind $src \"$dst\" 2>/dev/null\n"

  /** Single-quote a token for safe use in a POSIX shell. */
  private fun sq(s: String): String = "'" + s.replace("'", "'\\''") + "'"
}
