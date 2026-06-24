package io.neoterm.setup.proot

import android.system.Os
import io.neoterm.utils.NLog
import java.io.File

/**
 * Guest-oldali kompatibilitási shimek, amiket a rootfs `/usr/local/bin`-jébe
 * írunk (a PATH-ban megelőzi a `/usr/bin`-t, így leárnyékolja a disztró
 * eszközeit, anélkül hogy felülírná őket). Minden indításkor frissítjük
 * ([install]), így a meglévő telepítéseknél is megjelennek.
 *
 * - `systemctl`: proot-ban nincs systemd (PID 1) → a `start/stop/status/…`
 *   igéket SysV initre (`service` / `/etc/init.d`) fordítja, a boot/daemon
 *   igéket sikeres no-opként kezeli. Így a Debian/Kali csomagscriptek és a
 *   `msfdb` nem halnak el a „System has not been booted with systemd" hibán.
 * - `dmesg`: Android tiltja a kernel ring buffert → egy rövid, valószerű
 *   boot-logot ad a NeoTerm/proot környezetről (hibázás helyett).
 * - `journalctl` / `loginctl`: minimális no-op stubok (sok script hívja őket).
 */
object ProotShims {

  private const val SYSTEMCTL = """#!/bin/sh
# Minimal systemctl shim proot-hoz (nincs systemd / PID 1). A gyakori igéket
# SysV initre (service / /etc/init.d) fordítja, a boot/daemon igéket sikeres
# no-opként kezeli. NeoTerm.
case "${'$'}1" in --version|-v|version) echo "systemd 0 (systemctl-shim for proot)"; exit 0;; esac

verb=""; unit=""
for a in "${'$'}@"; do
  case "${'$'}a" in
    -*) : ;;                                  # flagek (--now, --no-pager, -f) eldobva
    *) if [ -z "${'$'}verb" ]; then verb="${'$'}a"; elif [ -z "${'$'}unit" ]; then unit="${'$'}{a%.service}"; fi ;;
  esac
done

svc() {                                       # service -> init.d fallback
  if command -v service >/dev/null 2>&1; then service "${'$'}unit" "${'$'}1"
  elif [ -x "/etc/init.d/${'$'}unit" ]; then "/etc/init.d/${'$'}unit" "${'$'}1"
  else echo "systemctl-shim: nincs init script ehhez: '${'$'}unit'" >&2; return 1; fi
}

case "${'$'}verb" in
  start|stop|restart|reload|force-reload|try-restart) if [ -n "${'$'}unit" ]; then svc "${'$'}verb"; else exit 0; fi ;;
  status)            if [ -n "${'$'}unit" ]; then svc status; else exit 0; fi ;;
  is-active)         if [ -n "${'$'}unit" ] && svc status >/dev/null 2>&1; then echo active; else echo inactive; exit 3; fi ;;
  enable|disable)    command -v update-rc.d >/dev/null 2>&1 && update-rc.d "${'$'}unit" "${'$'}verb" >/dev/null 2>&1; exit 0 ;;
  is-enabled)        echo enabled; exit 0 ;;
  daemon-reload|daemon-reexec|reset-failed|preset|preset-all|set-default|mask|unmask) exit 0 ;;
  is-system-running) echo running; exit 0 ;;
  list-units|list-unit-files|show|cat) exit 0 ;;
  *)                 exit 0 ;;                 # ismeretlen ige -> csendes siker
esac
"""

  private const val DMESG = """#!/bin/sh
# dmesg proot-hoz: a valódi kernel ring buffer tiltott Androidon, ezért a guest
# egy ÍRHATÓ /dev/kmsg pufferbe ír (echo msg > /dev/kmsg), ezt olvassuk vissza.
# A puffer akkumulál: a proot-patch O_APPEND-et kényszerít a /dev/kmsg-re, így a
# csonkoló `>` sem veszít üzenetet (a törlés ezért truncate-tel megy). NeoTerm.
KMSG=/dev/kmsg
clear=0; follow=0; notime=0
for a in "${'$'}@"; do
  case "${'$'}a" in
    -C|--clear)      truncate -s 0 "${'$'}KMSG" 2>/dev/null; exit 0 ;;
    -c|--read-clear) clear=1 ;;
    -w|--follow|-W|--follow-new) follow=1 ;;
    -t|--notime)     notime=1 ;;
    --version)       echo "dmesg (NeoTerm proot kmsg shim)"; exit 0 ;;
    -h|--help)       echo "Usage: dmesg [-c|-C|-w|-t ...] - NeoTerm proot kmsg buffer"; exit 0 ;;
    -*)              : ;;
  esac
done
up=${'$'}(cut -d' ' -f1 /proc/uptime 2>/dev/null || echo 0)
emit() { printf '[%11.6f] %s\n' "${'$'}2" "${'$'}1"; }
banner() {
  emit "Linux ${'$'}(uname -r 2>/dev/null || echo unknown) - NeoTerm proot userland (fake root -0)" 0.000000
  emit "kmsg buffer: ${'$'}KMSG  (write: echo msg > /dev/kmsg)" 0.000100
}
fmt() {
  while IFS= read -r ln; do
    # NeoTerm writes "<seconds>;<msg>" (timestamp recorded at log time). Use that
    # per-line timestamp; fall back to the read-time uptime for raw guest writes
    # (echo msg > /dev/kmsg) and old-format lines, stripping any <prio> prefix.
    ts=${'$'}{ln%%;*}; rest=${'$'}{ln#*;}
    case "${'$'}ts" in
      ''|*[!0-9.]*|*.*.*) m=${'$'}{ln#<*>}; t=${'$'}up ;;
      *)                  m=${'$'}rest;     t=${'$'}ts ;;
    esac
    if [ "${'$'}notime" = 1 ]; then printf '%s\n' "${'$'}m"; else emit "${'$'}m" "${'$'}t"; fi
  done
}
banner
if [ "${'$'}follow" = 1 ]; then
  { [ -f "${'$'}KMSG" ] && tail -n +1 -f "${'$'}KMSG"; } 2>/dev/null | fmt
else
  [ -f "${'$'}KMSG" ] && fmt < "${'$'}KMSG"
  [ "${'$'}clear" = 1 ] && truncate -s 0 "${'$'}KMSG" 2>/dev/null
fi
exit 0
"""

  private const val NOOP = "#!/bin/sh\nexit 0\n"

  /**
   * Login-shellben (root) helyreállítja az auth-adatbázis fájljainak módját.
   * A -0 fake-root alatt egyes csomag-/`adduser`-műveletek a `/etc/passwd`-t
   * (és társait) temp-fájl+rename úton írják újra, és a fájl 0600-ra állhat, ami
   * eltöri a `getpwuid`-ot a privilégium-dropoló démonoknál (pl. PostgreSQL
   * `initdb`: „could not look up effective user ID"). A guestbeli `chmod` a proot
   * chmod-handlerén megy át, így a perzisztens (xattr) módot is helyreállítja —
   * függetlenül attól, hogy meta-érték vagy valódi fájlmód romlott el. SOURCE-olva
   * fut (profile.d), ezért nincs benne `exit`/`return`.
   */
  private const val AUTH_PERMS_FIXUP = """# NeoTerm: az auth-DB fájlok olvashatóságának biztosítása proot fake-root alatt.
if [ "${'$'}(id -u 2>/dev/null)" = 0 ]; then
  [ -e /etc/passwd ]  && chmod 644 /etc/passwd  2>/dev/null
  [ -e /etc/group ]   && chmod 644 /etc/group   2>/dev/null
  [ -e /etc/shadow ]  && chmod 640 /etc/shadow  2>/dev/null
  [ -e /etc/gshadow ] && chmod 640 /etc/gshadow 2>/dev/null
  # Runtime lock dir: picocom/minicom/cu create UUCP lock files here; minimal
  # rootfs (no systemd) may lack it -> "cannot lock /dev/ttyUSB0: Permission denied".
  mkdir -p /run/lock 2>/dev/null && chmod 1777 /run/lock 2>/dev/null
  [ -e /var/lock ] || ln -s /run/lock /var/lock 2>/dev/null
fi
"""

  /** Kiírja/frissíti a shimeket a rootfs `/usr/local/bin`-jébe (idempotens). */
  fun install(rootfs: String) {
    val binDir = File(rootfs, "usr/local/bin")
    if (!binDir.isDirectory && !binDir.mkdirs()) {
      NLog.e("ProotShims", "Cannot create $binDir")
      return
    }
    write(binDir, "systemctl", SYSTEMCTL)
    write(binDir, "dmesg", DMESG)
    write(binDir, "journalctl", NOOP)
    write(binDir, "loginctl", NOOP)

    // Login-shell fixup: az auth-DB fájlok módja (profile.d → root login-shellben fut).
    val profileDir = File(rootfs, "etc/profile.d")
    if (profileDir.isDirectory || profileDir.mkdirs()) {
      runCatching {
        val f = File(profileDir, "00-neoterm-auth-perms.sh")
        f.writeText(AUTH_PERMS_FIXUP)
        Os.chmod(f.absolutePath, 420 /* 0644 — profile.d scriptet a shell source-olja */)
      }.onFailure { NLog.e("ProotShims", "Cannot write auth-perms fixup: ${it.message}") }
    }
  }

  private fun write(dir: File, name: String, content: String) {
    runCatching {
      val f = File(dir, name)
      f.writeText(content)
      Os.chmod(f.absolutePath, 493 /* 0755 */)
    }.onFailure {
      NLog.e("ProotShims", "Cannot write shim $name: ${it.message}")
    }
  }
}
