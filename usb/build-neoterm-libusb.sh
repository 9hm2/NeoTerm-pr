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
    python3
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
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/socket.h>
#include <sys/un.h>
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
	char line[64];
	int ln = snprintf(line, sizeof line, "%s\n", path);
	if (ln <= 0 || write(s, line, (size_t)ln) != ln) { close(s); return -1; }
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

/* Enumerate from NeoTerm's device LIST instead of sysfs/usbfs (both are EACCES
 * for the app uid under Android proot). Each line is "/dev/bus/usb/BBB/DDD ...".
 * Descriptors are then read from the device fd (get_usbfs_fd -> NeoTerm fd). */
static int neoterm_scan_devices(struct libusb_context *ctx)
{
	int s = socket(AF_UNIX, SOCK_STREAM, 0);
	if (s < 0)
		return LIBUSB_SUCCESS;
	struct sockaddr_un a;
	memset(&a, 0, sizeof a);
	a.sun_family = AF_UNIX;
	static const char nm[] = "io.neoterm.usb";
	a.sun_path[0] = '\0';
	memcpy(a.sun_path + 1, nm, sizeof(nm) - 1);
	socklen_t L = offsetof(struct sockaddr_un, sun_path) + 1 + (sizeof(nm) - 1);
	if (connect(s, (struct sockaddr *)&a, L) < 0) { close(s); return LIBUSB_SUCCESS; }
	if (write(s, "LIST\n", 5) != 5) { close(s); return LIBUSB_SUCCESS; }
	char buf[8192];
	size_t off = 0;
	ssize_t n;
	while (off < sizeof(buf) - 1 && (n = read(s, buf + off, sizeof(buf) - 1 - off)) > 0)
		off += (size_t)n;
	close(s);
	buf[off] = '\0';
	char *save = NULL;
	for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		unsigned bus, dev;
		if (sscanf(line, "/dev/bus/usb/%u/%u", &bus, &dev) == 2)
			(void)linux_enumerate_device(ctx, (uint8_t)bus, (uint8_t)dev, NULL);
	}
	return LIBUSB_SUCCESS;
}
/* === end NeoTerm === */
'''

# Insert the helper right after the linux_usbfs.h include.
m = re.search(r'#include\s+"linux_usbfs\.h"\s*\n', s)
if not m:
    print("anchor for helper insertion not found", file=sys.stderr); sys.exit(2)
s = s[:m.end()] + helper + s[m.end():]

# --- 0) usbfs dir not opendir-able as the app uid → don't make it fatal ---
# (v1.0.27 returns -99 here; 1.0.30 already defaults to /dev/bus/usb and goes on)
s2 = re.sub(
    r'usbfs_path = find_usbfs_path\(\);\n'
    r'\tif \(!usbfs_path\) \{\n'
    r'\t\tusbi_err\(ctx, "could not find usbfs"\);\n'
    r'\t\treturn LIBUSB_ERROR_OTHER;\n'
    r'\t\}',
    'usbfs_path = find_usbfs_path();\n'
    '\tif (!usbfs_path) {\n'
    '\t\tusbi_warn(ctx, "NeoTerm: usbfs not accessible, defaulting to /dev/bus/usb");\n'
    '\t\tusbfs_path = "/dev/bus/usb";\n'
    '\t}',
    s, count=1)
if s2 == s:
    print("anchor for find_usbfs patch not found", file=sys.stderr); sys.exit(6)
s = s2

# --- 1) make the hotplug monitor failure non-fatal in op_init ---
s2 = re.sub(
    r'r = linux_start_event_monitor\(\);\n\t\}',
    'r = linux_start_event_monitor();\n'
    '\t\tif (r != LIBUSB_SUCCESS) {\n'
    '\t\t\tusbi_warn(ctx, "NeoTerm: hotplug monitor unavailable, continuing");\n'
    '\t\t\tr = LIBUSB_SUCCESS;\n'
    '\t\t}\n'
    '\t}',
    s, count=1)
if s2 == s:
    print("anchor for hotplug patch not found", file=sys.stderr); sys.exit(3)
s = s2

# --- 2) EACCES fallback to the NeoTerm fd in get_usbfs_fd ---
s2 = re.sub(
    r'(\tif \(!silent\) \{\n\t\tusbi_err\(ctx, "libusb couldn)',
    '\tif (errno == EACCES) {\n'
    '\t\tint nfd = neoterm_usb_fd(path);\n'
    '\t\tif (nfd != -1)\n'
    '\t\t\treturn nfd;\n'
    '\t}\n\n'
    r'\1',
    s, count=1)
if s2 == s:
    print("anchor for get_usbfs_fd patch not found", file=sys.stderr); sys.exit(4)
s = s2

# --- 3) enumerate via the NeoTerm socket (sysfs + /dev/bus/usb dir are EACCES) ---
s2 = re.sub(
    r'\tif \(sysfs_available\)\n'
    r'\t\treturn sysfs_get_device_list\(ctx\);\n'
    r'\telse\n'
    r'\t\treturn usbfs_get_device_list\(ctx\);',
    '\t(void)sysfs_get_device_list; (void)usbfs_get_device_list;\n'
    '\treturn neoterm_scan_devices(ctx);',
    s, count=1)
if s2 == s:
    print("anchor for scan_devices patch not found", file=sys.stderr); sys.exit(7)
s = s2

open(p, 'w').write(s)
print("patched linux_usbfs.c OK (%d -> %d bytes)" % (len(orig), len(s)))
PY

echo "== patch linux_netlink.c (stop guard) =="
NL="$WORK/libusb/libusb/os/linux_netlink.c"
test -f "$NL" || { echo "linux_netlink.c not found"; exit 1; }
python3 - "$NL" <<'PY'
import sys
p = sys.argv[1]
s = open(p).read()
# Our op_init patch keeps init going even if the netlink monitor failed to
# start (socket bind is SELinux-blocked under proot), so stop must tolerate a
# monitor that never started instead of asserting.
needle = '\tassert(linux_netlink_socket != -1);'
if needle not in s:
    print("anchor for netlink stop patch not found", file=sys.stderr); sys.exit(5)
s = s.replace(needle,
    '\tif (linux_netlink_socket == -1)  /* NeoTerm: monitor never started */\n'
    '\t\treturn LIBUSB_SUCCESS;', 1)
open(p, 'w').write(s)
print("patched linux_netlink.c OK")
PY

echo "== build =="
cd "$WORK/libusb"
./bootstrap.sh
# --disable-udev: enumerate from sysfs (readable under proot); hotplug uses
# netlink (which we patch to fail-soft). Avoids the udev context complications.
./configure --prefix="$PREFIX" --disable-udev --disable-static
make -j"$(nproc)"
make install
ldconfig 2>/dev/null || true

echo
echo "== done =="
echo "Installed patched libusb to $PREFIX/lib (libusb-1.0.so.0)."
echo "If $PREFIX/lib isn't picked up automatically, prefer it explicitly:"
echo "  export LD_LIBRARY_PATH=$PREFIX/lib:\$LD_LIBRARY_PATH"
echo "Then (NeoTerm running + USB permission granted):  lsusb -v"
