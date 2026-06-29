# USB Wi‑Fi framework — status

Active plan: `DESIGN.md` (chip‑agnostic uKernel **shim + `.so`**; `.ko`/LKL
rejected, see `DESIGN-ko-framework.md`).

## W0 — vendoring & NDK build of the driver‑independent framework ✅ (compiles)

Vendored under `app/src/main/cpp/ukwifi/` (the WiFi‑specific `.c` only — the
headers + core kernel‑API shim are reused from the FS engine in
`app/src/main/cpp/ukfs/`):

```
ukwifi/shim/net/{netdev,skbuff,ieee80211}.c   kernel-side net shim
ukwifi/stack/cfg80211/cfg80211_core.c         custom cfg80211 (wiphy/scan/bss/connect/key)
ukwifi/stack/mac80211/mac80211_core.c         mac80211 stub (full-MAC needs none)
ukwifi/hcd/usbfs_hcd.c                         URB <-> USBDEVFS over io.neoterm.usb (SCM_RIGHTS)
ukwifi/server/userver.c                        loader + UK_OP_* proxy (io.neoterm.wifi)
ukwifi/ukwifi.cmake                            NDK build (mirrors ukfs.cmake)
```

No Wi‑Fi driver is bundled — `userver` `dlopen`s a chip's vendor driver `.so` at
runtime, so the framework is chip‑agnostic.

**Result:** all 7 framework sources compile with the NDK (r26b, aarch64,
android‑26, bionic), **0 errors**. The one bionic portability fix vs the host
build: `shim/net/netdev.c` used `struct sockaddr` (not provided by bionic's
`<linux/socket.h>`); replaced with a layout‑compatible local struct passed to
`ndo_set_mac_address(void*)`.

Reproduce (standalone, no gradle):

```sh
NDK=/opt/android-ndk-r26b
CC=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android26-clang
INC=app/src/main/cpp/ukfs/include
WARN="-Wno-implicit-function-declaration -Wno-incompatible-pointer-types \
 -Wno-incompatible-function-pointer-types -Wno-unused -Wno-unused-parameter \
 -Wno-sign-compare -Wno-implicit-fallthrough -Wno-missing-braces \
 -Wno-unknown-pragmas -Wno-macro-redefined -Wno-visibility"
# kernel-side (fake headers first):
$CC -c -fPIC -O2 -D_GNU_SOURCE -pthread -I$INC $WARN \
   app/src/main/cpp/ukwifi/shim/net/*.c \
   app/src/main/cpp/ukwifi/stack/*/*.c
# userspace (real headers win, fake tree -idirafter):
$CC -c -fPIC -O2 -D_GNU_SOURCE -pthread -idirafter $INC $WARN \
   app/src/main/cpp/ukwifi/hcd/usbfs_hcd.c app/src/main/cpp/ukwifi/server/userver.c
```

## W0b — runnable daemon `libukwifid.so` ✅ (links)

- Added `shim/usb/usb_core.c` (USB core: `usb_register`/enumerate/probe/URB) and
  `hcd/mock_hcd.c` (device‑less backend) to the vendored set.
- Adapted `server/userver.c` for **single‑binary** use: with no `--shim`, it
  resolves the kernel‑API symbols from its own image via `dlopen(NULL)` (the shim
  + cfg80211 are statically linked in); it still `dlopen`s only the chip's driver
  `.so` at runtime. The image is linked with `-Wl,--export-dynamic` so those
  symbols are visible to `dlsym` and to the dlopen'd driver.
- `ukwifi.cmake` now builds the `ukwifid` executable → **`libukwifid.so`** (PIE,
  AGP‑packaged like `ukfsd`); wired into `app/CMakeLists.txt` behind
  `option(UKWIFI_BUILD ON)`.

**Result (NDK r26b, aarch64, android‑26, bionic):** `libukwifid.so` configures,
compiles and **links with 0 errors / 0 undefined symbols** (496 KB ELF64 DYN/PIE;
NEEDED only libc/libm/libdl). It exports the 266 dynamic symbols a driver needs
(`usb_register_driver`, `wiphy_new`, `cfg80211_inform_bss`, `ieee80211_alloc_hw`,
`ukernel_*`, …). Combined `ukfs` + `ukwifi` configure is conflict‑free. Still no
driver bundled — chip‑agnostic.

Reproduce the daemon link:

```sh
NDK=/opt/android-ndk-r26b
cat > /tmp/ukw/CMakeLists.txt <<X
cmake_minimum_required(VERSION 3.10)
project(ukw C)
include(/abs/path/app/src/main/cpp/ukwifi/ukwifi.cmake)
X
cmake -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
      -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -S /tmp/ukw -B /tmp/ukw/out
cmake --build /tmp/ukw/out --target ukwifid     # -> libukwifid.so
```

## W0c — module manager (modprobe / lsmod / rmmod backend) ✅ (links)

The guest must operate the driver with the **standard kernel‑module commands**
(the `.ko` path stays rejected — the driver is still a pre‑built `.so` under the
hood). Added the daemon backend:

- `server/modmgr.c` (+ `modmgr.h`): a name→`.so` registry. `ukw_modprobe(name)`
  resolves `<moddir>/<name>.so` (or `lib<name>.so`; moddir = `$UK_WIFI_MODDIR` or
  the daemon's own dir), `dlopen`s it, runs `module_init`, probes the chip.
  `ukw_rmmod(name)` = `module_exit` + `dlclose`. `ukw_lsmod()` emits real
  `/proc/modules` lines.
- `proxy.h`: new ops `UK_OP_MODPROBE` (30), `UK_OP_RMMOD` (31), `UK_OP_LSMOD` (32)
  — additive, ukfs does not use `proxy.h`.
- `userver.c`: inits the manager, registers argv‑loaded modules for `lsmod`, and
  serves the three ops on `io.neoterm.wifi`.

`libukwifid.so` rebuilds with the manager (links, 0 errors; exports `ukw_modprobe`
/`ukw_rmmod`/`ukw_lsmod`/`ukw_modmgr_init`). The **guest‑side glue** (the
`UK_WIFI` redirect of `init_module`/`finit_module`/`delete_module` → these ops,
and `/proc/modules` read → `UK_OP_LSMOD`) is part of W3.

## W1 — Android bridge + Settings toggle + UK_WIFI gate ✅

- `UsbWifiBridge.kt` (`io.neoterm.utils`): launches/supervises `libukwifid.so` in
  **serve mode with no driver** on the abstract socket `@io.neoterm.wifi`
  (`--serve --sock @io.neoterm.wifi --hcd usbfs`, `UK_WIFI_MODDIR` = app lib dir);
  `killStale()` like `FsBridge`; gated by the toggle. Torn down in
  `NeoTermService` alongside the other bridges.
- Settings: **"USB Wi-Fi (modprobe)"** CheckBox (`key_general_usb_wifi` +
  strings + `setting_general.xml`), `DefaultValues.enableUsbWifi=false`,
  `NeoPreference.isUsbWifiEnabled()`.
- `ProotManager`: `UsbWifiBridge.ensureReady()` on launch; `UK_WIFI=1` env added
  when the toggle is on (gates the future W3 redirect).
- Daemon: `userver` now starts **chip-agnostic** (no argv module required in
  `--serve`; skips the one-shot probe), and `proxy_serve` binds **abstract**
  sockets (`@name`). Rebuilds/links clean (NDK aarch64/bionic).

So toggling **USB Wi-Fi** on starts the framework daemon; the guest-facing
behaviour arrives with W3.

## W2 — /sys/class/net + /sys/class/ieee80211 bridge ✅

- `UsbWifiSysfsBridge.kt`: binds `sys-class-net → /sys/class/net` and
  `sys-class-ieee80211 → /sys/class/ieee80211` (gated by the toggle, since the
  overlay hides the app‑inaccessible real netdevs), seeded with a static `lo`.
- The daemon **populates** them: `server/wsysfs.c` (`ukw_wsysfs_refresh`) writes
  per‑interface (`address`/`ifindex`/`type`/`flags`/`mtu`/`operstate`/`carrier`/
  `uevent` + `phy80211` symlink) and per‑phy (`name`/`index`/`macaddress`) files
  from the live netdev/wiphy accessors, into `$UK_WIFI_SYSFS_NET` /
  `$UK_WIFI_SYSFS_PHY`. Called after a driver probes (in `modmgr` and the argv
  path). So `wlanN`/`phyN` appear when a chip comes up; the skeleton + `lo` exist
  from the start, listable from the guest.
- `UsbWifiBridge` passes the two sysfs paths as env; `ProotManager` binds the
  trees (toggle‑gated). Daemon rebuilds/links clean (NDK aarch64/bionic;
  `ukw_wsysfs_refresh` exported).

`ip link` / `if_nametoindex` / `readdir(/sys/class/net)` will see the interface
once a driver is up; full Wi‑Fi control (`iw`/`wpa_supplicant`) is nl80211 → W3.

## W3a — proot module-syscall redirect (modprobe / rmmod / lsmod) ✅ (host-validated)

- `proot/patches/uknl_wifi_redirect.c` (gated by `UK_WIFI`, injected after the USB
  redirect): `finit_module(fd)` → `UK_OP_MODPROBE <name>` (name from the fd path:
  basename minus `memfd:`/`.ko*`), `init_module(buf)` → name from the `.modinfo`
  `name=` string, `delete_module(name)` → `UK_OP_RMMOD <name>`, all over the
  abstract socket `@io.neoterm.wifi`; the syscall result is faked from the
  daemon's reply.
- Seccomp gate (`fakeid0-xattr.py`): traps `finit_module`/`init_module`/
  `delete_module` only when `UK_WIFI` is set.
- `lsmod`: the daemon mirrors its module list into `$UK_WIFI_PROCMOD`
  (`modmgr.c::write_procmod`), bound over `/proc/modules` by `UsbWifiSysfsBridge`
  — so `lsmod` reflects loaded drivers with no syscall redirect.

Host-validated under the rebuilt proot: a guest calling `finit_module(rtl8812au.ko)`
+ `delete_module("rtl8812au")` made the (stub) daemon receive `OP=30
name=rtl8812au` then `OP=31 name=rtl8812au`, and both syscalls returned 0. So the
guest's `modprobe`/`rmmod` reach the daemon by name; `lsmod` reads the bound file.

## W3b-1 — daemon netlink engine (FULL nl80211, reused) ✅ (host-validated)

The proven uKernel **nl80211 command set is reused as-is** (not rewritten):
`bridge/{netlink_msg,nl_dispatch,genl_ctrl,nl80211_cmds}.c` are vendored under
`ukwifi/nl/`, and the `userver_client` they call is swapped for an **in-process
adapter** (`userver_client_inproc.c`) that invokes the daemon's own
`ukernel_*` functions directly (no socket round-trip). The LD_PRELOAD-only
globals (`uknl_send`/`uknl_mcast_send`/`uknl_scan_gen`/sysfs hooks) get sync-model
glue in `nlglue.c` (early-flush is a no-op since a handler builds its whole reply;
async mcast events are W3b-2). The daemon registers genl CTRL + nl80211 and serves
**`UK_OP_NL`** (raw netlink request in → raw reply out via `ukw_nl_dispatch`).

Host-validated (gcc, the engine is portable C): `CTRL_CMD_GETFAMILY("nl80211")`
returns family id `0x24`; `NL80211_CMD_GET_INTERFACE` returns `wlan0`. Daemon
links clean (NDK aarch64/bionic; `ukw_nl_dispatch`/`nl80211_register` exported).

## W3b-2 — proot genl netlink ferry (synchronous) ✅ (host-validated)

`uknl_wifi_redirect.c` now ferries the guest's nl80211/genl netlink socket to the
daemon:
- **mark** at `socket()` EXIT (`uknl_wifi_mark_socket`, called from
  `translate_syscall_exit`): only `AF_NETLINK` + `NETLINK_GENERIC` fds are tracked
  (rtnetlink/uevent left alone), keyed by (tgid, fd).
- **sendmsg** on a marked fd: gather the iov into the raw netlink request, send it
  as `UK_OP_NL` to `@io.neoterm.wifi`, stash the reply; report all bytes sent.
- **recvmsg**: drain the stashed reply into the iov (offset-tracked across calls;
  `-EAGAIN` once empty); zero msg_namelen/controllen.
- **close**: free the entry.
- Seccomp (`UK_WIFI`): traps `socket`/`sendmsg`/`recvmsg`/`close` (+ the W3a module
  syscalls).

Host-validated through the rebuilt proot end to end: a guest
`socket(AF_NETLINK,SOCK_RAW,NETLINK_GENERIC)` + `sendmsg(CTRL_CMD_GETFAMILY
"nl80211")` + `recvmsg` got 228 bytes back with `CTRL_ATTR_FAMILY_ID = 0x24`,
ferried through a stub `@io.neoterm.wifi` running the real nl engine. So
synchronous genl/nl80211 commands (`iw dev`, `iw list`, `iw dev wlan0 info/link`,
GET_SCAN dump, CONNECT, NEW_KEY) work from the guest.

## W3b-3 — async scan events (NEW_SCAN_RESULTS) ✅ (host-validated)

Generation-based event delivery (mirrors the bridge's `uknl_scan_gen`/late-
subscriber logic):
- daemon: `ukw_nl_scangen()` (current gen, side-effect-free) and
  `ukw_nl_event(last_gen)` (builds a NEW_SCAN_RESULTS via the reused
  `uknl_build_scan_event` if a newer scan completed), served as
  `UK_OP_NL_SCANGEN` / `UK_OP_NL_EVENT`.
- proot: `setsockopt(SOL_NETLINK, NETLINK_ADD_MEMBERSHIP)` on a marked fd records
  the subscription (`last_gen=0` so an already-completed scan still fires once)
  and fakes success; `recvmsg` on a subscribed fd (with no stashed reply) returns
  one NEW_SCAN_RESULTS when the gen advanced; `poll`/`ppoll` reports the fd
  readable when a stash remains or `scangen != last_gen` (50ms cap so the guest
  re-polls). Seccomp adds setsockopt/poll/ppoll.

Host-validated through the rebuilt proot: a guest `socket(genl)` +
`setsockopt(ADD_MEMBERSHIP, scan)` + `poll` (→ POLLIN) + `recvmsg` received a
36-byte `nlmsg_type=0x24 cmd=NL80211_CMD_NEW_SCAN_RESULTS` event (the stub daemon
had `scan_gen=1`). So `iw scan`'s trigger→subscribe→event→GET_SCAN flow and
`wpa_supplicant`'s scan event work from the guest.

## Next (not done)

- **W3b-4** — `AF_PACKET` (monitor RX / injection / EAPOL for the 4-way handshake)
  and rtnetlink (`ip link`), plus MLME/connect events (extend the event path
  beyond scan-gen) for the full `wpa_supplicant` associate flow — the more
  preload-entangled `bridge/{packet_sock,rtnetlink}.c`.
- A chip's vendor driver `.so` in the module dir → end-to-end on device.
- A chip's vendor driver `.so` (built against the shim, like the proven
  `rtl8812au`) dropped into the module dir — then end‑to‑end on device.
