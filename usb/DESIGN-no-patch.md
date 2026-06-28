# Stock libusb under proot — no in-distro patch (design + investigation)

Goal: make **unmodified** distro libusb (and everything on top: `lsusb`, pyusb,
libftdi, rtl-sdr) work under NeoTerm's proot, the same way the camera went from a
stream to a real `/dev/video0` — by intercepting in the **proot layer** instead of
shipping a patched `libusb` (`usb/build-neoterm-libusb.sh`).

Status: **investigation / design only.** This branch (`claude/libusb-proot`) holds
the analysis; nothing here changes runtime behaviour yet. Master is untouched.

## Key constraint discovered: proot cannot inject an external fd

The FUSE work already hit this. `uknl_fs_redirect.c` notes that `open("/dev/fuse")`
is handled **WITHOUT morphing the open syscall** ("which proot doesn't support
cleanly — the sysnum-change workaround"): the guest opens the bound *marker* file
and the redirect then proxies `read/write/ioctl` on that marker fd.

So the patched-libusb trick (receive NeoTerm's real usbfs fd via `SCM_RIGHTS` and
do `USBDEVFS_*` ioctls on it directly) **cannot** be reproduced by handing the
tracee that fd. The shim must instead use the marker fd and **proxy the ioctls** —
exactly like the block proxy (`/dev/uksd0` → `io.neoterm.block`).

## Architecture (tracer-held real fd + USBDEVFS ioctl proxy)

1. **`/dev/bus/usb/BBB/DDD`** are bound empty marker nodes (like `/dev/uksd0`).
2. The tracer (proot), on the first access to a node, connects to the existing
   **`io.neoterm.usb`** socket (`UsbBridge.kt`, already serves `LIST` + a device fd
   via `SCM_RIGHTS`) and receives the **real usbfs fd into the *tracer*** (a normal
   `recvmsg`). It associates that fd with the tracee's marker fd.
3. **`USBDEVFS_*` ioctls** on the marker fd are intercepted (UK_USB seccomp) and
   re-issued by the tracer on the real fd, copying data buffers between tracee and
   tracer memory via `read_data`/`write_data`. Async URBs (`SUBMITURB`/`REAPURB`)
   need URB tracking (tracee buffer addr ↔ tracer buffer); control transfers
   (`USBDEVFS_CONTROL`) are synchronous and simplest (lsusb -v path).
4. **Enumeration:** on device `/sys/bus/usb` is EACCES, so libusb falls back to the
   **usbfs path** (`usbfs_get_device_list`): it scans `/dev/bus/usb/*/*`, opens each
   node and reads the descriptor blob. So we need:
   - getdents injection for `/dev/bus/usb` (bus dirs) and `/dev/bus/usb/BBB`
     (device nodes), from `io.neoterm.usb` `LIST` — like the `/dev` ttyUSB/uksd0
     injection.
   - fake `fstat`/`newfstatat` reporting a **char device, major 189** for the nodes
     (libusb/usbfs expect a char special, like v4l2-ctl wanted major 81) — TBD
     whether required.
   - descriptors readable: either the marker file *contains* the descriptor blob
     (written by ProotManager from `LIST`), or `read()` on the node is proxied to
     the real fd (which returns the descriptors). The proxied-read route unifies
     with the I/O proxy.
5. **netlink:** if stock libusb's hotplug monitor failure is fatal (v1.0.27 was),
   fake `socket(AF_NETLINK, …, NETLINK_KOBJECT_UEVENT)` + `bind` success — scoped
   to *exactly* that family/protocol so `NETLINK_ROUTE` (ip, NetworkManager) is
   untouched. On the host, `libusb_init` did **not** fail without netlink, so this
   may be unnecessary — confirm on device.

## Deconfliction with the existing USB paths — SAFE

Investigated `UsbBridge.kt`, `UsbSerialBridge`, `BlockBridge`, and the proot
patches. No conflict with `/dev/ttyUSB*` or `/dev/uksd0`:

- **Separate sockets:** ttyUSB = `io.neoterm.ttyusb`, block = `io.neoterm.block`,
  fs = `io.neoterm.fs[.pN]`, camera = `io.neoterm.camera`, iio = `io.neoterm.iio`,
  libusb = `io.neoterm.usb`. The proot patches do **not** currently use
  `io.neoterm.usb` at all — the shim would be its first proot consumer.
- **Separate proot paths:** `/dev/bus/usb/*` is a disjoint prefix from
  `/dev/ttyUSB*` and `/dev/uksd0`; getdents inject dir `/dev/bus/usb` ≠ `/dev`.
  Each dispatch is env-gated (UK_BLOCK/UK_FS/UK_CAM/UK_USB) and additive.
- **Ownership already deconflicted in the app:** `UsbBridge.requestPermission`
  routes a known serial chip (serial toggle on) to `UsbSerialBridge` (→ttyUSB), a
  mass-storage device (storage toggle on) to `BlockBridge` (→uksd0), and only
  *everything else* to the raw fd-server — "avoids a double claim".
- A device owned by a bridge would still appear in `LIST`; if a libusb app tries to
  open it, Android allows the second `openDevice` but `claimInterface` returns
  **BUSY** (the bridge holds it) → graceful failure, ttyUSB/uksd0 keep working.
  Refinement: the shim should **filter bridge-owned devices** out of the libusb
  enumeration when those toggles are on, to avoid duplicates/confusion.

## Host testing — via umockdev (no real USB needed)

`umockdev` (libumockdev-preload) mocks a USB device entirely in userspace, so the
host CAN validate this without a real device. Findings (see `usb/test/`):

**Enumeration is VALIDATED.** Straced stock (udev-built) libusb under umockdev: it
enumerates a device purely from **sysfs**, reading exactly these per-device files:
`subsystem` (symlink → .../bus/usb), `uevent`, `busnum`, `devnum`, `speed`, and the
binary `descriptors`. With a proper mock (`usb/test/umock_enum.c`, binary
descriptors via the libumockdev C API) stock libusb lists the device:
`devices=1  bus=1 addr=2 1234:5678`. So a **readable fake `/sys/bus/usb`**
(BlockSysfsBridge pattern, populated from `io.neoterm.usb` LIST + descriptors)
makes unmodified libusb enumerate — no patch, no ioctl needed for enumeration.

Implication for the device: on Android `/sys/bus/usb/devices` is **EACCES** (exists
but unreadable), which is almost certainly why the in-distro patch exists — stock
libusb sees sysfs as "present" but can't read it → 0 devices. The shim's fix is to
**bind a readable fake `/sys/bus/usb`** over it (the camera/block bridges already
overlay `/sys` subtrees the same way). The usbfs-scan / fd-injection route is NOT
needed for enumeration.

**I/O (USBDEVFS ioctls)** can also be host-tested with umockdev's ioctl
record/replay (`umockdev-run -i ioctl=dump`), but needs an ioctl dump (recorded
from a real device, or hand-authored for control transfers). Still device-validated
for real hardware, but the proxy marshalling logic is host-checkable.

→ Plan: enumeration first (host-validated via umockdev + device), then control
transfers (umockdev ioctl replay + device), then bulk/URB (device).

## Phased plan

1. **Enumerate**: `/dev/bus/usb` markers + getdents inject + char-189 stat + read
   descriptors → stock `lsusb` lists devices. (device-tested)
2. **Control transfers**: proxy `USBDEVFS_CONTROL` (+ claim/release, get-driver,
   connectinfo, capabilities) → `lsusb -v`, simple HID/CDC. (device-tested)
3. **Bulk/interrupt + async URBs**: proxy `SUBMITURB`/`REAPURB`/`DISCARDURB` with
   URB+buffer tracking → pyusb, libftdi, rtl-sdr. (device-tested)
4. **Hardening**: filter bridge-owned devices from `LIST`; scoped netlink fake if
   needed; hotplug best-effort.

## Open questions to resolve on device
- Does libusb pick the usbfs path under proot (sysfs EACCES)? (expected yes)
- Is the char-189 stat fake required for usbfs enumeration?
- Is the netlink fake needed, or does `libusb_init` tolerate a dead monitor?
- Does Android's usbfs fd accept `USBDEVFS_SUBMITURB` from a process that didn't
  open it (the tracer received it via SCM_RIGHTS)? (the patched libusb proves the
  *guest* can; the *tracer* doing it on behalf is the new bit to confirm)
