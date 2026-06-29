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
# or other-platform types (PDM_ODM_T, PWRTRACK_METHOD, struct rtl8192cd_priv,
# CamelCase Windows members). The proven uKernel build compiled a curated subset.
# Mirror that by excluding the non-target categories — same effect as the vendor
# Kbuild gates:
#   platform/*               -> CONFIG_PLATFORM_*   (we're generic)
#   *sdio*/*gspi*/*pci*       -> bus != USB
#   rhashtable.c             -> CONFIG_RTW_MESH
#   rtw_mp*/rtw_bt_mp/rtw_eeprom/rtw_ioctl_rtl -> CONFIG_MP_INCLUDED / legacy
#   8814a                    -> CONFIG_RTL8814A not selected (a different chip)
#   halrf*_ap/_win/_iot      -> phydm RF platform variants for AP(router-SDK) /
#                               WIN(Windows) / IOT; we build CE (the *_ce.c ones)
#   hal_halmac / halmac/     -> halmac engine (8822b/8821c-class chips)
#   halrf_txgapcal           -> 8814a-era TX-gap cal (non-CE PDM_ODM_T)
# Override with $EXCLUDE (an extended-regex over full paths); set EXCLUDE='' to
# build everything (e.g. a non-Realtek driver that needs no curation). For a
# DIFFERENT Realtek chip, drop its tag from here and add -DCONFIG_RTL<chip>.
DEFAULT_EXCLUDE='/platform/|sdio|gspi|_pci|pcie|/rhashtable\.c'
DEFAULT_EXCLUDE="$DEFAULT_EXCLUDE"'|/rtw_mp\.c|/rtw_mp_ioctl\.c|/rtw_bt_mp\.c|/rtw_eeprom\.c|/rtw_ioctl_rtl\.c'
DEFAULT_EXCLUDE="$DEFAULT_EXCLUDE"'|8814a|hal_halmac\.c|/halmac/|halrf_txgapcal\.c|hal(rf|phyrf)[a-z0-9_]*_(ap|win|iot)\.c'
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

# --- .ko name-carrier (compat) ---------------------------------------------
# A minimal, kmod-parseable ELF .ko so depmod indexes the module and anything
# that inspects /lib/modules (modinfo, depmod) is happy. NOTE: it is NOT the load
# path — see the wrapper section below. On Android the finit_module/init_module
# syscalls are blocked by the app-sandbox seccomp filter (SECCOMP_RET_ERRNO ENOSYS),
# whose action outranks proot's SECCOMP_RET_TRACE, so NO userspace tracer (proot)
# can ever intercept them. The guest therefore loads the driver via the wrapper
# below, which talks to the daemon directly. The carrier stays for tooling compat.
KODIR="/lib/modules/$KVER/kernel/drivers/net/wireless/ukwifi"
mkdir -p "$KODIR"
cat > "$WORK/carrier.c" <<EOF
__attribute__((section(".modinfo"),used)) static const char _kn[]  = "name=$OUT";
__attribute__((section(".modinfo"),used)) static const char _kv[]  = "vermagic=$KVER SMP preempt mod_unload aarch64";
__attribute__((section(".modinfo"),used)) static const char _kd[]  = "depends=";
__attribute__((section(".modinfo"),used)) static const char _kl[]  = "license=GPL";
EOF
if "$CC" -c -fno-stack-protector -o "$KODIR/$OUT.ko" "$WORK/carrier.c" 2>"$WORK/koerr"; then
  echo "build-driver: ko-name carrier $KODIR/$OUT.ko (minimal ELF, kmod-parseable)"
else
  sed 's/^/  /' "$WORK/koerr" >&2
  : > "$KODIR/$OUT.ko"
  echo "build-driver: WARN could not compile ELF carrier; wrote empty .ko (kmod may EINVAL — use the daemon path or report this)" >&2
fi
( cd "/lib/modules/$KVER" && depmod -a "$KVER" 2>/dev/null ) || true
echo "build-driver: ko-name carrier $KODIR/$OUT.ko"

# --- guest modprobe/lsmod/rmmod/insmod wrappers ----------------------------
# The real load path. Since finit_module is seccomp-blocked (see above), the guest
# can't reach the daemon through the proot redirect. But the guest shares the app's
# network namespace, so it can connect to the daemon's abstract socket DIRECTLY
# (@io.neoterm.wifi) using only allowed syscalls (socket/connect/write/read). This
# tiny helper does exactly the UK_OP_MODPROBE/RMMOD/LSMOD protocol the redirect used
# to, and is installed under the module-tool names in /usr/local/sbin (first on the
# guest PATH, so `modprobe rtl8812au` / `lsmod` / `rmmod` / `insmod` just work).
cat > "$WORK/ukmodtool.c" <<'CEOF'
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <libgen.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
enum { OP_MODPROBE=30, OP_RMMOD=31, OP_LSMOD=32 };
struct req { uint32_t op, cmd, len; };
struct rsp { int32_t ret; uint32_t len; };
static int dconn(void){
    int s=socket(AF_UNIX,SOCK_STREAM,0); if(s<0) return -1;
    struct sockaddr_un a; memset(&a,0,sizeof a); a.sun_family=AF_UNIX;
    static const char nm[]="io.neoterm.wifi";
    a.sun_path[0]='\0'; memcpy(a.sun_path+1,nm,sizeof(nm)-1);
    socklen_t L=(socklen_t)(offsetof(struct sockaddr_un,sun_path)+1+sizeof(nm)-1);
    if(connect(s,(struct sockaddr*)&a,L)<0){ close(s); return -1; }
    return s;
}
static int dcall(uint32_t op,const char*p,uint32_t plen,char*out,uint32_t cap,int32_t*pret){
    int s=dconn();
    if(s<0){ fprintf(stderr,"ukmod: a Wi-Fi daemon nem elérhető (@io.neoterm.wifi) — be van kapcsolva a USB Wi-Fi toggle?\n"); return -100; }
    struct req r={op,0,plen}; int rc=-1;
    if(write(s,&r,sizeof r)==(ssize_t)sizeof r && (plen==0 || write(s,p,plen)==(ssize_t)plen)){
        struct rsp rs; if(read(s,&rs,sizeof rs)==(ssize_t)sizeof rs){
            if(pret)*pret=rs.ret; rc=0;
            uint32_t want=rs.len,got=0;
            while(out && got<want && got<cap){ ssize_t k=read(s,out+got,(want<cap?want:cap)-got); if(k<=0)break; got+=(uint32_t)k; }
            if(out && cap) out[got<cap?got:cap-1]='\0';
        }
    }
    close(s); return rc;
}
static void modname(const char*in,char*out,size_t osz){
    char tmp[1024]; snprintf(tmp,sizeof tmp,"%s",in?in:""); char*b=basename(tmp);
    char*dot=strchr(b,'.'); if(dot)*dot='\0'; snprintf(out,osz,"%s",b);
}
int main(int argc,char**argv){
    char a0[1024]; snprintf(a0,sizeof a0,"%s",argv[0]); char tool[256]; snprintf(tool,sizeof tool,"%s",basename(a0));
    int remove=0; const char*pos=NULL;
    for(int i=1;i<argc;i++){
        if(argv[i][0]=='-'){ if(!strcmp(argv[i],"-r")||!strcmp(argv[i],"--remove")) remove=1; continue; }
        if(!pos) pos=argv[i];
    }
    if(!strcmp(tool,"lsmod")){
        char buf[1<<16]; int32_t ret=0;
        if(dcall(OP_LSMOD,NULL,0,buf,sizeof buf,&ret)!=0) return 1;
        fputs("Module                  Size  Used by\n",stdout); fputs(buf,stdout); return 0;
    }
    char nm[256]={0}; if(pos) modname(pos,nm,sizeof nm);
    if(!strcmp(tool,"rmmod") || (!strcmp(tool,"modprobe") && remove)){
        if(!nm[0]){ fprintf(stderr,"%s: hiányzó modulnév\n",tool); return 1; }
        int32_t ret=0; if(dcall(OP_RMMOD,nm,strlen(nm),NULL,0,&ret)<0) return 1;
        if(ret!=0){ fprintf(stderr,"rmmod: %s: nincs betöltve\n",nm); return 1; } return 0;
    }
    /* modprobe / insmod -> load */
    if(!nm[0]){ fprintf(stderr,"%s: hiányzó modul%s\n",tool,!strcmp(tool,"insmod")?"fájl":"név"); return 1; }
    int32_t ret=0; if(dcall(OP_MODPROBE,nm,strlen(nm),NULL,0,&ret)<0) return 1;
    if(ret==0){ if(!strcmp(tool,"insmod")) printf("%s betöltve (uKernel Wi-Fi)\n",nm); return 0; }
    if(ret==-2 && !strcmp(tool,"modprobe")) return 0;   /* not a ukwifi module: no-op (don't break boot/udev) */
    fprintf(stderr,"%s: ERROR: could not insert '%s': daemon ret=%d (lásd: cat /data/data/io.neoterm/files/ukwifid.log)\n",tool,nm,ret);
    return 1;
}
CEOF
if "$CC" -O2 -o "$WORK/ukmodtool" "$WORK/ukmodtool.c" 2>"$WORK/wraperr"; then
  mkdir -p /usr/local/bin /usr/local/sbin
  install -m 0755 "$WORK/ukmodtool" /usr/local/bin/ukmodtool
  for w in modprobe insmod rmmod lsmod; do ln -sf ../bin/ukmodtool "/usr/local/sbin/$w"; done
  echo "build-driver: installed guest wrappers -> /usr/local/sbin/{modprobe,insmod,rmmod,lsmod} (talk to @io.neoterm.wifi)"
else
  sed 's/^/  /' "$WORK/wraperr" >&2
  echo "build-driver: WARN could not build the modprobe wrapper (cc error above); modprobe will NOT reach the daemon" >&2
fi

echo
echo "Done. Load it from the guest (uses the wrappers -> daemon, no kernel module):"
echo "    hash -r              # let the shell pick up /usr/local/sbin/modprobe"
echo "    modprobe $OUT"
echo "    lsmod                # served from the daemon"
echo "    iw dev  ;  ip link   # wlan0 should appear once the chip probes"
echo "    cat /data/data/io.neoterm/files/ukwifid.log   # daemon log (dlopen/probe)"
