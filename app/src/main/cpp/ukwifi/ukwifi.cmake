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
#     sched/module-inits/... fileio.c pulls glue we don't want here; drop it. ---
file(GLOB UKW_SHIM_CORE ${UKFS_DIR}/shim/core/*.c)
list(FILTER UKW_SHIM_CORE EXCLUDE REGEX "/fileio\\.c$")

# ============================================================================
# Kernel-side translation units: the fake <linux/*>,<net/*> headers must come
# FIRST (-I), so they shadow the NDK sysroot. The net shim + cfg80211 implement
# the kernel symbols the (future) driver links against.
# ============================================================================
add_library(ukwifi_kshim OBJECT
  ${UKW_DIR}/shim/net/netdev.c
  ${UKW_DIR}/shim/net/skbuff.c
  ${UKW_DIR}/shim/net/ieee80211.c
  ${UKW_DIR}/stack/cfg80211/cfg80211_core.c
  ${UKW_DIR}/stack/mac80211/mac80211_core.c
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
  ${UKW_DIR}/server/userver.c)      # loader + UK_OP_* proxy (io.neoterm.wifi)
target_compile_options(ukwifi_user PRIVATE ${UKW_CFLAGS} -idirafter ${UKW_INC})

# Aggregate framework (driver-independent). The runnable daemon target
# (libukwifid.so) is added in W0b once userver is adapted for single-binary use.
add_library(ukwifi_framework STATIC
  $<TARGET_OBJECTS:ukwifi_kshim>
  $<TARGET_OBJECTS:ukwifi_user>)
set_target_properties(ukwifi_framework PROPERTIES LINKER_LANGUAGE C)
