package io.neoterm.setup.proot

import io.neoterm.component.config.NeoTermPath
import io.neoterm.utils.RootUtils
import java.io.File

/**
 * Real-root **chroot** runtime, offered instead of proot on rooted devices.
 *
 * Unlike proot (ptrace fake-chroot, no kernel privileges), chroot has true root
 * and kernel access, so the distro talks to hardware directly: ALSA at /dev/snd,
 * USB at /dev/bus/usb, etc. Therefore the Android-side bridges (PulseAudio over
 * AAudio, the mic source, the USB fd socket) are deliberately NOT used in this
 * mode — see NeoTermService — and PULSE_SERVER/PULSE_SOURCE are not exported.
 *
 * The whole thing runs as one `su -c "<script>"`: bind-mount /dev /proc /sys
 * (+ a tmpfs /dev/shm and the X11 socket dir), then `chroot` into the rootfs and
 * exec the login shell. Reuses [ProotManager.Launch] so the session/native exec
 * path is unchanged.
 *
 * @author kiva
 */
object ChrootManager {

  /** True when chroot is selected AND the device actually has `su`. */
  fun isUsable(): Boolean = RootUtils.isRooted()

  fun buildLaunch(
    distro: Distro = ProotManager.selectedDistro(),
    loginShell: String? = null,
    guestCwd: String = "/root",
    extraEnv: List<String> = emptyList(),
    command: List<String> = emptyList()
  ): ProotManager.Launch {
    val rootfs = distro.rootfsPath()
    val shell = loginShell ?: distro.defaultShell
    val su = RootUtils.suPath() ?: "su"

    val x11Sock = "${NeoTermPath.PROOT_ROOT_PATH}/x11/.X11-unix"
    File(x11Sock).apply { mkdirs() }

    // Guest environment (clean slate via `env -i`). Note: no PULSE_* — audio in
    // chroot goes straight to the kernel (e.g. ALSA), not through the app.
    val envTokens = ArrayList<String>()
    envTokens.add("HOME=/root")
    envTokens.add("PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
    envTokens.add("TERM=xterm-256color")
    envTokens.add("COLORTERM=truecolor")
    envTokens.add("LANG=${ProotManager.guestLang(distro)}")
    envTokens.add("PS1=\\u@neoterm:\\w\\$ ")
    envTokens.add("TMPDIR=/tmp")
    envTokens.add("SHELL=$shell")
    envTokens.add("DISPLAY=:0")
    envTokens.add("XDG_RUNTIME_DIR=/tmp")
    envTokens.add("MOZ_DISABLE_CONTENT_SANDBOX=1")
    envTokens.add("MOZ_DISABLE_RDD_SANDBOX=1")
    envTokens.add("CHROMIUM_FLAGS=--no-sandbox --disable-gpu")
    envTokens.add("GTK_USE_PORTAL=0")
    envTokens.add("NO_AT_BRIDGE=1")
    extraEnv.forEach { if (it.isNotEmpty()) envTokens.add(it) }

    val envArgs = envTokens.joinToString(" ") { sq(it) }
    // For the interactive login shell, wrap it in `setsid --ctty` so it becomes
    // a session leader with the PTY as its controlling terminal — otherwise the
    // su-spawned shell has no job control ("cannot set terminal process group").
    // One-shot package commands don't need a controlling terminal.
    val shellPart = if (command.isEmpty()) {
      "setsid --ctty ${sq(shell)} ${distro.loginArgs.joinToString(" ") { sq(it) }}"
    } else {
      "${sq(shell)} -c ${sq(command.joinToString(" "))}"
    }

    val ext = System.getenv("EXTERNAL_STORAGE")
    val script = buildString {
      append("R=").append(sq(rootfs)).append('\n')
      append("mount --bind /dev \"\$R/dev\" 2>/dev/null\n")
      append("mount --bind /proc \"\$R/proc\" 2>/dev/null\n")
      append("mount --bind /sys \"\$R/sys\" 2>/dev/null\n")
      append("mount --bind /dev/pts \"\$R/dev/pts\" 2>/dev/null\n")
      append("mkdir -p \"\$R/dev/shm\" 2>/dev/null; mount -t tmpfs tmpfs \"\$R/dev/shm\" 2>/dev/null\n")
      append("mkdir -p \"\$R/tmp/.X11-unix\" 2>/dev/null; mount --bind ").append(sq(x11Sock)).append(" \"\$R/tmp/.X11-unix\" 2>/dev/null\n")
      if (ext != null && ext.isNotEmpty()) {
        append("[ -d ").append(sq(ext)).append(" ] && mkdir -p \"\$R/sdcard\" && mount --bind ")
          .append(sq(ext)).append(" \"\$R/sdcard\" 2>/dev/null\n")
      }
      append("cd \"\$R\" 2>/dev/null\n")
      append("exec chroot \"\$R\" /usr/bin/env -i ").append(envArgs).append(' ').append(shellPart).append('\n')
    }

    return ProotManager.Launch(
      executable = su,
      args = arrayOf("su", "-c", script),
      env = arrayOf(
        "PATH=/system/bin:/system/xbin:/sbin",
        "TERM=xterm-256color"
      ),
      hostCwd = NeoTermPath.PROOT_ROOT_PATH
    )
  }

  /** Single-quote a token for safe use in the su shell script. */
  private fun sq(s: String): String = "'" + s.replace("'", "'\\''") + "'"
}
