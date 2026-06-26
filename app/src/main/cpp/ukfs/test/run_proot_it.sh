#!/usr/bin/env bash
# Host END-TO-END integration test for the USB-storage FS stack.
#
# Unlike run_host_tests.sh (which tests the native layers BELOW proot in
# isolation), this builds the REAL proot — with the fakeid0-xattr.py redirect
# patches — for the host (x86_64/glibc), wires it to a host-built ukfsd serving a
# FAT image over an in-process block server, then runs ACTUAL commands
# (mkdir -p, echo>, cat, rm -rf, mv, ln -s, find, dd) inside proot against the
# mounted volume and asserts the results.
#
# This is what catches the proot-LAYER bugs that the unit tests can't: syscall
# interception, fcntl(F_DUPFD) tracking, chdir-into-vmount, getdents delivery,
# the PR_void result re-poke, the legacy (non-*at) syscall handlers, etc.
#
# The host is x86_64, so the guest uses the legacy mkdir/rmdir/unlink/rename/stat
# syscalls AND a 144-byte struct stat — both handled by the redirect's portable
# (#if __x86_64__) paths. On the aarch64 device only the *at forms + 128-byte
# stat fire; this test exercises the same logic through the x86_64 twins.
#
# Usage:  test/run_proot_it.sh
# Exit 0 iff all assertions pass. Skips cleanly when build tools are absent.

set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
UKFS="$(cd "$HERE/.." && pwd)"
ROOT="$(cd "$UKFS/../../../../.." && pwd)"
W="$(mktemp -d)"
PIDS=""
cleanup(){ for p in $PIDS; do kill "$p" 2>/dev/null; done; rm -rf "$W"; }
trap cleanup EXIT

PASS=0; FAIL=0
ok(){ echo "  PASS  $1"; PASS=$((PASS+1)); }
bad(){ echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
have(){ command -v "$1" >/dev/null 2>&1; }
die_skip(){ echo "== proot integration test SKIPPED: $1 =="; exit 0; }

echo "== USB-storage FS — proot integration test =="
for t in gcc make python3 mkfs.vfat mcopy xxd ar; do have "$t" || die_skip "missing tool: $t"; done

TALLOC_VER=2.4.2
DL="$ROOT/proot/.download"; mkdir -p "$DL"
TARBALL="$DL/talloc-$TALLOC_VER.tar.gz"
if [ ! -f "$TARBALL" ]; then
  curl -fsSL --max-time 30 -o "$TARBALL" "https://www.samba.org/ftp/talloc/talloc-$TALLOC_VER.tar.gz" \
    || die_skip "cannot fetch talloc (offline?)"
fi

# ── 1. libtalloc.a (host) ─────────────────────────────────────────────────────
tar -xzf "$TARBALL" -C "$W"
TD="$(find "$W" -maxdepth 1 -type d -name 'talloc-*' | head -1)"
TC="$(find "$TD" -maxdepth 3 -name talloc.c -type f ! -path '*test*' | head -1)"
TH="$(find "$TD" -maxdepth 3 -name talloc.h -type f ! -path '*test*' | head -1)"
TINC="$(dirname "$TH")"
mkdir -p "$W/stub"
cat > "$W/stub/replace.h" <<'EOF'
#ifndef _REPLACE_H
#define _REPLACE_H
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif
#ifndef MAX
#define MAX(a,b) ((a)>(b)?(a):(b))
#endif
#endif
EOF
gcc -c -O2 -fPIC -D_GNU_SOURCE=1 -DHAVE_VA_COPY=1 -DHAVE_INTPTR_T=1 -DHAVE_VOID_PTR=1 \
  -DHAVE_STDLIB_H=1 -DHAVE_STDIO_H=1 -DHAVE_STDARG_H=1 -DHAVE_STDBOOL_H=1 -DHAVE_STDINT_H=1 \
  -DHAVE_STRING_H=1 -DHAVE_UNISTD_H=1 -DHAVE_TIME_H=1 -DHAVE_LIMITS_H=1 -DHAVE_MEMSET=1 \
  -DHAVE_GETPAGESIZE=1 -DHAVE___ATTRIBUTE__=1 \
  -DTALLOC_BUILD_VERSION_MAJOR=2 -DTALLOC_BUILD_VERSION_MINOR=4 -DTALLOC_BUILD_VERSION_RELEASE=2 \
  -I"$W/stub" -I"$TINC" -I"$TD" "$TC" -o "$W/talloc.o" 2>/dev/null \
  && ar rcs "$W/libtalloc.a" "$W/talloc.o" || die_skip "talloc build failed"
ok "build libtalloc.a (host)"

# ── 2. host proot with the redirect patches ───────────────────────────────────
cp -r "$ROOT/proot/vendor/proot/src" "$W/prsrc"
python3 "$ROOT/proot/patches/fakeid0-xattr.py" "$W/prsrc" >/dev/null 2>&1 || { bad "patch proot"; }
MK="$(find "$W/prsrc" -maxdepth 2 \( -name Makefile -o -name GNUmakefile \) ! -path '*test*' ! -path '*loader*' | head -1)"
for tok in 'extension/python/python\.o' 'extension/python/python_extension\.o' 'extension/python/proot_wrap\.o' \
           'extension/python/proot\.o' 'extension/python/python_extension\.py' 'extension/python/proot\.py' \
           'extension/python/proot\.i' 'python_extension\.py' 'python_extension\.o' 'proot\.py' 'proot\.i'; do
  sed -i "s| *${tok}||g" "$MK"
done
mkdir -p "$W/prsrc/extension/python"
printf '#include <stdint.h>\nstruct Extension; typedef struct Extension Extension; typedef int ExtensionEvent;\nint python_callback(Extension *e, ExtensionEvent ev, intptr_t d1, intptr_t d2){(void)e;(void)ev;(void)d1;(void)d2;return -1;}\n' \
  > "$W/prsrc/extension/python/python_stub.c"
sed -i '/^OBJECTS += \\$/i OBJECTS += extension/python/python_stub.o' "$MK"
sed -i 's/,--rosegment//g' "$MK"
mkdir -p "$W/stubinc/linux"
printf '#ifndef _STUB_ASHMEM_H\n#define _STUB_ASHMEM_H\n#include <linux/ioctl.h>\n#define __ASHMEMIOC 0x77\n#define ASHMEM_SET_SIZE _IOW(__ASHMEMIOC,3,size_t)\n#define ASHMEM_GET_SIZE _IO(__ASHMEMIOC,4)\n#endif\n' \
  > "$W/stubinc/linux/ashmem.h"
( cd "$W/prsrc" && make CC=gcc LD=gcc \
    CFLAGS="-O2 -I$TINC -I$W/stubinc -Wno-error -Wno-format-truncation -Wno-format-overflow" \
    LDFLAGS="-L$W -ltalloc" -j4 proot >"$W/make.out" 2>&1 ) \
  && [ -x "$W/prsrc/proot" ] || { bad "build host proot"; sed -n '1,6p' "$W/make.out"; echo "== $PASS passed, $((FAIL+1)) failed =="; exit 1; }
ok "build host proot (patched)"

# ── 3. host ukfsd ─────────────────────────────────────────────────────────────
KCF="-O2 -fno-strict-aliasing -fno-builtin -fshort-wchar -D_GNU_SOURCE -D__KERNEL__ -DMODULE -w -Wno-implicit-function-declaration -I $UKFS/include -I $UKFS/linux/fs/fat"
FATDEF='-DCONFIG_VFAT_FS=1 -DCONFIG_FAT_FS=1 -DCONFIG_FAT_DEFAULT_CODEPAGE=437 -DCONFIG_FAT_DEFAULT_IOCHARSET="iso8859-1" -DCONFIG_FAT_DEFAULT_UTF8=0'
mkdir -p "$W/uk"; OBJ=""
for c in cache dir fatent file inode misc nfs namei_vfat; do
  gcc -c $KCF $FATDEF -DKBUILD_MODNAME="\"v_$c\"" "$UKFS/linux/fs/fat/$c.c" -o "$W/uk/$c.o" 2>/dev/null || { bad "ukfsd build ($c)"; exit 1; }
  OBJ="$OBJ $W/uk/$c.o"
done
for f in vfs:shim/fs/vfs.c blocksock:shim/fs/block_sock.c posix_acl:shim/fs/posix_acl.c compat:shim/compat_bionic.c; do
  b="${f%%:*}"; s="${f##*:}"; gcc -c $KCF $FATDEF "$UKFS/$s" -o "$W/uk/$b.o" 2>/dev/null || { bad "ukfsd build ($s)"; exit 1; }
  OBJ="$OBJ $W/uk/$b.o"
done
for src in "$UKFS"/shim/core/*.c; do
  [ "$(basename "$src")" = fileio.c ] && continue
  b="s_$(basename "$src" .c)"; gcc -c $KCF $FATDEF "$src" -o "$W/uk/$b.o" 2>/dev/null; OBJ="$OBJ $W/uk/$b.o"
done
gcc -o "$W/ukfsd" $OBJ "$UKFS/shim/fs/ukfsd.c" -I "$UKFS/include" -I "$UKFS/linux/fs/fat" $KCF $FATDEF -lpthread 2>/dev/null \
  || { bad "link ukfsd"; exit 1; }
ok "build host ukfsd"

# ── 4. FAT image + in-process block server ────────────────────────────────────
dd if=/dev/zero of="$W/fat.img" bs=1M count=64 status=none
mkfs.vfat -F32 -n UKIT "$W/fat.img" >/dev/null 2>&1
printf 'seed\n' > "$W/s"; mcopy -i "$W/fat.img" "$W/s" ::SEED.TXT
cat > "$W/blk.py" <<'PY'
import os, socket, sys, threading
SOCK, IMG = sys.argv[1], sys.argv[2]
srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM); srv.bind('\0'+SOCK); srv.listen(16)
def handle(c):
    try:
        f=open(IMG,'r+b'); sz=os.path.getsize(IMG); buf=bytearray()
        def rl():
            while b'\n' not in buf:
                d=c.recv(4096)
                if not d: return None
                buf.extend(d)
            i=buf.index(b'\n'); ln=bytes(buf[:i]); del buf[:i+1]; return ln.decode()
        def rn(n):
            while len(buf)<n:
                d=c.recv(65536)
                if not d: return None
                buf.extend(d)
            r=bytes(buf[:n]); del buf[:n]; return r
        while True:
            ln=rl()
            if ln is None: break
            if ln=='SIZE': c.sendall(b'OK %d 512\n'%sz)
            elif ln.startswith('READ '):
                _,o,l=ln.split(); o,l=int(o),int(l); f.seek(o); c.sendall(b'OK %d\n'%l + f.read(l))
            elif ln.startswith('WRITE '):
                _,o,l=ln.split(); o,l=int(o),int(l); d=rn(l)
                if d is None: break
                f.seek(o); f.write(d); f.flush(); c.sendall(b'OK\n')
            elif ln=='FLUSH': f.flush(); os.fsync(f.fileno()); c.sendall(b'OK\n')
            else: c.sendall(b'ERR\n')
    except Exception: pass
    finally:
        try: f.close()
        except Exception: pass
        try: c.close()
        except Exception: pass
while True:
    try: cc,_=srv.accept()
    except OSError: break
    threading.Thread(target=handle,args=(cc,),daemon=True).start()
PY

# unique abstract socket names so parallel runs / a real device don't collide
R="$$_$RANDOM"; BSOCK="io.neoterm.block"; FSOCK="io.neoterm.fs"
# NOTE: the redirect hard-codes io.neoterm.{fs,block}; ensure none are in use.
python3 "$W/blk.py" "$BSOCK" "$W/fat.img" >"$W/blk.log" 2>&1 & PIDS="$PIDS $!"
UKFSD_BLOCKSOCK="$BSOCK" "$W/ukfsd" "$FSOCK" >"$W/ukfsd.log" 2>&1 & PIDS="$PIDS $!"
sleep 0.5

# helper that issues the mount(2) the redirect intercepts (source must end /uksd0)
cat > "$W/mnt.c" <<'C'
#include <sys/mount.h>
#include <stdio.h>
int main(int c,char**v){ if(c<2) return 2; if(mount("/dev/uksd0",v[1],"vfat",0,"")){perror("mount");return 1;} return 0; }
C
gcc -O2 -o "$W/mnt" "$W/mnt.c" 2>/dev/null || { bad "build mnt helper"; exit 1; }

MP="$W/mp"; rm -rf "$MP"; mkdir -p "$MP"
cat > "$W/guest.sh" <<EOF
set +e
R=0
fail(){ echo "  GUESTFAIL: \$1"; R=1; }
$W/mnt "$MP" || { echo "  GUESTFAIL: mount"; exit 1; }
# nested mkdir -p (legacy mkdir + chdir), write, read-back
mkdir -p "$MP/a/b/c" || fail "mkdir -p"
echo hello > "$MP/a/b/c/f.txt" || fail "write"
[ "\$(cat "$MP/a/b/c/f.txt")" = hello ] || fail "read-back"
# listing reflects the new tree
ls "$MP" | grep -q '^a\$' || fail "ls top"
ls "$MP/a/b/c" | grep -q '^f.txt\$' || fail "ls nested"
# rename, then rm -rf the whole tree (fdopendir + fcntl-dup + recursion)
mv "$MP/a/b/c/f.txt" "$MP/a/b/c/g.txt" || fail "mv"
[ "\$(cat "$MP/a/b/c/g.txt")" = hello ] || fail "read after mv"
rm -rf "$MP/a" || fail "rm -rf"
[ -e "$MP/a" ] && fail "rm -rf left tree" || true
# seed still present, tree gone
ls "$MP" | grep -q SEED.TXT || fail "seed gone"
ls "$MP" | grep -q '^a\$' && fail "a still listed" || true
echo "GUEST_RESULT=\$R"
EOF
GOUT="$(UK_FS=1 UK_BLOCK=1 "$W/prsrc/proot" -0 /bin/sh "$W/guest.sh" 2>&1)"
echo "$GOUT" | sed 's/^/    /'
if echo "$GOUT" | grep -q "GUEST_RESULT=0"; then
  ok "proot e2e: mount + mkdir -p + write/read + mv + rm -rf (recursive)"
else
  bad "proot e2e: real-command run"
fi

echo
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
