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

## Next (not done)

- **W1+** — per `DESIGN.md`: `UsbWifiBridge.kt` launch + chip claim; bring a
  chip up (`probe`/`ndo_open`) once a driver `.so` is provided; the
  `/sys/class/net` + `/sys/class/ieee80211` bridge; the `uknl_wifi_redirect.c`
  control‑plane (AF_NETLINK/AF_PACKET/wext → `io.neoterm.wifi`); Settings
  toggle + `UK_WIFI` gate.
