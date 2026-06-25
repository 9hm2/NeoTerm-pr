# USB-storage: real `mount` of a pendrive under proot (no LD_PRELOAD)

Goal: `mount /dev/uksd0 /mnt` works inside the proot guest for **any** filesystem
(vfat / exfat / ntfs3 / ext4), with full read **and** write — and *no* `LD_PRELOAD`
in the guest. Static binaries, scripts, anything works transparently.

This cannot use the real kernel `mount(2)` (the Android kernel can't read our
userspace-proxied sectors — see "Why not real mount" below). Instead proot
**emulates** a mount: it intercepts the FS syscalls under the mountpoint and
serves them from a userspace filesystem engine that runs the *real* Linux kernel
FS drivers (the uKernel runtime). It is, in effect, a FUSE client implemented in
proot's ptrace layer instead of `/dev/fuse`.

```
guest:  mount /dev/uksd0 /mnt ;  ls /mnt ;  cat /mnt/f ;  cp x /mnt/
           │  proot (ptrace) — NO LD_PRELOAD
           ▼  intercept mount(2)/umount2 + path & fd syscalls under a vmount
   ┌──────────────────────────┐
   │ proot VFS-redirect        │   src: proot/patches/fakeid0-xattr.py
   │  · vmount table           │   socket: abstract  io.neoterm.fs
   │  · guest fd → (path,off)  │
   └───────────┬──────────────┘
               │ io.neoterm.fs  (this doc)
   ┌───────────▼──────────────┐
   │ ukfsd  (native, Android)  │   vendored uKernel:  shim/fs/vfs.c (ukfs_* ops)
   │  vfs.c → fat/exfat/       │   + kernel_shim + linux/fs/{fat,exfat,ntfs3,ext4}
   │         ntfs3/ext4 .so    │
   └───────────┬──────────────┘
               │ io.neoterm.block  (sector R/W; already implemented)
   ┌───────────▼──────────────┐
   │ BlockBridge.kt            │   SCSI Bulk-Only-Transport
   └───────────┬──────────────┘
               ▼  usbfs → pendrive
```

Layering rule: **USB/SCSI stays in Kotlin (BlockBridge), FS parsing stays native
(ukfsd).** The two speak `io.neoterm.block` (sectors). proot never parses a
filesystem; ukfsd never touches USB.

## Why not real `mount(2)`

`mount(2)` asks the **kernel** to read the block device and parse the superblock.
Our sectors live in userspace (the block proxy turns the guest's `read`/`write`
on `/dev/uksd0` into SCSI to BlockBridge). The kernel can't see them — behind
`/dev/uksd0` it only finds a regular marker file on `/data`, so it errors
("failed to set up loop device"). Root + `mknod` + a real in-kernel block driver
would be required. FUSE is also out (needs `CAP_SYS_ADMIN` / `/dev/fuse`, both
denied by Android SELinux without root). Hence: emulate the mount in proot.

## proot side (VFS-redirect)

State (per proot process, keyed by guest pid where relevant):
- **vmount table**: `mountpoint` guest paths that are currently mounted (e.g. `/mnt`).
  A guest path is "in a vmount" if it equals or is under a mountpoint.
- **vfd table**: `{pid, fd, char path[], long long off, bool is_dir}` — a guest fd
  that was `openat`'d under a vmount. The kernel fd is real but points at a 0-byte
  placeholder; all of its I/O is proxied to ukfsd. This generalizes the existing
  `g_blk[]` block-proxy table.

Interception (at syscall ENTER, `uknl_block_dispatch`-style, gated by `UK_FS`):
- `mount(2)`: if source resolves to `/dev/uksd0` → `MOUNT auto uksd0` to ukfsd,
  add target to the vmount table, fake success (`PR_void`, result 0). `umount/umount2`
  on a vmount → `UMOUNT`, drop the entry.
- `openat`/`open`: if path is under a vmount → ask ukfsd `STAT` (dir vs file vs
  ENOENT, honoring `O_CREAT`). Rewrite the path argument to a **placeholder backing
  file** so the kernel returns a real fd, then record `vfd{path, off=0, is_dir}`.
- `read`/`pread64`/`lseek`/`close` on a vfd → ukfsd `READ` / seek math / drop entry.
- `write`/`pwrite64` on a vfd → ukfsd `WRITE`.
- `fstat`/`newfstatat`/`statx` on a vfd or a vmount path → ukfsd `STAT`, fill the
  struct (reuse `uksd_put_stat`/`uksd_put_statx`, mode from ukfs).
- `getdents64` on a vfd dir → ukfsd `LIST`, synthesize `linux_dirent64` records
  (reuse `uknl_put_dirent64`).
- `readlinkat`, `statfs/fstatfs`, `faccessat`, `mkdirat`, `unlinkat`, `renameat(2)`,
  `symlinkat`, `truncate`/`ftruncate`, `fchmodat`, `fchownat`, `utimensat` under a
  vmount → the matching ukfsd op.

Known limitations (document for the user):
- **`mmap` of files on the mount does not return mounted content** (the fd maps the
  zero placeholder). Tools that `read`/`write` (cat, cp, editors, tar, mkfs-of-files)
  work; `mmap`-readers and executing binaries *from* the mount do not. This is the
  same constraint FUSE `direct_io` has.
- One device / one mount at a time (single pendrive).

## ukfsd side (native server)

Replaces `bridge/preload_fs.c` (the LD_PRELOAD front-end) with a unix-socket
server front-end that dispatches to the **same** `vfs.c` `ukfs_*` ops. Block
backend reads/writes sectors over `io.neoterm.block` instead of in-process SCSI.
`vfs.c` is path-based and stateless per call, so ukfsd holds no per-fd state.

## `io.neoterm.fs` wire protocol

Abstract unix socket `\0io.neoterm.fs`, `SOCK_STREAM`. One persistent connection,
one mounted FS at a time. Every request is a text command line terminated by `\n`;
the **path is always the last field and runs to end-of-line** (so spaces in names
are fine — newlines in names are rejected). Binary payloads (file data, dir blobs,
two-path ops) are framed by explicit byte counts. Replies start `OK` or
`ERR <errno>`.

| Request | Payload | Reply (+ payload) |
|---|---|---|
| `MOUNT <fstype> <devtoken>` | — | `OK` \| `ERR <e>` |
| `UMOUNT` | — | `OK` |
| `STAT <path>` | — | `OK <mode> <uid> <gid> <size> <ino> <mtime> <atime> <nlink> <rdev> <blocks>` \| `ERR <e>` |
| `STATFS` | — | `OK <bsize> <blocks> <bfree> <bavail> <files> <ffree> <namelen> <frsize> <ftype>` \| `ERR <e>` |
| `LIST <path>` | — | `OK <count> <bytes>` + blob: `count` × `{u8 type, u64 ino, u64 size, u16 namelen, name[]}` (LE) |
| `READ <offset> <len> <path>` | — | `OK <n>` + `n` bytes \| `ERR <e>` |
| `WRITE <offset> <len> <path>` | `len` bytes | `OK <n>` \| `ERR <e>` |
| `READLINK <path>` | — | `OK <len>` + `len` bytes \| `ERR <e>` |
| `CREATE <mode> <path>` | — | `OK` \| `ERR <e>` |
| `MKDIR <mode> <path>` | — | `OK` \| `ERR <e>` |
| `UNLINK <path>` | — | `OK` \| `ERR <e>` |
| `RMDIR <path>` | — | `OK` \| `ERR <e>` |
| `TRUNCATE <size> <path>` | — | `OK` \| `ERR <e>` |
| `CHMOD <mode> <path>` | — | `OK` \| `ERR <e>` |
| `CHOWN <uid> <gid> <path>` | — | `OK` \| `ERR <e>` |
| `UTIME <asec> <ansec> <msec> <mnsec> <path>` | — | `OK` \| `ERR <e>` |
| `RENAME <oldlen> <newlen>` | `<old><new>` bytes | `OK` \| `ERR <e>` |
| `SYMLINK <tlen> <llen>` | `<target><link>` bytes | `OK` \| `ERR <e>` |

`type` in LIST: 1 = dir, 2 = file (matches `ukfs_dirent.type`). `mode` is a full
POSIX `st_mode` (type bits + perms). `devtoken` is currently fixed `uksd0`.

`MOUNT auto …` autodetects the filesystem (the engine probes vfat/exfat/ntfs3/ext4),
matching `libukfs_all.so`'s lazy mount.

## Build / wiring

- Vendored uKernel FS engine: `app/src/main/cpp/ukfs/` (kernel_shim, `vfs.c`,
  `linux/fs/{fat,exfat,ntfs3,ext4}`, `ukfsd.c` server front-end).
- NDK build (CMake) cross-compiles for `aarch64` / bionic. The kernel FS drivers
  were written for glibc; bionic gaps are absorbed in the shim (iteration on-device).
- `FsBridge.kt` launches `ukfsd`, ensures `io.neoterm.fs` + `io.neoterm.block` are
  up, and ties their lifecycle to the proot session.
- `ProotManager` passes `UK_FS=1` (alongside `UK_BLOCK=1`) when USB-storage is on,
  enabling the proot VFS-redirect.
</content>
</invoke>
