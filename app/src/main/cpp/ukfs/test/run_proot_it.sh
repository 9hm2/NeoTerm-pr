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
skip(){ echo "  SKIP  $1 ($2)"; }

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
KCF="-O2 -fno-strict-aliasing -fno-builtin -fshort-wchar -D_GNU_SOURCE -D__KERNEL__ -DMODULE -w -Wno-implicit-function-declaration -I $UKFS/include -I $UKFS/linux/fs/fat -I $UKFS/linux/fs/exfat -I $UKFS/linux/fs/ntfs3 -I $UKFS/linux/fs/ntfs3/lib -I $UKFS/linux/fs/ext4 -I $UKFS/linux/fs/jbd2"
FATDEF='-DCONFIG_VFAT_FS=1 -DCONFIG_FAT_FS=1 -DCONFIG_FAT_DEFAULT_CODEPAGE=437 -DCONFIG_FAT_DEFAULT_IOCHARSET="iso8859-1" -DCONFIG_FAT_DEFAULT_UTF8=0'
EXDEF='-DCONFIG_EXFAT_FS=1 -DCONFIG_EXFAT_DEFAULT_IOCHARSET="utf8"'
NDEF='-DCONFIG_NTFS3_FS=1 -DCONFIG_NTFS3_LZX_XPRESS=1 -DCONFIG_NLS_DEFAULT="utf8"'
E4DEF='-DCONFIG_EXT4_FS=1 -DCONFIG_JBD2=1'
mkdir -p "$W/uk"; OBJ=""
for c in cache dir fatent file inode misc nfs namei_vfat; do
  gcc -c $KCF $FATDEF -DKBUILD_MODNAME="\"v_$c\"" "$UKFS/linux/fs/fat/$c.c" -o "$W/uk/fat_$c.o" 2>/dev/null || { bad "ukfsd build ($c)"; exit 1; }
  OBJ="$OBJ $W/uk/fat_$c.o"
done
for c in balloc cache dir fatent file inode misc namei nls super; do
  gcc -c $KCF $EXDEF -DKBUILD_MODNAME="\"ex_$c\"" "$UKFS/linux/fs/exfat/$c.c" -o "$W/uk/ex_$c.o" 2>/dev/null || { bad "ukfsd build (exfat/$c)"; exit 1; }
  OBJ="$OBJ $W/uk/ex_$c.o"
done
for c in "$UKFS"/linux/fs/ntfs3/*.c; do
  b="$(basename "$c" .c)"; gcc -c $KCF $NDEF -DKBUILD_MODNAME="\"n3_$b\"" "$c" -o "$W/uk/n3_$b.o" 2>/dev/null || { bad "ukfsd build (ntfs3/$b)"; exit 1; }
  OBJ="$OBJ $W/uk/n3_$b.o"
done
for c in "$UKFS"/linux/fs/ntfs3/lib/*.c; do
  b="$(basename "$c" .c)"; gcc -c $KCF $NDEF -include linux/minmax.h -DKBUILD_MODNAME="\"n3lib_$b\"" "$c" -o "$W/uk/n3lib_$b.o" 2>/dev/null || { bad "ukfsd build (ntfs3/lib/$b)"; exit 1; }
  OBJ="$OBJ $W/uk/n3lib_$b.o"
done
for c in "$UKFS"/linux/fs/jbd2/*.c "$UKFS"/linux/fs/ext4/*.c; do
  b="$(basename "$c" .c)"; gcc -c $KCF $E4DEF -DKBUILD_MODNAME="\"e4_$b\"" "$c" -o "$W/uk/e4_$b.o" 2>/dev/null || { bad "ukfsd build (ext4/$b)"; exit 1; }
  OBJ="$OBJ $W/uk/e4_$b.o"
done
for f in vfs:shim/fs/vfs.c blocksock:shim/fs/block_sock.c posix_acl:shim/fs/posix_acl.c ntstub:shim/fs/ntfs3_stubs.c e4stub:shim/fs/ext4_stubs.c compat:shim/compat_bionic.c; do
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
                _,o,l=ln.split(); o,l=int(o),int(l); f.seek(o); d=f.read(l)
                if len(d)<l: d=d+b'\x00'*(l-len(d))   # never short-read: pad past-EOF with zeros (avoid protocol desync)
                c.sendall(b'OK %d\n'%l + d)
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

# NOTE: the redirect hard-codes io.neoterm.{fs,block}; ensure none are in use.
BSOCK="io.neoterm.block"; FSOCK="io.neoterm.fs"

# helper that issues the mount(2) the redirect intercepts (source must end /uksd0).
# The mount fstype is irrelevant (the redirect always sends "MOUNT auto", and ukfsd
# probes vfat/exfat/ntfs3/ext4) — so the same helper drives every filesystem.
cat > "$W/mnt.c" <<'C'
#include <sys/mount.h>
#include <stdio.h>
int main(int c,char**v){ if(c<2) return 2; if(mount("/dev/uksd0",v[1],"vfat",0,"")){perror("mount");return 1;} return 0; }
C
gcc -O2 -o "$W/mnt" "$W/mnt.c" 2>/dev/null || { bad "build mnt helper"; exit 1; }

MP="$W/mp"
# FS-agnostic guest battery: creates its own seed (so it works on exfat too, where
# mtools can't pre-seed), then exercises the real command paths and verifies the
# seed survives the recursive rm of the test tree.
cat > "$W/guest.sh" <<EOF
set +e
R=0
fail(){ echo "  GUESTFAIL: \$1"; R=1; }
$W/mnt "$MP" || { echo "  GUESTFAIL: mount"; exit 1; }
echo seeddata > "$MP/SEED.TXT" || fail "seed write"
mkdir -p "$MP/a/b/c" || fail "mkdir -p"
echo hello > "$MP/a/b/c/f.txt" || fail "write"
[ "\$(cat "$MP/a/b/c/f.txt")" = hello ] || fail "read-back"
ls "$MP" | grep -q '^a\$' || fail "ls top"
ls "$MP/a/b/c" | grep -q '^f.txt\$' || fail "ls nested"
# O_APPEND must land at EOF; fresh read must see grown content (git config pattern)
printf 'L1\n' > "$MP/a/ap.txt" || fail "append-create"
printf 'L2\n' >> "$MP/a/ap.txt" || fail "append-write"
[ "\$(cat "$MP/a/ap.txt" | tr '\\n' ,)" = "L1,L2," ] || fail "append-readback(\$(cat "$MP/a/ap.txt" | tr '\\n' ,))"
[ "\$(wc -c < "$MP/a/ap.txt")" -eq 6 ] || fail "append-size"
mv "$MP/a/b/c/f.txt" "$MP/a/b/c/g.txt" || fail "mv"
[ "\$(cat "$MP/a/b/c/g.txt")" = hello ] || fail "read after mv"
rm -rf "$MP/a" || fail "rm -rf"
[ -e "$MP/a" ] && fail "rm -rf left tree" || true
[ "\$(cat "$MP/SEED.TXT")" = seeddata ] || fail "seed lost"
ls "$MP" | grep -q '^a\$' && fail "a still listed" || true
echo "GUEST_RESULT=\$R"
EOF

# run the guest battery against one image (fresh servers each time, since the
# redirect's sockets are fixed and only one mount can be live at once)
run_e2e() {
  local label="$1" img="$2"
  for p in $PIDS; do kill "$p" 2>/dev/null; done; PIDS=""
  sleep 0.3
  python3 "$W/blk.py" "$BSOCK" "$img" >"$W/blk.log" 2>&1 & PIDS="$PIDS $!"
  UKFSD_BLOCKSOCK="$BSOCK" "$W/ukfsd" "$FSOCK" >"$W/ukfsd_$label.log" 2>&1 & PIDS="$PIDS $!"
  sleep 0.5
  rm -rf "$MP"; mkdir -p "$MP"
  local out; out="$(UK_FS=1 UK_BLOCK=1 "$W/prsrc/proot" -0 /bin/sh "$W/guest.sh" 2>&1)"
  echo "$out" | sed "s/^/    [$label] /"
  if echo "$out" | grep -q "GUEST_RESULT=0"; then
    ok "proot e2e [$label]: mkdir -p + write/read + append + mv + rm -rf"
  else
    bad "proot e2e [$label]: real-command run"
  fi
}

run_e2e vfat "$W/fat.img"
if have mkfs.exfat; then
  dd if=/dev/zero of="$W/exfat.img" bs=1M count=64 status=none
  mkfs.exfat "$W/exfat.img" >/dev/null 2>&1
  run_e2e exfat "$W/exfat.img"
else
  skip "proot e2e [exfat]" "mkfs.exfat not installed"
fi
if have mkfs.ntfs; then
  dd if=/dev/zero of="$W/ntfs.img" bs=1M count=64 status=none
  mkfs.ntfs -F -Q "$W/ntfs.img" >/dev/null 2>&1
  run_e2e ntfs3 "$W/ntfs.img"
else
  skip "proot e2e [ntfs3]" "mkfs.ntfs not installed"
fi
if have mkfs.ext4; then
  dd if=/dev/zero of="$W/ext4.img" bs=1M count=64 status=none
  mkfs.ext4 -q -F "$W/ext4.img" >/dev/null 2>&1
  run_e2e ext4 "$W/ext4.img"
else
  skip "proot e2e [ext4]" "mkfs.ext4 not installed"
fi

# ── multi-partition: a Raspberry-Pi-like MBR image (FAT boot + ext4 root),
#    BOTH partitions mounted SIMULTANEOUSLY in the guest via /dev/uksd0p1 and
#    /dev/uksd0p2, each routed by the redirect to its own ukfsd (io.neoterm.fs.pN). ─
if have mkfs.ext4 && have mcopy && have python3; then
  for p in $PIDS; do kill "$p" 2>/dev/null; done; PIDS=""; sleep 0.3
  P1S=2048; P1N=$((48*1024*1024/512)); P2S=$((P1S+P1N)); P2N=$((48*1024*1024/512)); TOT=$((P2S+P2N+2048))
  dd if=/dev/zero of="$W/pi.img" bs=512 count="$TOT" status=none
  dd if=/dev/zero of="$W/p1.fat" bs=512 count="$P1N" status=none
  mkfs.vfat -F32 -n BOOT "$W/p1.fat" >/dev/null 2>&1
  printf 'console=serial0 root=/dev/mmcblk0p2\n' > "$W/cmdline.txt"
  MTOOLS_SKIP_CHECK=1 mcopy -i "$W/p1.fat" "$W/cmdline.txt" ::CMDLINE.TXT 2>/dev/null
  dd if=/dev/zero of="$W/p2.ext4" bs=512 count="$P2N" status=none
  mkfs.ext4 -q -F -L rootfs "$W/p2.ext4" >/dev/null 2>&1
  python3 - "$W/pi.img" "$W/p1.fat" "$W/p2.ext4" "$P1S" "$P1N" "$P2S" "$P2N" <<'PY'
import struct, sys
img,p1,p2,s1,n1,s2,n2 = sys.argv[1],sys.argv[2],sys.argv[3],*map(int,sys.argv[4:8])
with open(img,'r+b') as o:
    with open(p1,'rb') as f: o.seek(s1*512); o.write(f.read())
    with open(p2,'rb') as f: o.seek(s2*512); o.write(f.read())
    def e(b,t,st,sc): return struct.pack('<B3sB3sII',b,b'\xfe\xff\xff',t,b'\xfe\xff\xff',st,sc)
    o.seek(0x1BE); o.write(e(0x80,0x0C,s1,n1)); o.write(e(0x00,0x83,s2,n2)); o.write(b'\0'*32)
    o.seek(0x1FE); o.write(b'\x55\xaa')
PY
  cat > "$W/mntp.c" <<'C'
#include <sys/mount.h>
#include <stdio.h>
int main(int c,char**v){ if(c<3) return 2; if(mount(v[1],v[2],"auto",0,"")){perror("mount");return 1;} return 0; }
C
  gcc -O2 -o "$W/mntp" "$W/mntp.c" 2>/dev/null || bad "build mntp helper"
  MP1="$W/mp1"; MP2="$W/mp2"; rm -rf "$MP1" "$MP2"; mkdir -p "$MP1" "$MP2"
  cat > "$W/guest_parts.sh" <<EOF
set +e
R=0; fail(){ echo "  GUESTFAIL: \$1"; R=1; }
$W/mntp /dev/uksd0p1 "$MP1" || fail "mount p1"
$W/mntp /dev/uksd0p2 "$MP2" || fail "mount p2"
# p1 (FAT) has the seeded CMDLINE.TXT; p2 (ext4) is fresh
[ -f "$MP1/CMDLINE.TXT" ] || fail "p1 seed missing"
# write to BOTH simultaneously
echo bootdata > "$MP1/extra.txt" || fail "p1 write"
mkdir -p "$MP2/etc" && echo rootdata > "$MP2/etc/hostname" || fail "p2 write"
[ "\$(cat "$MP1/extra.txt")" = bootdata ] || fail "p1 readback"
[ "\$(cat "$MP2/etc/hostname")" = rootdata ] || fail "p2 readback"
# isolation: p1's file must NOT appear on p2 and vice-versa
ls "$MP1" | grep -q hostname && fail "p1 leaked p2 content" || true
ls "$MP2" | grep -q CMDLINE && fail "p2 leaked p1 content" || true
ls "$MP2" | grep -q '^etc\$' || fail "p2 etc missing"
echo "GUEST_RESULT=\$R"
EOF
  python3 "$W/blk.py" "$BSOCK" "$W/pi.img" >"$W/blk_parts.log" 2>&1 & PIDS="$PIDS $!"
  # launch the full daemon pool (the redirect allocates a free daemon per mount)
  for sk in io.neoterm.fs io.neoterm.fs.p1 io.neoterm.fs.p2 io.neoterm.fs.p3 io.neoterm.fs.p4; do
    UKFSD_BLOCKSOCK="$BSOCK" "$W/ukfsd" "$sk" >"$W/ukfsd_$sk.log" 2>&1 & PIDS="$PIDS $!"
  done
  sleep 0.6
  out="$(UK_FS=1 UK_BLOCK=1 "$W/prsrc/proot" -0 /bin/sh "$W/guest_parts.sh" 2>&1)"
  echo "$out" | sed "s/^/    [parts] /"
  if echo "$out" | grep -q "GUEST_RESULT=0"; then
    ok "proot e2e [parts]: /dev/uksd0p1 (FAT) + /dev/uksd0p2 (ext4) mounted SIMULTANEOUSLY"
  else
    bad "proot e2e [parts]: simultaneous multi-partition mount"
  fi
else
  skip "proot e2e [parts]" "mkfs.ext4 / mcopy / python3 missing"
fi

# ── loop devices: mount a DOWNLOADED IMAGE like on a real Linux PC — losetup -fP
#    + mount /dev/loopNp2, and mount -o loop,offset=,sizelimit= -t vfat. The redirect
#    emulates the loop ioctls; ukfsd opens the image's host path directly. ──────────
if [ -f "$W/pi.img" ] && have losetup && have mount; then
  for p in $PIDS; do kill "$p" 2>/dev/null; done; PIDS=""; sleep 0.3
  # daemon pool (loop mounts allocate free daemons; PARTS query needs one too) — no
  # block server needed: loop mounts open the image host path directly.
  for sk in io.neoterm.fs io.neoterm.fs.p1 io.neoterm.fs.p2 io.neoterm.fs.p3 io.neoterm.fs.p4; do
    UKFSD_BLOCKSOCK="$BSOCK" "$W/ukfsd" "$sk" >"$W/ukfsd_$sk.log" 2>&1 & PIDS="$PIDS $!"
  done
  sleep 0.6
  gcc -O2 -o "$W/loopmnt" "$UKFS/test/loopmnt.c" 2>/dev/null || bad "build loopmnt helper"
  # loop device node markers + proot binds. NOTE: we drive the loop ioctls via the
  # loopmnt helper (not the host `losetup`, which would scan sysfs and grab the
  # host's REAL loop devices, bypassing the emulation). A bound /dev/kmsg captures
  # the redirect's debug log.
  : > "$W/kmsg"
  mkdir -p "$W/lmark"
  LB="-b $W/kmsg:/dev/kmsg"
  # marker basename MUST equal the device basename — the redirect identifies the
  # loop device by readlink'ing the fd and parsing the basename.
  mkm() { : > "$W/lmark/$1"; LB="$LB -b $W/lmark/$1:/dev/$1"; }
  mkm loop-control
  for n in 0 1 2 3; do mkm "loop$n"; for q in 1 2 3 4; do mkm "loop${n}p$q"; done; done
  MPE="$W/lmpe"; MPF="$W/lmpf"; rm -rf "$MPE" "$MPF"; mkdir -p "$MPE" "$MPF"
  FATOFF=$((2048*512))
  cat > "$W/guest_loop.sh" <<EOF
set +e
R=0; fail(){ echo "  GUESTFAIL: \$1"; R=1; }
# ext4 root partition (p2) via loop partition scan (uses /dev/loop0). The helper
# also raw-reads the loop node and verifies the MBR signature (read-routing: what
# blkid/fdisk/dd/bare-mount rely on).
LOUT=\$("$W/loopmnt" "$W/pi.img" "$MPE" 2) || fail "loop mount p2 (ext4)"
echo "\$LOUT" | sed 's/^/  /'
echo "\$LOUT" | grep -q RAWREAD=mbr-ok || fail "loop raw read-routing (MBR sig)"
echo loopdata > "$MPE/lf.txt" || fail "ext4 loop write"
[ "\$(cat "$MPE/lf.txt")" = loopdata ] || fail "ext4 loop readback"
# df/statfs must report the IMAGE size (~tens of MB), not the host rootfs
SZ=\$(df -k "$MPE" 2>/dev/null | awk 'NR==2{print \$2}')
[ -n "\$SZ" ] && [ "\$SZ" -gt 0 ] && [ "\$SZ" -lt 1000000 ] || fail "df statfs size (1K-blocks=\$SZ)"
# FAT boot partition via whole-loop at offset (mount -o loop,offset= style)
"$W/loopmnt" "$W/pi.img" "$MPF" 0 $FATOFF || fail "loop mount @offset (vfat)"
ls "$MPF" | grep -qi cmdline || fail "fat loop content (\$(ls "$MPF"))"
# mkfs ON a loop partition node (the losetup -fP + mkfs /dev/loopNp1 flow): build a
# fresh empty partitioned image, set up the loop, then mkfs the partition node and
# mount it — needs BLKGETSIZE64 on the loop node to report the partition size.
dd if=/dev/zero of="$W/mk.img" bs=1M count=48 status=none
printf 'label: dos\nstart=2048, type=83\n' | sfdisk "$W/mk.img" >/dev/null 2>&1
LN=\$("$W/loopmnt" "$W/mk.img" - 1 | sed -n 's/.*LOOPN=//p')
echo "  mkfs target loop=\$LN"
[ -n "\$LN" ] || fail "loopmnt config-only"
[ -b "/dev/loop\${LN}p1" ] || fail "loop node not seen as block device (stat S_IFBLK)"
mkfs.ext4 -q -F "/dev/loop\${LN}p1" 2>/dev/null || fail "mkfs.ext4 on /dev/loop\${LN}p1"
mkdir -p "$W/lmpm"
mount "/dev/loop\${LN}p1" "$W/lmpm" || fail "mount freshly-mkfs'd loop partition"
echo mkfsloop > "$W/lmpm/t" && [ "\$(cat "$W/lmpm/t")" = mkfsloop ] || fail "rw on mkfs'd loop part"
umount "$W/lmpm"
echo "GUEST_RESULT=\$R"
EOF
  out="$(UK_FS=1 UK_BLOCK=1 "$W/prsrc/proot" -0 $LB /bin/sh "$W/guest_loop.sh" 2>&1)"
  echo "$out" | sed "s/^/    [loop] /"
  if echo "$out" | grep -q "GUEST_RESULT=0"; then
    ok "proot e2e [loop]: loop-mount image partition (ext4) + whole-loop @offset (vfat)"
  else
    bad "proot e2e [loop]: image-file loop mount"
    grep -a 'uk_fs:\|LOOP' "$W/kmsg" 2>/dev/null | sed 's/^/      kmsg: /' | tail -15
  fi
else
  skip "proot e2e [loop]" "no pi.img / losetup / mount"
fi

echo
echo "== $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
