#!/usr/bin/env bash
# Build a NeoTerm-patched libusb INSIDE the proot distro, so that unmodified
# libusb tools (lsusb, pyusb, libftdi, rtl-sdr, …) work under proot on Android.
#
# Two patches to libusb's Linux backend:
#   1) op_init(): a failing hotplug monitor (netlink/udev is SELinux-blocked
#      under Android proot) is no longer fatal — init succeeds, enumeration +
#      I/O still work, only hotplug callbacks are missing.
#   2) _get_usbfs_fd(): when open("/dev/bus/usb/B/D") fails with EACCES (the raw
#      node isn't openable as the app uid), fetch the already-open fd from
#      NeoTerm over the abstract unix socket "io.neoterm.usb" (SCM_RIGHTS).
#
# NeoTerm itself needs no change: its socket already serves a device's fd keyed
# by its name, which equals the path libusb opens (/dev/bus/usb/%03u/%03u).
#
# Run this in the distro (Kali/Ubuntu/Debian; root):
#   bash build-neoterm-libusb.sh
# Then (NeoTerm running + USB permission granted for the device):
#   lsusb -v
#
# Distros differ; for Arch use pacman deps, for Alpine (musl) it also builds but
# install apk equivalents. This script targets apt-based distros.
set -euo pipefail

LIBUSB_TAG="${LIBUSB_TAG:-v1.0.27}"
PREFIX="${PREFIX:-/usr/local}"
WORK="${WORK:-/tmp/neoterm-libusb}"

echo "== deps =="
if command -v apt-get >/dev/null 2>&1; then
  apt-get update
  apt-get install -y --no-install-recommends \
    build-essential autoconf automake libtool pkg-config git ca-certificates \
    libudev-dev python3
fi

echo "== fetch libusb $LIBUSB_TAG =="
rm -rf "$WORK"; mkdir -p "$WORK"
git clone --depth 1 --branch "$LIBUSB_TAG" https://github.com/libusb/libusb "$WORK/libusb"
SRC="$WORK/libusb/libusb/os/linux_usbfs.c"
test -f "$SRC" || { echo "linux_usbfs.c not found"; exit 1; }

echo "== patch linux_usbfs.c =="
python3 - "$SRC" <<'PY'
import re, sys
p = sys.argv[1]
s = open(p).read()
orig = s

# --- helper: fetch a device fd from NeoTerm over the abstract unix socket ---
helper = r'''
/* === NeoTerm: fetch a USB device fd over the abstract unix socket === */
#include <sys/socket.h>
#include <sys/un.h>
#include <stddef.h>
static int neoterm_usb_fd(const char *path)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return -1;
	struct sockaddr_un a;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	static const char nm[] = "io.neoterm.usb";
	a.sun_path[0] = '\0';
	memcpy(a.sun_path + 1, nm, sizeof(nm) - 1);
	socklen_t L = offsetof(struct sockaddr_un, sun_path) + 1 + (sizeof(nm) - 1);
	if (connect(s, (struct sockaddr *)&a, L) < 0) { close(s); return -1; }
	dprintf(s, "%s\n", path);
	struct msghdr m;
	memset(&m, 0, sizeof m);
	char buf[256];
	struct iovec io = { buf, sizeof buf - 1 };
	char cbuf[CMSG_SPACE(sizeof(int))];
	memset(cbuf, 0, sizeof cbuf);
	m.msg_iov = &io; m.msg_iovlen = 1;
	m.msg_control = cbuf; m.msg_controllen = sizeof cbuf;
	ssize_t n = recvmsg(s, &m, 0);
	int fd = -1;
	if (n > 0) {
		struct cmsghdr *c = CMSG_FIRSTHDR(&m);
		if (c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
			memcpy(&fd, CMSG_DATA(c), sizeof fd);
	}
	close(s);
	return fd;
}
/* === end NeoTerm === */
'''

# Insert the helper right after the linux_usbfs.h include.
m = re.search(r'#include\s+"linux_usbfs\.h"\s*\n', s)
if not m:
    print("anchor for helper insertion not found", file=sys.stderr); sys.exit(2)
s = s[:m.end()] + helper + s[m.end():]

# --- 1) make the hotplug monitor failure non-fatal in op_init ---
s2 = re.sub(
    r'usbi_err\(ctx,\s*"error starting hotplug event monitor"\);\s*\n\s*return r;',
    'usbi_warn(ctx, "NeoTerm: hotplug monitor unavailable, continuing without it");',
    s)
if s2 == s:
    print("anchor for hotplug patch not found", file=sys.stderr); sys.exit(3)
s = s2

# --- 2) EACCES fallback to the NeoTerm fd in _get_usbfs_fd ---
s2 = re.sub(
    r'(fd = open\(path, mode[^;]*\);\s*\n\s*if \(fd != -1\)\s*\n\s*return fd;[^\n]*\n)',
    r'\1\tif (errno == EACCES) {\n'
    r'\t\tint nfd = neoterm_usb_fd(path);\n'
    r'\t\tif (nfd != -1)\n'
    r'\t\t\treturn nfd;\n'
    r'\t}\n',
    s, count=1)
if s2 == s:
    print("anchor for _get_usbfs_fd patch not found", file=sys.stderr); sys.exit(4)
s = s2

open(p, 'w').write(s)
print("patched OK (%d -> %d bytes)" % (len(orig), len(s)))
PY

echo "== build =="
cd "$WORK/libusb"
./bootstrap.sh
./configure --prefix="$PREFIX" --disable-static
make -j"$(nproc)"
make install
ldconfig 2>/dev/null || true

echo
echo "== done =="
echo "Installed patched libusb to $PREFIX/lib (libusb-1.0.so.0)."
echo "If $PREFIX/lib isn't picked up automatically, prefer it explicitly:"
echo "  export LD_LIBRARY_PATH=$PREFIX/lib:\$LD_LIBRARY_PATH"
echo "Then (NeoTerm running + USB permission granted):  lsusb -v"
