# NeoTerm — proot alapú átállás

Ez a dokumentum a NeoTerm **Termux-stílusú natív bootstrap → proot** átállását
írja le. A proot rendszer építő-/kiszolgáló oldala a társ-repóban
(`Claude-repo/proot/`) található; ez a dokumentum a kliens-integrációt fedi.

## Mi változott a két modell között

| | Régi (Termux-stílus) | Új (proot) |
| --- | --- | --- |
| Gyökér | `…/files/usr` egyedi `PREFIX` | valódi disztró-rootfs (`…/files/rootfs/<distro>`) |
| Útvonalak | `/bin`, `/usr` futásidőben átírva | a megszokott helyükön (`-r` chroot) |
| Shebang | `libnexec.so` `LD_PRELOAD` wrapper | nem kell — proot módban kikapcsol |
| Csomagok | egyedi `PREFIX`-szel újrafordítva | standard `apt`/`apk`/`pacman` a guestben |
| Izoláció | nincs (app UID) | proot ptrace chroot + bind-mountok, `-0` fake root |

## Kliensoldali komponensek

- **`component/config/defaults.kt`** — új útvonalak: `PROOT_ROOT_PATH`,
  `PROOT_BIN_PATH`, `PROOT_TMP_PATH`, `ROOTFS_PATH`; új forrás
  `DEFAULT_PROOT_SOURCE`. Új default-ok: `enableProot=true`,
  `prootDistro="ubuntu"`.
- **`setup/proot/Distro.kt`** — a támogatott disztrók (Ubuntu/Alpine/Kali/Arch)
  + alapértelmezett shell, login-kapcsolók, rootfs/proot URL-építés.
- **`setup/proot/ProotManager.kt`** — telepítettség-ellenőrzés és a proot
  parancssor összeállítása (`buildLaunch`).
- **`setup/proot/ProotInstaller.kt`** — a proot bináris + a disztró-rootfs
  (`.tar.xz`) letöltése és kibontása (symlink/hardlink/jogosultság kezeléssel,
  opcionális sha256-ellenőrzéssel).
- **`component/config/comp.kt`** (`NeoPreference`) — `isProotEnabled`,
  `getProotDistro` + setterek (`KEY_PROOT_ENABLED`, `KEY_PROOT_DISTRO`).
- **`setup/setup.kt`** — `needSetup()` proot-tudatos; `setupProot()` belépő.
- **`ui/other/SetupActivity.kt`** — proot módban a rootfs-telepítőre ágazik,
  és kihagyja a legacy apt-szinkront.
- **`component/session/shell.kt`** — a bejelentkező shellt proot alá
  csomagolja (`ProotManager.buildLaunch`), `libnexec` nélkül.

## Indítási parancssor

```
proot --kill-on-exit --link2symlink -0 -r <rootfs>           \
      -b /dev -b /proc -b /sys -b /dev/pts                   \
      -b /proc/self/fd:/dev/fd                                \
      -b /proc/self/fd/0:/dev/stdin                           \
      -b /proc/self/fd/1:/dev/stdout                          \
      -b /proc/self/fd/2:/dev/stderr                          \
      -b /dev/urandom:/dev/random  [-b <EXTERNAL_STORAGE>:/sdcard]  \
      -w /root                                                \
      /usr/bin/env -i HOME=/root TERM=xterm-256color LANG=C.UTF-8 … \
      /bin/bash --login
```

A natív réteg (`app/src/main/cpp/neoterm.cpp`) a host oldalon a proot binárist
`execvp`-li; a proot a beágyazott loaderrel fut, `PROOT_TMP_DIR` egy app-saját
írható könyvtár.

## Forrás-layout (kiszolgáló oldal)

A `DEFAULT_PROOT_SOURCE` base-URL alól tölt a kliens:

```
<base>/proot/<arch>/proot
<base>/rootfs/<distro>/<arch>.tar.xz
<base>/rootfs/<distro>/<arch>.tar.xz.sha256
```

`<arch> ∈ {aarch64, arm, x86_64}` (`SetupHelper.determineArchName()`),
`<distro> ∈ {ubuntu, alpine, kali, arch}`. Az artefaktumokat a
`Claude-repo/proot/build-proot.sh` és `fetch-rootfs.sh` állítja elő.

## Disztró kiválasztása

Programozottan: `NeoPreference.setProotDistro("alpine")` (majd újratelepítés).
A `Distro.DEFAULT` jelenleg Ubuntu. (Disztró-választó beállítás-UI: követő
munka.)

## Visszafelé kompatibilitás

A legacy Termux-bootstrap kód érintetlen: ha `isProotEnabled()` hamis, a régi
útvonal fut. A rendszer-shell (`/system/bin/sh`) mód is változatlan, és a proot
telepítés megszakítása esetén fallbackként szolgál.
