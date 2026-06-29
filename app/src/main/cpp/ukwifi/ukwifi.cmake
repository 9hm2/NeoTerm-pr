# uKernel Wi-Fi framework — Android/aarch64 (bionic) cross-build.
#
# Chip-AGNOSTIC framework only: the kernel-API shim (reused from ../ukfs),
# the net shim (netdev/skbuff/ieee80211), the custom cfg80211 + mac80211-stub,
# the usbfs-passthrough HCD (talks to io.neoterm.usb), and the userver loader/
# proxy. NO Wi-Fi driver is bundled — a chip's vendor driver .so is loaded by
# userver at runtime (see wifi/DESIGN.md). Mirrors the uKernel host build
# (build/Makefile + build/module.mk) but for the NDK toolchain, and reuses the
# already-bionic-ported headers + core shim from the FS engine.
#
# W0 milestone: the framework compiles for aarch64/bionic. The runnable daemon
# (libukwifid.so) needs userver's single-binary adaptation (skip the dlopen of
# the statically-linked shim/cfg80211) — that is W0b, layered on top of this.

set(UKW_DIR ${CMAKE_CURRENT_LIST_DIR})
set(UKFS_DIR ${UKW_DIR}/../ukfs)          # shared headers + core kernel-API shim
set(UKW_INC ${UKFS_DIR}/include)

# Warning suppressions: clang (NDK) promotes several kernel-isms to hard errors
# that the original gcc build left as warnings (same set ukfs.cmake uses, plus
# the macro/visibility noise from the fake headers).
set(UKW_WARN
  -Wno-implicit-function-declaration -Wno-error=implicit-function-declaration
  -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types
  -Wno-unused -Wno-unused-parameter -Wno-sign-compare
  -Wno-implicit-fallthrough -Wno-missing-braces -Wno-unknown-pragmas
  -Wno-macro-redefined -Wno-visibility)

set(UKW_CFLAGS -fPIC -O2 -fno-strict-aliasing -D_GNU_SOURCE -pthread ${UKW_WARN})

# --- core kernel-API shim (shared with the FS engine): kmalloc/printk/sync/
#     sched/module-inits/... INCLUDING fileio.c, which carries kernel globals +
#     helpers a Wi-Fi driver needs and nothing else here provides: init_net (the
#     initial netns the driver references), filp_open/kernel_read/kernel_write
#     (efuse/firmware file reads), dev_alloc_name (wlan%d), get_random_* (strong;
#     compat_bionic's is weak and yields), the radiotap iterator. The FS engine
#     drops fileio.c (it has its own vfs.c); the Wi-Fi daemon needs it. ---
file(GLOB UKW_SHIM_CORE ${UKFS_DIR}/shim/core/*.c)

# ============================================================================
# Kernel-side translation units: the fake <linux/*>,<net/*> headers must come
# FIRST (-I), so they shadow the NDK sysroot. The net shim + cfg80211 implement
# the kernel symbols the (future) driver links against.
# ============================================================================
add_library(ukwifi_kshim OBJECT
  ${UKW_DIR}/shim/net/netdev.c
  ${UKW_DIR}/shim/net/skbuff.c
  ${UKW_DIR}/shim/net/ieee80211.c
  ${UKW_DIR}/shim/usb/usb_core.c            # USB core: usb_register, enumerate+probe, URB
  ${UKW_DIR}/stack/cfg80211/cfg80211_core.c
  ${UKW_DIR}/stack/mac80211/mac80211_core.c
  ${UKW_DIR}/shim/fw.c                       # request_firmware -> guest /lib/firmware
  ${UKFS_DIR}/shim/compat_bionic.c
  ${UKW_SHIM_CORE})
target_include_directories(ukwifi_kshim BEFORE PRIVATE ${UKW_INC})
target_compile_options(ukwifi_kshim PRIVATE ${UKW_CFLAGS} -DKBUILD_MODNAME="ukwifi")

# ============================================================================
# Userspace translation units: real system headers must win, so the fake tree
# is added with -idirafter (only the ukernel/*.h internal contracts are picked
# up from it; <linux/usbdevice_fs.h> etc. resolve to the real sysroot).
# ============================================================================
add_library(ukwifi_user OBJECT
  ${UKW_DIR}/hcd/usbfs_hcd.c        # URB <-> USBDEVFS over the io.neoterm.usb fd
  ${UKW_DIR}/hcd/mock_hcd.c         # device-less HCD backend (testing)
  ${UKW_DIR}/server/modmgr.c        # modprobe/rmmod/lsmod -> dlopen vendor driver .so
  ${UKW_DIR}/server/wsysfs.c        # /sys/class/net + /sys/class/ieee80211 writer
  ${UKW_DIR}/shim/libc_compat.c     # __isoc23_* glibc-ABI forwarders for the dlopen'd driver
  # netlink engine (genl + full nl80211 cmds, reused from the uKernel bridge) +
  # the in-process userver_client adapter — driven by UK_OP_NL.
  ${UKW_DIR}/nl/netlink_msg.c
  ${UKW_DIR}/nl/nl_dispatch.c
  ${UKW_DIR}/nl/genl_ctrl.c
  ${UKW_DIR}/nl/nl80211_cmds.c
  ${UKW_DIR}/nl/rtnetlink.c
  ${UKW_DIR}/nl/userver_client_inproc.c
  ${UKW_DIR}/nl/nlglue.c)
target_include_directories(ukwifi_user PRIVATE ${UKW_DIR}/nl)
target_compile_options(ukwifi_user PRIVATE ${UKW_CFLAGS} -idirafter ${UKW_INC})

# ============================================================================
# ukwifid: the io.neoterm.wifi daemon (loader + UK_OP_* proxy), single-binary.
# Output as libukwifid.so (a PIE executable) so AGP packages it into jniLibs and
# Android extracts it executable to nativeLibraryDir — same trick as ukfsd. The
# shim + cfg80211 are statically linked in; userver resolves them via dlopen(NULL)
# (hence -Wl,--export-dynamic), and dlopens only the chip's vendor driver .so at
# runtime. No driver is bundled.
# ============================================================================
add_executable(ukwifid
  ${UKW_DIR}/server/userver.c
  $<TARGET_OBJECTS:ukwifi_kshim>
  $<TARGET_OBJECTS:ukwifi_user>)
set_target_properties(ukwifid PROPERTIES PREFIX "lib" OUTPUT_NAME "ukwifid" SUFFIX ".so")
# userver.c is userspace: real headers win, fake tree only for ukernel/*.h.
target_compile_options(ukwifid PRIVATE ${UKW_CFLAGS} -idirafter ${UKW_INC})
target_link_options(ukwifid PRIVATE -Wl,--export-dynamic)
target_link_libraries(ukwifid dl)
