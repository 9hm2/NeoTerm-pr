# uKernel FS engine — Android/aarch64 (bionic) cross-build.
#
# Runs the REAL Linux kernel FS drivers (linux/fs/*) in userspace against
# uKernel's fake kernel headers (include/) + the core kernel-API shim
# (shim/core) + the VFS bridge (shim/fs/vfs.c). This file builds the
# standalone mount+list test (ukfs_test_vfat) to validate the cross-compile;
# the ukfsd socket server is layered on top once this is green.
#
# Mirrors build/build_ukfs.sh from the uKernel project (host gcc) but for the
# NDK toolchain. See docs/USB_STORAGE_MOUNT.md.

set(UKFS_DIR ${CMAKE_CURRENT_LIST_DIR})
set(UKFS_INC ${UKFS_DIR}/include)

# Common flags for the kernel-side translation units (drivers + vfs + shim).
# -D__KERNEL__/-DMODULE: the fake headers present the in-kernel API.
# -fshort-wchar: the kernel treats wchar_t as 16-bit (FAT/exFAT/NTFS name tables).
# -fno-builtin/-fno-strict-aliasing: kernel code assumes these.
set(UKFS_KCFLAGS
  -fPIC -O2 -fno-strict-aliasing -fno-builtin -fshort-wchar -D_GNU_SOURCE
  -D__KERNEL__ -DMODULE
  # clang (NDK) promotes these kernel-isms to hard errors; the original gcc
  # build left them as warnings. The kernel relies on implicit decls resolving
  # at link and on loose function-pointer signatures.
  -Wno-implicit-function-declaration -Wno-error=implicit-function-declaration
  -Wno-incompatible-pointer-types -Wno-incompatible-function-pointer-types
  -Wno-unused -Wno-unused-parameter -Wno-sign-compare
  -Wno-implicit-fallthrough -Wno-missing-braces -Wno-unknown-pragmas)

# --- core kernel-API shim (kmalloc/printk/kmem_cache/mutex/module_inits/...) ---
file(GLOB UKFS_SHIM_CORE ${UKFS_DIR}/shim/core/*.c)
# fileio.c pulls usb/net glue we don't need for the FS path; drop it.
list(FILTER UKFS_SHIM_CORE EXCLUDE REGEX "/fileio\\.c$")

# --- vfat driver: only the vfat namei (msdos namei module-init would clash) ---
set(UKFS_FAT_SRC cache dir fatent file inode misc nfs namei_vfat)
set(UKFS_FAT_DEFS
  -DCONFIG_VFAT_FS=1 -DCONFIG_FAT_FS=1 -DCONFIG_FAT_DEFAULT_CODEPAGE=437
  -DCONFIG_FAT_DEFAULT_IOCHARSET="iso8859-1" -DCONFIG_FAT_DEFAULT_UTF8=0)

set(UKFS_OBJ_SRCS "")
foreach(c ${UKFS_FAT_SRC})
  list(APPEND UKFS_OBJ_SRCS ${UKFS_DIR}/linux/fs/fat/${c}.c)
  # Each driver file needs a UNIQUE KBUILD_MODNAME so the fat-core init
  # (inode cache) and the vfat init (register_filesystem) land in separate
  # module slots and BOTH run.
  set_source_files_properties(${UKFS_DIR}/linux/fs/fat/${c}.c PROPERTIES
    COMPILE_DEFINITIONS "KBUILD_MODNAME=\"vfat_${c}\"")
endforeach()

# --- shared FS engine: the vfat driver + VFS/ACL shim + kernel-API shim,
#     compiled once and linked into both the test harness and the ukfsd server. ---
add_library(ukfs_engine OBJECT
  ${UKFS_DIR}/shim/fs/vfs.c
  ${UKFS_DIR}/shim/fs/posix_acl.c
  ${UKFS_DIR}/shim/compat_bionic.c   # backtrace/hex/system_wq/get_random_u32 shims
  ${UKFS_OBJ_SRCS}
  ${UKFS_SHIM_CORE})
target_include_directories(ukfs_engine PRIVATE ${UKFS_INC} ${UKFS_DIR}/linux/fs/fat)
target_compile_options(ukfs_engine PRIVATE ${UKFS_KCFLAGS} ${UKFS_FAT_DEFS})

# --- ukfsd: io.neoterm.fs unix-socket server (the Android FS daemon) ---
add_executable(ukfsd ${UKFS_DIR}/shim/fs/ukfsd.c $<TARGET_OBJECTS:ukfs_engine>)
target_include_directories(ukfsd PRIVATE ${UKFS_INC} ${UKFS_DIR}/linux/fs/fat)
target_compile_options(ukfsd PRIVATE ${UKFS_KCFLAGS} ${UKFS_FAT_DEFS})
target_link_libraries(ukfsd dl)

# --- ukfs_test_vfat: standalone mount/list/read/write harness (dev/debug) ---
add_executable(ukfs_test_vfat ${UKFS_DIR}/shim/fs/ukfs_test.c $<TARGET_OBJECTS:ukfs_engine>)
target_include_directories(ukfs_test_vfat PRIVATE ${UKFS_INC} ${UKFS_DIR}/linux/fs/fat)
target_compile_options(ukfs_test_vfat PRIVATE ${UKFS_KCFLAGS} ${UKFS_FAT_DEFS})
target_link_libraries(ukfs_test_vfat dl)
