#!/usr/bin/env bash
# proot-me/proot cross-compile Android NDK ARM64 (Bionic) toolchain-nel.
#
# Két komponenst kell forrásból fordítani:
#   1) talloc — Samba memory allocator, proot dependency. A waf build-jét
#      kihagyjuk, csak a `lib/talloc/talloc.c`-t fordítjuk le közvetlenül
#      NDK clanggel és libtalloc.a-vé archiveolunk.
#   2) proot  — saját src/Makefile-t használ; CC/LD/CFLAGS env-en át.
#      A loader rész (loader.elf, ptrace-tracee induláshoz) ugyanazzal
#      az NDK CC-vel épül; xxd-vel header-fájlbe inline-olódik.
#
# Bemenet (env):
#   ANDROID_NDK_HOME — NDK install path
#   TARGET           — clang triple (default aarch64-linux-android24)
#
# Verzió-pin:
#   PROOT_VERSION=5.4.0
#   TALLOC_VERSION=2.4.2
#
# Kimenet: $DL_DIR/proot — statikus aarch64 Android ELF (kb. 1-2 MB).
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DL_DIR="${1:?usage: build-proot.sh <download_dir>}"
mkdir -p "${DL_DIR}"

: "${ANDROID_NDK_HOME:?ANDROID_NDK_HOME nincs beállítva (NDK install path)}"
NDK_BIN="${ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/bin"
TARGET="${TARGET:-aarch64-linux-android24}"

PROOT_VERSION="${PROOT_VERSION:-5.4.0}"
TALLOC_VERSION="${TALLOC_VERSION:-2.4.2}"

CC="${NDK_BIN}/${TARGET}-clang"
# A loader linkelése a proot GNUmakefile-ben `-Wl,...,--rosegment`-et használ
# (LOADER_LDFLAGS), amit csak az lld ismer. Az NDK alapértelmezett linkere a
# régi bfd `ld` (binutils 4.9.x), ami "unrecognized option '--rosegment'"-tel
# elhasal. Ezért minden link-lépést (loader + fő proot) lld-vel végeztetünk.
LD="${CC} -fuse-ld=lld"
AR="${NDK_BIN}/llvm-ar"
# OBJCOPY + OBJDUMP: a proot GNUmakefile a loader-wrapped.o-hoz az
# `objdump -f cli/cli.o` outputból derivátja a formátumot és arch-ot,
# majd átadja `objcopy --output-target=... --binary-architecture=...`-nak.
# Default-ban host x86_64 binutils-t használ, ami az aarch64 .o-t
# "architecture: UNKNOWN!" formában írja le — innen jött a
# "architecture UNKNOWN! unknown" linker-hiba.
#
# GNU binutils-aarch64-linux-gnu (apt) tartalmazza a megfelelő
# multi-arch parsert mind objdump-ban mind objcopy-ban.
if command -v aarch64-linux-gnu-objcopy >/dev/null \
   && command -v aarch64-linux-gnu-objdump >/dev/null; then
    OBJCOPY="$(command -v aarch64-linux-gnu-objcopy)"
    OBJDUMP="$(command -v aarch64-linux-gnu-objdump)"
    echo "[build-proot] OBJCOPY = ${OBJCOPY} (GNU aarch64 binutils)"
    echo "[build-proot] OBJDUMP = ${OBJDUMP} (GNU aarch64 binutils)"
else
    OBJCOPY="${NDK_BIN}/llvm-objcopy"
    OBJDUMP="${NDK_BIN}/llvm-objdump"
    echo "[build-proot] OBJCOPY = ${OBJCOPY} (NDK llvm-objcopy — fallback)"
    echo "[build-proot] OBJDUMP = ${OBJDUMP} (NDK llvm-objdump — fallback)"
fi

if [[ ! -x "${CC}" ]]; then
  echo "[build-proot] HIBA: nincs ${CC}" >&2
  ls "${NDK_BIN}" >&2 | head -20 || true
  exit 3
fi

# ─── 1) Forrás: proot VENDORELVE a repóban, talloc tarball ────────────────
# A proot forrást NEM töltjük le build-időben: a Termux fork egy pinnelt
# másolata a repóban van (proot/vendor/proot, lásd VENDOR.md). Így a build
# reprodukálható és offline. (A TERMUX forkból buildelünk, NEM az upstream
# proot-me-ből — utóbbit az Android-kernel seccomp-filtere SIGSYS-szel
# (signal 31) öli, mert a kernel blokkolja a ptrace-syscallt; a Termux fork
# tartalmazza az ehhez kellő kernel-hook patcheket.)
VENDOR_PROOT="${SCRIPT_DIR}/vendor/proot"
if [[ ! -d "${VENDOR_PROOT}/src" ]]; then
  echo "[build-proot] HIBA: nincs vendorelt proot forrás: ${VENDOR_PROOT}/src" >&2
  echo "[build-proot] (lásd proot/vendor/proot/VENDOR.md — a forrást a repó tartalmazza)" >&2
  exit 2
fi

# talloc: továbbra is tarball (a samba allocator dependency). Vendorelhető
# ugyanígy, ha teljesen offline build kell — egyelőre letöltjük + cache-eljük.
TALLOC_TARBALL="${DL_DIR}/talloc-${TALLOC_VERSION}.tar.gz"
if [[ ! -f "${TALLOC_TARBALL}" ]]; then
  TALLOC_URL="https://www.samba.org/ftp/talloc/talloc-${TALLOC_VERSION}.tar.gz"
  echo "[build-proot] download talloc src: ${TALLOC_URL}"
  curl -fL --retry 4 --retry-delay 2 -o "${TALLOC_TARBALL}" "${TALLOC_URL}"
fi

WORK="${DL_DIR}/build"
rm -rf "${WORK}"
mkdir -p "${WORK}"
# proot: a pristine vendor-fát egy friss munkamásolatba másoljuk, mert a build
# in-place patcheli a forrást (sed a Makefile-on/stat.c-n + a python xattr-patch).
# Így a vendor-fa érintetlen marad, és ismételt build sem kettőzi a patcheket.
PROOT_DIR="${WORK}/proot-src"
mkdir -p "${PROOT_DIR}"
cp -a "${VENDOR_PROOT}/." "${PROOT_DIR}/"
# talloc: tarball kibontás a WORK-be.
( cd "${WORK}" && tar -xzf "${TALLOC_TARBALL}" )

TALLOC_DIR=$(find "${WORK}" -maxdepth 2 -type d -name "talloc-*" | head -n1)
echo "[build-proot] proot src:  ${PROOT_DIR} (vendor: ${VENDOR_PROOT})"
echo "[build-proot] talloc src: ${TALLOC_DIR}"

# ─── 2) Build talloc statikusan (csak talloc.c, waf nélkül) ──────────────
TALLOC_OUT="${WORK}/talloc-out"
mkdir -p "${TALLOC_OUT}"

# A samba standalone talloc tarball-ban a talloc.c és .h a ROOT alatt
# vannak (NEM lib/talloc/-ban — az csak a teljes samba git tree layoutja).
# Dinamikusan megkeressük.
TALLOC_C=$(find "${TALLOC_DIR}" -maxdepth 4 -name 'talloc.c' -type f \
           ! -path '*/bin/*' ! -path '*/test*' | head -n1)
TALLOC_H=$(find "${TALLOC_DIR}" -maxdepth 4 -name 'talloc.h' -type f \
           ! -path '*/bin/*' ! -path '*/test*' | head -n1)
if [[ -z "${TALLOC_C}" || -z "${TALLOC_H}" ]]; then
  echo "[build-proot] HIBA: talloc.c vagy talloc.h nem található:" >&2
  echo "  talloc.c=${TALLOC_C}" >&2
  echo "  talloc.h=${TALLOC_H}" >&2
  echo "  talloc tree top:" >&2
  ls -la "${TALLOC_DIR}" >&2 | head -30
  exit 7
fi
TALLOC_INC=$(dirname "${TALLOC_H}")
echo "[build-proot] talloc.c = ${TALLOC_C}"
echo "[build-proot] talloc.h = ${TALLOC_H} (include: ${TALLOC_INC})"

# A talloc.c néhány HAVE_* makrót vár az autoconf-config.h-ból; mi NDK-val
# (Bionic, modern POSIX) cross-compile-olunk, és ezeket explicit megadjuk.
# A `replace.h`-t a HAVE_-makrókkal eldobjuk és csak a stdlib-define-okra
# hagyatkozunk.
#
# TALLOC_BUILD_VERSION_* — normalban a samba waf build-system generálja
# a verzió-számból. Mi a TALLOC_VERSION env változóból szedjük ki ÉS
# explicit átadjuk -D-vel hogy a TALLOC_MAGIC_NON_RANDOM macro lefusson.
IFS=. read -r TALLOC_V_MAJ TALLOC_V_MIN TALLOC_V_REL <<< "${TALLOC_VERSION}"
TALLOC_DEFS=(
    -DHAVE_VA_COPY=1
    -DHAVE_INTPTR_T=1
    -DHAVE_VOID_PTR=1
    -DHAVE_STDLIB_H=1
    -DHAVE_STDIO_H=1
    -DHAVE_STDARG_H=1
    -DHAVE_STDBOOL_H=1
    -DHAVE_STDINT_H=1
    -DHAVE_STRING_H=1
    -DHAVE_UNISTD_H=1
    -DHAVE_TIME_H=1
    -DHAVE_MEMSET=1
    -DHAVE_GETPAGESIZE=1
    -D_GNU_SOURCE=1
    -DTALLOC_BUILD_VERSION_MAJOR=${TALLOC_V_MAJ}
    -DTALLOC_BUILD_VERSION_MINOR=${TALLOC_V_MIN}
    -DTALLOC_BUILD_VERSION_RELEASE=${TALLOC_V_REL}
)

# talloc.c #include "replace.h" — ez samba POSIX-compat shim, NEM része a
# standalone talloc tarball-nak. Bionic NDK 24+-on minden olyan POSIX-funkció
# elérhető natívan amit a samba replace.c körbe-helyettesít, így egy minimál
# stub-ot tudunk adni neki ami csak a standard libc header-eket include-olja.
STUB_INC="${WORK}/talloc-stub-include"
mkdir -p "${STUB_INC}"
cat > "${STUB_INC}/replace.h" <<'EOF'
/* Minimal replace.h stub — csak Bionic NDK 24+ build-hez. A samba lib/replace
 * a portable POSIX-compat shim, de NDK Bionic-on minden szükséges függvény
 * (snprintf, strdup, mmap, va_copy, ...) natívan megvan. */
#ifndef _MINIMAL_REPLACE_H
#define _MINIMAL_REPLACE_H

#define _GNU_SOURCE 1

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <fcntl.h>
#include <sys/param.h>   /* MIN/MAX glibc-on */

/* MIN/MAX (glibc-extension) — Bionic-ban nincs, talloc.c hivatkozza. */
#ifndef MIN
#define MIN(a,b) (((a) < (b)) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a,b) (((a) > (b)) ? (a) : (b))
#endif

#endif /* _MINIMAL_REPLACE_H */
EOF

echo "[build-proot] compile talloc.c"
"${CC}" -c -O2 -fPIC \
    "${TALLOC_DEFS[@]}" \
    -I"${STUB_INC}" \
    -I"${TALLOC_INC}" \
    -I"${TALLOC_DIR}" \
    "${TALLOC_C}" \
    -o "${TALLOC_OUT}/talloc.o"

"${AR}" rcs "${TALLOC_OUT}/libtalloc.a" "${TALLOC_OUT}/talloc.o"
ls -lh "${TALLOC_OUT}/libtalloc.a"

# A proot Makefile <talloc.h>-t #include-ol — adjuk meg a header-keresési
# útvonalat is.
TALLOC_INCLUDE_FLAG="-I${TALLOC_INC}"

# ─── 3) Build proot (saját Makefile-jével) ────────────────────────────────
# A proot release-eknek vegyes a layout-ja: van amelyikben src/Makefile,
# van amelyikben GNUmakefile, van amelyikben a top-level Makefile vezet
# az egészbe. Dinamikusan keressük meg.
PROOT_MAKEFILE=$(find "${PROOT_DIR}" -maxdepth 3 \
                 \( -name Makefile -o -name GNUmakefile \) -type f \
                 ! -path '*/test*' ! -path '*/loader/*' | head -n1)
if [[ -z "${PROOT_MAKEFILE}" ]]; then
  echo "[build-proot] HIBA: nem találom a proot fő Makefile-t." >&2
  echo "[build-proot] proot tree:" >&2
  find "${PROOT_DIR}" -maxdepth 3 -type d >&2
  echo "[build-proot] összes Makefile:" >&2
  find "${PROOT_DIR}" -maxdepth 4 -name '[Mm]akefile' -o -name 'GNUmakefile' >&2
  exit 4
fi
PROOT_SRC=$(dirname "${PROOT_MAKEFILE}")
echo "[build-proot] proot Makefile: ${PROOT_MAKEFILE}"
echo "[build-proot] proot src dir:  ${PROOT_SRC}"

# proot a `xxd` parancsot várja a loader header-ének előállításához.
if ! command -v xxd >/dev/null; then
  echo "[build-proot] HIBA: 'xxd' nincs telepítve (apt install xxd / vim-common)" >&2
  exit 5
fi

# A proot Makefile a CC/LD/CFLAGS/LDFLAGS env-változókat figyelembe veszi.
# Cross-compile esetén minden eszközt explicit átadunk; CFLAGS-ban a talloc.h
# kereshetősége + az NDK API szint definíciói.
# A proot egy "python" extension-t is buildel default-ban, ami CPython
# fejlesztői header-eket vár arch-specifikus pyconfig.h-val. Nekünk se nem
# kell (nem futtatunk python-ban scriptelt proot-runt), se nem elérhető a
# cross-target python sysroot-on.
#
# Stratégia (minimálisan invazív):
#   1) A python.o és proot_wrap.o tokeneket egyszerűen kivesszük az OBJECTS
#      = ... (vagy +=) sorból. A rules (target:recipe blokkok) érintetlenül
#      maradnak, de make sosem hívja meg őket, mert nincsenek az
#      $(OBJECTS)-ben.
#   2) A python forrás-direktóriát töröljük (sanity — ha egy makro mégis
#      probálkozna build-elni, nem lesz mit).
#
# Korábban sed/awk-vel törölni próbáltuk az egész szabályt, de az
# `ifdef`/`endif` páros és `$(shell ...)` blokkok közelébe nyúlva
# "extraneous 'endif'" / "Illegal option -g" hibákat kaptunk. A targeted
# sed csak konkrét token-eket cserél le.
echo "[build-proot] strip python extension (cross-build incompat)"
# Az OBJECTS / prerequisites listákból kivesszük az ÖSSZES python-asszet
# referenciát. A target:recipe blokkok érintetlenek maradnak (sose lesznek
# invokálva mert nincsenek a build dep-grafikusban).
for tok in \
    'extension/python/python\.o' \
    'extension/python/python_extension\.o' \
    'extension/python/proot_wrap\.o' \
    'extension/python/proot\.o' \
    'extension/python/python_extension\.py' \
    'extension/python/proot\.py' \
    'extension/python/proot\.i' \
    'python_extension\.py' \
    'python_extension\.o' \
    'proot\.py' \
    'proot\.i'
do
    sed -i.bak "s| *${tok}||g" "${PROOT_MAKEFILE}"
done
# A python forrásokat NEM töröljük, csak a python.c-t cseréljük le
# egy stub-ra: a `python_callback()` szimbólumot a cli/proot.c LD-szinten
# referálja (a -P CLI option handler kéri), így nem elég csak az OBJECTS-
# ből kivenni — egy stub function kell ami undefined symbol-t megakadá-
# lyozza. Visszaadunk -1-et = a -P opció soft-fail-el ha valaki használná.
mkdir -p "${PROOT_SRC}/extension/python"
cat > "${PROOT_SRC}/extension/python/python_stub.c" <<'EOF'
/* Stub: python extension nincs build-elve. A cli/proot.c -P opciója
 * (initialize_extension(tracee, python_callback, value)) hivatkozik
 * a python_callback szimbólumra LD-szinten. Ez a stub csak undefined-
 * symbolt akadályoz meg; a -P futtatása silent -1 (= "extension not
 * available", proot folytatja értelmesen). */
#include <stdint.h>
struct Extension;
typedef struct Extension Extension;
typedef int ExtensionEvent;
int python_callback(Extension *e, ExtensionEvent ev, intptr_t d1, intptr_t d2)
{
    (void)e; (void)ev; (void)d1; (void)d2;
    return -1;
}
EOF
# A stub-ot statikusan adjuk a proot OBJECTS listához. Az OBJECTS-listát
# a `proot: $(OBJECTS)` szabály kéri — make automatikusan végigmegy a
# %.o → %.c szabályon és lefordítja. Standalone OBJECTS += sort beszúrunk
# az első OBJECTS += elé (continuation \ miatt nem fűzhetjük utána).
sed -i.bak '/^OBJECTS += \\$/i OBJECTS += extension/python/python_stub.o' "${PROOT_MAKEFILE}"

# A LOADER_LDFLAGS `--rosegment`-et ad át (`...,-Ttext=...,--rosegment,-z,...`),
# ami egy gold/bfd-specifikus opció. Az lld-ben a read-only szegmens az
# ALAPÉRTELMEZETT (a kapcsoló neve ott `--no-rosegment` lenne), így az lld a
# pozitív `--rosegment`-et "unknown argument"-ként eldobja; a régi NDK bfd ld
# pedig "unrecognized option"-nel. Mivel lld-vel linkelünk és az úgyis külön
# rodata-szegmenst gyárt, egyszerűen kivesszük a flaget — funkcionálisan azonos.
sed -i.bak 's/,--rosegment//g' "${PROOT_MAKEFILE}"

# ─── fake_id0 ownership fix (suid → euid) ─────────────────────────────────
# A -0 fake-root módban a `stat` egy app-tulajdonú fájl tulajdonosát az
# EMULÁLT MENTETT uid-dal (config->suid) írja felül. A programok viszont a
# `st_uid == geteuid()` mintával ellenőrzik a tulajdont, azaz az EFFEKTÍV
# uid (config->euid) ellen. Amikor egy privilege-drop euid != suid állapotot
# hagy — pl. `su`/`runuser`/`pg_createcluster`, ami seteuid-del vált a cél-
# userre, de a mentett uid 0 marad —, a check elhasal:
#   FATAL: data directory "..." has wrong ownership   (PostgreSQL)
# A `setpriv` azért működik, mert teljesen dropol (ruid=euid=suid). Mivel a
# FATAL eleve csak akkor jön, ha st_uid != geteuid(), és st_uid == suid, ez
# bizonyítja hogy suid != euid — így az effektív uid/gid jelentése a stat-ban
# (mind a hagyományos stat, mind a modern statx ágon, amit a glibc ma használ)
# konstrukció szerint helyreállítja az egyezést.
FAKEID_STAT=$(find "${PROOT_SRC}" -path '*/fake_id0/stat.c' -type f 2>/dev/null | head -n1)
if [[ -n "${FAKEID_STAT}" ]]; then
  echo "[build-proot] patch fake_id0 stat ownership (suid->euid): ${FAKEID_STAT}"
  sed -i.bak -e 's/config->suid/config->euid/g' -e 's/config->sgid/config->egid/g' "${FAKEID_STAT}"
  grep -q 'config->euid' "${FAKEID_STAT}" || { echo "[build-proot] HIBA: fake_id0 stat patch nem alkalmazódott" >&2; exit 8; }
else
  echo "[build-proot] WARN: fake_id0/stat.c nem található — ownership-patch kihagyva" >&2
fi

# ─── fake_id0: xattr-backed PERSISTENT ownership (+ -DUSERLAND below) ──────
# A fenti euid-fix csak az initdb-féle `st_uid == geteuid()` ellenőrzést oldja
# meg (hívófüggő). A Debian root-wrapperek (pg_ctlcluster: „must not be owned
# by root") rootként statolnak, és a faked tulajdon hívófüggősége miatt rootot
# látnak. Ehhez PERZISZTENS, hívófüggetlen tulajdon kell. A USERLAND meta-mód
# ezt megadja, de fájlonként `.proot-meta-file.*` kísérőfájllal teleszórja a
# rootfs-t. Ez a patch a meta-tárolást egy `user.proot.meta` xattr-be teszi a
# fájlra magára → nulla kísérőfájl, ugyanaz a perzisztencia. A statx-ágat is
# kiegészíti (a modern glibc `stat()` azt hívja). A tárolási logikát host-on
# verifikáltuk: patches/fakeid0-xattr-test/run.sh.
if [[ -f "${SCRIPT_DIR}/patches/fakeid0-xattr.py" ]]; then
  echo "[build-proot] patch fake_id0 -> xattr-backed persistent ownership"
  python3 "${SCRIPT_DIR}/patches/fakeid0-xattr.py" "${PROOT_SRC}"
else
  echo "[build-proot] HIBA: patches/fakeid0-xattr.py nem található" >&2; exit 8
fi

echo "[build-proot] make -C ${PROOT_SRC} (NDK cross-compile)"
# A Termux fork néhány extension-ja (ashmem_memfd, fake_id0) missing-include-okat
# tartalmaz amik NDK clang 18+ strict-mode-on (-Werror=implicit-function-declaration)
# ERROR-rel halnak el. Lekapcsoljuk warningra.
make -C "${PROOT_SRC}" -j"$(nproc)" \
    CC="${CC}" \
    LD="${LD}" \
    AR="${AR}" \
    OBJCOPY="${OBJCOPY}" \
    OBJDUMP="${OBJDUMP}" \
    STRIP="${NDK_BIN}/llvm-strip" \
    HOST_CC="cc" \
    CFLAGS="-O2 -DUSERLAND ${TALLOC_INCLUDE_FLAG} -DGIT_VERSION=\"v${PROOT_VERSION}\" -Wno-error=implicit-function-declaration -Wno-error=incompatible-function-pointer-types -Wno-error=int-conversion" \
    LDFLAGS="-L${TALLOC_OUT} -ltalloc -static-libgcc -Wl,-z,noexecstack" \
    proot

PROOT_BIN="${PROOT_SRC}/proot"
if [[ ! -f "${PROOT_BIN}" ]]; then
  echo "[build-proot] HIBA: nincs proot bin a build végén." >&2
  ls -la "${PROOT_SRC}" >&2 || true
  exit 6
fi

cp -v "${PROOT_BIN}" "${DL_DIR}/proot"
"${NDK_BIN}/llvm-strip" "${DL_DIR}/proot" 2>/dev/null || true
chmod +x "${DL_DIR}/proot"

# Sanity
file "${DL_DIR}/proot" || true
ls -lh "${DL_DIR}/proot"
echo "[build-proot] kész: ${DL_DIR}/proot"
