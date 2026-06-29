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

## BLOCKER found while building Phase 1 (enumeration)

umockdev (complete in-process fake `/sys`) → stock libusb enumerates. But the shim
delivers enumeration via a **proot bind** that fakes only `/sys/bus/usb` (+ a
`/sys/devices/...` subtree), leaving the rest of `/sys` as the host's real tree.
That partial bind does **NOT** enumerate — `libusb_get_device_list` returns 0 even
though, inside proot, every file is correct and readable:
`/sys/bus/usb/devices/1-1 -> ../../../devices/neoterm-usb/1-1`, `idVendor`=1234,
`subsystem -> .../bus/usb`, `descriptors`=36 bytes. So `libudev` needs more global
`/sys` consistency than a partial overlay provides, and the exact missing bit can't
be found because **proot blocks nested `ptrace`** → no `strace` *inside* proot (on
host or device), so this can't be debugged the usual way.

Two candidate ways forward (both need real device iteration, which is expensive
without in-proot strace):

1. **Comprehensive fake `/sys`** — overlay enough of `/sys` (bus/usb, class/, maybe
   devices/) consistently for libudev. Risk: broad `/sys` overlay may disturb other
   guest tooling; exact requirement unknown without strace.
2. **Force the usbfs path** (bypass libudev): make libusb detect sysfs as
   unavailable so it scans `/dev/bus/usb` and reads descriptors from the nodes.
   Needs: libusb's sysfs-detection nuance (couldn't confirm without source/strace),
   `/dev/bus/usb` markers, char-189 stat, and descriptor bytes per node (marker file
   content or proxied read). Then the USBDEVFS ioctl proxy on top.

## BREAKTHROUGH: device logs pinpoint the real blocker (netlink, not enumeration)

`LIBUSB_DEBUG=5 lsusb` on the device (libusb 1.0.30) shows init dying *before*
enumeration:

    [op_init] could not find usbfs, defaulting to /dev/bus/usb
    [op_init] sysfs is available
    [linux_udev_start_event_monitor] could not initialize udev monitor
    [op_init] error starting hotplug event monitor
    unable to initialize libusb: -99

So on device: **sysfs IS available** (enumeration path is fine), and `libusb_init`
fails at the **udev/netlink hotplug monitor** — a `NETLINK_KOBJECT_UEVENT` socket
whose `bind()` to a multicast group is SELinux-blocked for app uids. libusb treats
that as fatal (-99). (This is exactly what the in-distro patch's "hotplug
non-fatal" hunk worked around.)

**Fix (implemented):** `proot/patches/uknl_usb_redirect.c` (UK_USB) fakes success
for that one `bind()` — an `AF_NETLINK` socket binding to `nl_groups != 0`. The
monitor then "starts" (no events ever arrive — fine, no uevents under proot) and
`libusb_init()` succeeds. Host-verified scoping: a normal `AF_INET` bind still gets
a real port; an `AF_NETLINK` group bind that the kernel would reject returns 0 only
under `UK_USB`. Wired in `fakeid0-xattr.py` (dispatch after the camera shim + a
`UK_USB` seccomp filter trapping `bind`); `ProotManager` exports `UK_USB=1`.

Refinement (device round 2): faking only `bind()` did NOT fix it — the failure is
the **socket() creation**, not the bind. `udev_monitor_new_from_netlink` returns
NULL because `socket(AF_NETLINK, …, NETLINK_KOBJECT_UEVENT)` is itself SELinux-
blocked for app uids. So the shim now (UK_USB):
1. **socket()**: rewrites protocol `NETLINK_KOBJECT_UEVENT`→`NETLINK_ROUTE` (app
   uids may create that) so the call succeeds and libusb gets a real, pollable fd;
2. **bind()** (AF_NETLINK, groups≠0): faked success;
3. **setsockopt(SOL_NETLINK, NETLINK_ADD_MEMBERSHIP)**: faked success.
With bind+membership neutralised the socket is never actually subscribed, so no
events flow (no hotplug) but `udev_monitor_new_from_netlink` /
`udev_monitor_enable_receiving` both succeed → `libusb_init()` succeeds.
Host-verified: under UK_USB the uevent socket's `SO_PROTOCOL` becomes 0
(NETLINK_ROUTE); AF_INET sockets are untouched.

→ Next, on device: confirm `lsusb` now initialises and whether it enumerates from
the (available) sysfs. If enumeration is empty (sysfs readdir EACCES), add the
fake `/sys/bus/usb`. Then `/dev/bus/usb` + the USBDEVFS ioctl proxy for I/O.

## Device round 3: init WORKS, enumeration returns 0 (libudev wall)

The netlink fakes worked — on device `libusb_init` now SUCCEEDS (no more -99):
the udev event thread starts, `installing new context`, `libusb_get_device_list`
runs. But `lsusb` is empty: `get_device_list` returns 0, because the app-uid can't
read the real `/sys/bus/usb` device entries.

Tried to make libusb enumerate from a **faked `/sys/bus/usb`** (the validated-with-
umockdev idea). Reproduced the device's "0 devices" on host (both under proot and a
plain bind-mount of a complete fake `/sys`) and straced it (bind-mount, no proot):
libudev's `udev_enumerate_scan_devices` **finds `1-1`**, readlinks it, canonicalises
to `/sys/devices/1-1` (manual `..`-walk + `/proc/self/fd`), then **discards it** and
moves on to `/sys/class` — it never reads the device's `uevent`/`descriptors`.
umockdev's *preload* gets past this exact step (it intercepts the path ops); a real
faked `/sys` (bind-mount OR proot) does not. So libudev applies a path/validity
check that a faked sysfs doesn't satisfy and that can't be cracked without diving
deep into libudev internals.

### Where this leaves the no-patch effort
- **Solved:** `libusb_init` (the -99 that the in-distro patch's hotplug hunk
  worked around) — via the UK_USB netlink socket/bind/setsockopt fakes. This part
  is real, device-confirmed, and on the branch.
- **Open:** enumeration. Two routes remain, both non-trivial and device-only:
  1. Satisfy libudev's sysfs enumeration (the canonicalisation/validity check it
     applies to a faked `/sys`) — opaque, no strace under proot.
  2. **Force the usbfs path** (bypass libudev): make libusb detect sysfs as
     *unavailable* so it runs `usbfs_get_device_list` (scan `/dev/bus/usb`, read
     descriptors from the nodes). Avoids libudev entirely. Needs the sysfs-
     unavailable nudge + `/dev/bus/usb` nodes serving descriptors + the USBDEVFS
     ioctl proxy for I/O.

### Recommendation
The in-distro **patched libusb (`build-neoterm-libusb.sh`) remains the supported,
fully-working path** (init + enumeration + I/O). The no-patch shim has unblocked
init; completing enumeration+I/O is a real research effort. If pursued, route 2
(force usbfs, bypass libudev) is the cleaner bet. Branch keeps all of this; master
is untouched.

## Device round 4: route 2 is a dead end for udev-built libusb; libudev wall confirmed

Two findings close this off for the stock distro libusb:

1. **No usbfs fallback in the udev build.** The usbfs enumeration path
   (`usbfs_get_device_list`) only exists when libusb is built `--disable-udev`
   (which is exactly what `build-neoterm-libusb.sh` does). The distro libusb is a
   **udev build** — enumeration goes through libudev unconditionally; there is no
   usbfs path to force. (Confirmed on host: libusb reports "sysfs is available"
   even with no `/sys/bus/usb` at all.) So **route 2 is not applicable** to the
   unmodified distro library.

2. **Exactly why a faked `/sys` fails libudev.** Side-by-side strace, faked `/sys`
   (mount-namespace, no proot) vs the working umockdev run:
   - umockdev: libudev reads `/sys/devices/1-1/{uevent,subsystem,descriptors}` and
     enumerates the device (even though `/run/udev/data/+usb:1-1` is ENOENT — the
     "initialized" DB entry is NOT required).
   - real faked `/sys`: libudev finds `1-1`, readlinks it, does a manual
     `..`-walk + `/proc/self/fd` canonicalisation to `/sys/devices/1-1`, then
     **discards it before ever reading `uevent`**, and moves to `/sys/class`.
   umockdev only gets past that canonicalisation because its **LD_PRELOAD**
   intercepts the path/readlink ops *in-process*; proot's syscall-level redirect
   can't reproduce that in-process resolution. So a proot-bound fake `/sys` can't
   satisfy libudev's enumeration.

### Verdict
The no-patch-via-proot approach **cannot complete enumeration** for the stock
udev-built distro libusb: init is fixable in proot (done), but enumeration is
gated by libudev path-canonicalisation that only an in-process preload can pass.

Options that remain (all leave master/patch as-is):
- **Keep `build-neoterm-libusb.sh`** (the patched, fully-working libusb) as the
  supported path. ← recommended.
- A **guest LD_PRELOAD shim** (umockdev-style, minimal) that feeds libudev
  enumeration from `io.neoterm.usb`. Works with any libusb (no libusb patch) but
  adds a preload to the guest env — a different trade-off, not a proot shim.

The UK_USB netlink init-fix on this branch is still worth keeping if a future
preload/no-patch path is pursued (it removes the -99 independently).

## Device round 5: pure-proot real-udev — blocked by systemd sd-device validation

Goal refined by the user: run the **real, unmodified udev/libudev** under proot
(no LD_PRELOAD, no guest build), everything provided by NeoTerm + proot. proot
*can* intercept the syscalls, and we control proot, so in principle this should be
doable.

Confirmed proot handles the obvious things: `/proc/self/fd/N` for a bound path
reverse-translates to the **guest** path (`/sys/bus/usb/devices/1-1`), not the host
bind path. So canonicalisation input is correct.

But the real (systemd 255) libudev still returns **0 devices** from a faked `/sys`,
whether provided by a proot bind or a plain mount-namespace bind. Straced it
side-by-side against the working umockdev run and ruled out, one by one:
- descriptors binary encoding (fixed; libusb reads them fine);
- complete vs partial fake `/sys` (complete bind-mount also fails);
- `/proc/self/fd` translation (proot returns the guest path correctly);
- `/run/udev/data/...` initialised-DB entry (umockdev works **without** it);
- `statfs()` magic — hypothesised libudev requires `SYSFS_MAGIC`, but umockdev's
  own `/sys` reports `EXT2_SUPER_MAGIC` and still works; faking `SYSFS_MAGIC` on a
  bind-mount `/sys` did **not** help. Red herring.

In the failing trace, libudev finds `1-1`, resolves it to `/sys/devices/1-1`
(manual `..`-walk + `/proc/self/fd`), then **discards it before reading `uevent`**.
umockdev gets past this only because its preload replaces ~40 libc functions
(open/stat/lstat/readlink/scandir/opendir/statfs/…) so sd-device's in-process path
validation sees a fully self-consistent fake. proot's **syscall-level** redirect
provides correct results but does not reproduce that in-process consistency, and
the exact sd-device check that rejects the device is buried in systemd internals
(not crackable without sd-device source-level debugging; `ltrace` unavailable;
no nested `strace` under proot).

### Honest conclusion
- **Solved & on branch:** `libusb_init` (the -99) via the UK_USB netlink
  socket/bind/setsockopt fakes — pure proot, no guest changes.
- **Not achievable as pure-proot right now:** making the **unmodified systemd
  libudev enumerate** from a proot-provided `/sys`. It is gated by sd-device
  in-process validation that only a comprehensive libc preload (umockdev-style)
  satisfies — i.e. exactly the LD_PRELOAD approach ruled out. Cracking it is a
  systemd-internals research effort with uncertain payoff.

### Where that leaves the options
1. **Keep `build-neoterm-libusb.sh`** (works fully) — needs an in-guest build.
2. **Deep systemd sd-device study** to find and satisfy the exact validation via
   proot path-op handling — uncertain, possibly large.
3. **Accept** that unmodified-libudev enumeration needs in-process interception
   (preload) which is out of scope by the no-preload/no-patch constraint.

## Device round 6: BREAKTHROUGH — `SYSTEMD_DEVICE_VERIFY_SYSFS=0`

Read systemd 255.4 `sd-device.c` `device_set_syspath(verify=true)`. The exact check
that discarded our faked device is:

    r = getenv_bool_secure("SYSTEMD_DEVICE_VERIFY_SYSFS");
    if (r != 0) {                                  // unset → runs
        if (fd_is_fs_type(fd, SYSFS_MAGIC) == 0)   // is the dir really on sysfs?
            return -ENODEV;                        // "outside of sysfs, refusing"
    }

systemd's libudev verifies each device dir is on a real sysfs filesystem — and a
proot/bind fake `/sys` is ext2/etc., so every device is refused. **But the check is
gated by an env var.** Setting **`SYSTEMD_DEVICE_VERIFY_SYSFS=0`** makes the
*unmodified* systemd libudev skip it.

**Validated (host, real systemd libusb 1.0.30 + libudev 255, NO preload, NO
patch):** a complete fake `/sys` bound over `/sys` **under proot** plus
`SYSTEMD_DEVICE_VERIFY_SYSFS=0` →

    libusb_init = 0 (ok)
    get_device_list = 1 devices
      bus=1 addr=2  1234:5678

So the no-preload / no-guest-patch goal IS achievable: proot provides a fake
`/sys/bus/usb` (from `io.neoterm.usb`) + the env var, and the stock distro libusb
enumerates. This is the user's vision realised.

(The host's *partial* subtree binds didn't enumerate, but the host container's
`/sys` is minimal/inconsistent; on a real device the existing sensor/block bridges
already use partial `/sys` subtree binds successfully, so the USB subtree bind +
env var is expected to work on-device. Delivery to validate on device.)

### Remaining work (no patch, no preload)
1. `ProotManager`: export `SYSTEMD_DEVICE_VERIFY_SYSFS=0` (done) + `UK_USB` for the
   netlink init-fix.
2. A `UsbSysfsBridge` (Kotlin, like `BlockSysfsBridge`): build a readable fake
   `/sys/bus/usb` (+ device dirs: uevent, busnum, devnum, speed, descriptors,
   idVendor/idProduct, subsystem→usb) from `io.neoterm.usb` LIST + raw descriptors;
   bind it. → stock `lsusb` lists devices, unmodified.
3. I/O (`lsusb -v`, pyusb): `/dev/bus/usb` nodes + the USBDEVFS proxy or the
   io.neoterm.usb fd (Phase 2), on device.

## Assessment (superseded by the breakthrough above — kept for history)

The no-patch path is a real **research effort**, materially harder than the camera:
- libusb enumeration (libudev) is environment-finicky; a partial proot `/sys` bind
  doesn't satisfy it, and `strace` is unavailable inside proot to diagnose.
- The `USBDEVFS` I/O proxy is a large surface and can only be fully validated on
  real hardware.
The existing in-distro **patched libusb (`build-neoterm-libusb.sh`) works today** and
is contained. Recommendation: keep the patch as the supported path; pursue the
no-patch shim only as an opt-in research track (device-side), starting from the
usbfs-path route (option 2) which avoids the libudev wall entirely.

Deconfliction with ttyUSB/uksd0 remains SAFE either way (separate sockets/paths;
app already deconflicts ownership).

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
