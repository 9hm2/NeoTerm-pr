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
# Fake kernel headers come FIRST (-I) so <linux/*>,<net/*> shadow the distro's
# real ones; the sysroot still provides <stdint.h>, <stddef.h> etc. This mirrors
# the ukwifi_kshim group in ukwifi.cmake exactly (same -Wno set, same -ffree-
# standing intent), just with the guest's native cc instead of the NDK clang.
CFLAGS=(
  -fPIC -O2 -fno-strict-aliasing -ffreestanding -fno-stack-protector
  -fno-common -D_GNU_SOURCE -DKBUILD_MODNAME="\"$OUT\""
  -DUKERNEL_DRIVER_BUILD -DCONFIG_IOCTL_CFG80211 -DRTW_USE_CFG80211_STA_EVENT
  -I"$SHIM_INC"                     # fake kernel API — must win
  -I"$SRC" -I"$SRC/include" -I"$SRC/core" -I"$SRC/os_dep" -I"$SRC/hal"
  # kernel-ism suppressions (clang-on-NDK set; harmless on gcc)
  -Wno-implicit-function-declaration -Wno-incompatible-pointer-types
  -Wno-unused -Wno-unused-parameter -Wno-sign-compare -Wno-missing-braces
  -Wno-implicit-fallthrough -Wno-unknown-pragmas -Wno-macro-redefined
  "${EXTRA[@]}"
)

# --- collect sources -------------------------------------------------------
# Build every .c under the driver tree. Vendor trees are big but self-contained;
# the kernel symbols they reference resolve at load time from the daemon.
mapfile -d '' SRCS < <(find "$SRC" -name '*.c' -print0)
[ "${#SRCS[@]}" -gt 0 ] || die "no .c files under $SRC"
echo "build-driver: ${#SRCS[@]} source files"

WORK="$(mktemp -d)"; trap 'rm -rf "$WORK"' EXIT
OBJS=()
i=0
for s in "${SRCS[@]}"; do
  o="$WORK/obj_$i.o"; i=$((i+1))
  if ! "$CC" "${CFLAGS[@]}" -c "$s" -o "$o" 2>"$WORK/err"; then
    echo "build-driver: FAILED on $s" >&2; sed 's/^/  /' "$WORK/err" >&2
    die "compile error (often a CONFIG_* you must add as -D…, see wifi/BUILD-DRIVER.md)"
  fi
  OBJS+=("$o")
done

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
