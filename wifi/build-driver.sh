#!/usr/bin/env bash
# uKernel Wi-Fi — build a chip's vendor driver into a uKernel-loadable .so,
# *inside the proot guest* (no NDK dev host, no cross-compiler).
#
# WHY a guest build with -nostdlib:
#   The .so is dlopen()'d by the app-side daemon (libukwifid.so), which runs on
#   BIONIC. A normal `gcc -shared` in a glibc distro (Kali/Debian) produces a
#   glibc-linked .so → the bionic daemon can't load it (NEEDS libc.so.6, glibc
#   versioned symbols). But kernel driver code never calls libc — it only calls
#   the kernel API, which the daemon exports (cfg80211/usb_core/skbuff/printk/…
#   via -Wl,--export-dynamic). So we link the driver `-nostdlib`: the result has
#   NO libc dependency and loads cleanly under bionic. The few freestanding
#   helpers the compiler emits (memcpy/memset/memmove) are resolved from the
#   daemon's bionic libc at dlopen via the global scope. Unresolved kernel
#   symbols are left to load-time (--unresolved-symbols=ignore-all) — they bind
#   to the daemon's shim when the .so is actually loaded by modprobe.
#
# RESULT: a guest-native aarch64 .so, libc-agnostic, loadable by the bionic
#   daemon. Drop it in $UK_WIFI_MODDIR (the guest's /lib/ukwifi) and `insmod`.
#
# Usage:
#   wifi/build-driver.sh <driver-src-dir> [out-name] [extra CFLAGS…]
# Example (RTL8812AU / RTL8811AU / RTL8821AU, aircrack-ng/rtl8812au tree):
#   wifi/build-driver.sh ~/rtl8812au rtl8812au \
#     -DCONFIG_RTL8812A -DCONFIG_RTL8821A
#
# Env overrides:
#   CC          C compiler (default: cc — the guest's native aarch64 toolchain)
#   SHIM_INC    uKernel fake-kernel header tree
#               (default: ./app/src/main/cpp/ukfs/include from this checkout;
#                set it if you only copied the headers into the guest)
#   MODDIR      install dir for the .so (default: /lib/ukwifi == UK_WIFI_MODDIR)
#   KVER        kernel release for the .ko name-carrier (default: `uname -r`)
set -euo pipefail

die() { echo "build-driver: $*" >&2; exit 1; }

SRC="${1:-}"; [ -n "$SRC" ] || die "usage: build-driver.sh <driver-src-dir> [out-name] [CFLAGS…]"
[ -d "$SRC" ] || die "driver source dir not found: $SRC"
shift
OUT="${1:-driver}"; [ "${1:-}" ] && shift || true
EXTRA=("$@")

CC="${CC:-cc}"
command -v "$CC" >/dev/null 2>&1 || die "no C compiler ('$CC'); in the guest: apt install build-essential"

# Locate the shim headers. Prefer an explicit $SHIM_INC; else this repo checkout.
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SHIM_INC="${SHIM_INC:-$HERE/app/src/main/cpp/ukfs/include}"
[ -f "$SHIM_INC/net/cfg80211.h" ] || die "shim headers not found at $SHIM_INC
  (clone the repo in the guest, or copy app/src/main/cpp/ukfs/include there and
   point SHIM_INC at it)"

MODDIR="${MODDIR:-/lib/ukwifi}"
KVER="${KVER:-$(uname -r)}"

echo "build-driver: CC=$CC"
echo "build-driver: src=$SRC  out=$OUT.so  shim=$SHIM_INC"
echo "build-driver: install -> $MODDIR/$OUT.so   ko-name -> /lib/modules/$KVER"

# --- compile flags ---------------------------------------------------------
# Auto-discover the driver's OWN include dirs: vendor trees do `#include
# "halrf/foo.h"` etc. and rely on their Kbuild adding a long -I list (hal/phydm,
# hal/phydm/halrf, …). Instead of hard-coding per chip, add EVERY directory that
# contains a header — chip-agnostic. These come AFTER $SHIM_INC so the shim still
# wins for the kernel API (<linux/*>,<net/*>); they only satisfy the driver's
# internal relative includes.
DRV_INC=()
while IFS= read -r -d '' d; do DRV_INC+=( -I"$d" ); done \
  < <(find "$SRC" -type f \( -name '*.h' -o -name '*.hpp' \) -printf '%h\0' | sort -zu)
echo "build-driver: ${#DRV_INC[@]} driver include dirs"

# Realtek vendor-tree defaults — the EXACT flags the proven uKernel build used
# (build/rtl8812au.mk). The critical one is DM_ODM_SUPPORT_TYPE=0x04 (ODM_CE =
# Customer-Embedded = Linux): without it the phydm HAL's enums/struct fields
# don't assemble and ~all hal/phydm/*.c fail ("enumerator value … not an integer
# constant", "field 'mpt_dig_timer' has incomplete type", "ASSOCIATE_ENTRY_NUM
# undeclared"). These macros are Realtek-specific and inert for non-Realtek
# drivers; override the whole group with $RTW_CONFIG="" if your chip differs.
RTW_CONFIG="${RTW_CONFIG:- -DDM_ODM_SUPPORT_TYPE=0x04 -DCONFIG_LITTLE_ENDIAN \
  -DCONFIG_TXPWR_BY_RATE=1 -DCONFIG_WIFI_MONITOR -DCONFIG_MONITOR_MODE_XMIT \
  -DDRV_NAME=\"$OUT\" -DEFUSE_MAP_PATH=\"$MODDIR/$OUT.efuse\"}"

# Fake kernel headers come FIRST (-I) so <linux/*>,<net/*> shadow the distro's
# real ones; the sysroot still provides <stdint.h>, <stddef.h> etc. -O2 (NOT -O0:
# the proven build NULL-derefs at -O0 in _init_timer). Mirrors rtl8812au.mk; the
# user's chip-select (-DCONFIG_RTL8812A …) comes via "${EXTRA[@]}" and wins last.
CFLAGS=(
  -fPIC -O2 -fno-strict-aliasing -fno-stack-protector
  -fno-common -D_GNU_SOURCE -DKBUILD_MODNAME="\"$OUT\""
  -DUKERNEL_DRIVER_BUILD -DCONFIG_IOCTL_CFG80211
  $RTW_CONFIG
  -I"$SHIM_INC"                     # fake kernel API — must win
  "${DRV_INC[@]}"                   # driver's own header dirs (auto-discovered)
  # kernel-ism suppressions (clang-on-NDK set; harmless on gcc)
  -Wno-implicit-function-declaration -Wno-incompatible-pointer-types
  -Wno-unused -Wno-unused-parameter -Wno-sign-compare -Wno-missing-braces
  -Wno-implicit-fallthrough -Wno-unknown-pragmas -Wno-macro-redefined
  "${EXTRA[@]}"
)

# --- collect sources -------------------------------------------------------
# A vendor tree ships sources for OTHER buses/platforms/chips that its Kbuild
# never compiles for a USB STA build (selected by CONFIG_*). Building them all
# fails on headers we don't have (mach/*.h, linux/mmc/sdio_func.h, linux/jhash.h)
# or test-only types (PMPT_CONTEXT, NDIS_STATUS, sint/BOOLEAN). The proven uKernel
# build compiled a curated ~145-file subset, not the full 209. Mirror that by
# excluding the non-target categories — same effect as the vendor Kbuild gates:
#   platform/*          -> CONFIG_PLATFORM_*   (we're generic)
#   *sdio*/*gspi*/*pci*  -> bus != USB
#   rhashtable.c        -> CONFIG_RTW_MESH
#   rtw_mp*/rtw_bt_mp/rtw_eeprom/rtw_ioctl_rtl -> CONFIG_MP_INCLUDED / legacy
# Override with $EXCLUDE (an extended-regex over full paths); set EXCLUDE='' to
# build everything (e.g. a non-Realtek driver that needs no curation).
DEFAULT_EXCLUDE='/platform/|sdio|gspi|_pci|pcie|/rhashtable\.c|/rtw_mp\.c|/rtw_mp_ioctl\.c|/rtw_bt_mp\.c|/rtw_eeprom\.c|/rtw_ioctl_rtl\.c'
EXCLUDE="${EXCLUDE-$DEFAULT_EXCLUDE}"
SRCS=()
nexcl=0
while IFS= read -r -d '' s; do
  if [ -n "$EXCLUDE" ] && printf '%s' "$s" | grep -Eq "$EXCLUDE"; then
    nexcl=$((nexcl+1)); continue
  fi
  SRCS+=("$s")
done < <(find "$SRC" -name '*.c' -print0)
[ "${#SRCS[@]}" -gt 0 ] || die "no .c files under $SRC (after exclude)"
echo "build-driver: ${#SRCS[@]} source files ($nexcl excluded as non-target; \$EXCLUDE)"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
OBJS=()
FAILED=0
i=0
for s in "${SRCS[@]}"; do
  o="$WORK/obj_$i.o"; i=$((i+1))
  if "$CC" "${CFLAGS[@]}" -c "$s" -o "$o" 2>"$WORK/err.$i"; then
    OBJS+=("$o")
  else
    FAILED=$((FAILED+1))
    echo "$s" >> "$WORK/failed"
    # keep only the hard errors (drop the warning noise) for the summary
    grep -E 'error:|fatal error:' "$WORK/err.$i" | head -3 \
      | sed "s|^|  [$(basename "$s")] |" >> "$WORK/errsum" || true
  fi
done

# Report ALL compile failures at once (don't die on the first) so one run
# surfaces every distinct missing-header / missing-CONFIG, not one per re-run.
if [ "$FAILED" -gt 0 ]; then
  echo >&2
  echo "build-driver: $FAILED/$i source(s) failed to compile:" >&2
  sort -u "$WORK/errsum" >&2 2>/dev/null || cat "$WORK/errsum" >&2
  echo >&2
  echo "  Common causes (see wifi/BUILD-DRIVER.md → Troubleshooting):" >&2
  echo "    'fatal error: X/Y.h: No such file' -> a driver header dir wasn't" >&2
  echo "      auto-added (rare); pass it as an extra flag: -I\$SRC/<dir>" >&2
  echo "    'undeclared CONFIG_FOO' / a whole feature file errors -> add" >&2
  echo "      -DCONFIG_FOO, or exclude that source (point the script at a subdir)" >&2
  die "compile errors above; nothing linked"
fi

# --- link: shared, libc-agnostic ------------------------------------------
"$CC" -shared -nostdlib -fPIC -o "$WORK/$OUT.so" "${OBJS[@]}" \
  -Wl,--unresolved-symbols=ignore-all -Wl,-soname,"$OUT.so"

mkdir -p "$MODDIR"
install -m 0644 "$WORK/$OUT.so" "$MODDIR/$OUT.so"
echo "build-driver: installed $MODDIR/$OUT.so"

# --- .ko name-carrier ------------------------------------------------------
# insmod/modprobe in the guest call finit_module(fd) on a real file; the proot
# UK_WIFI redirect reads the fd's path, extracts "<name>", and tells the daemon
# to dlopen $UK_WIFI_MODDIR/<name>.so. So the .ko need only EXIST with the right
# name — its bytes are never used. (modprobe also wants it under /lib/modules.)
KODIR="/lib/modules/$KVER/kernel/drivers/net/wireless/ukwifi"
mkdir -p "$KODIR"
: > "$KODIR/$OUT.ko"
( cd "/lib/modules/$KVER" && depmod -a "$KVER" 2>/dev/null ) || true
echo "build-driver: ko-name carrier $KODIR/$OUT.ko"
echo
echo "Done. Load it from the guest:"
echo "    insmod  $KODIR/$OUT.ko        # or: modprobe $OUT"
echo "    lsmod   | grep $OUT"
echo "    iw dev  ;  ip link            # wlan0 should appear"
