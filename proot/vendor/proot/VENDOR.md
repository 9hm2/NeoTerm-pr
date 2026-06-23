# Vendored: Termux PRoot fork

This directory contains a **vendored copy** of the Termux fork of PRoot, so the
app builds PRoot from in-repo sources instead of downloading them at build time
(reproducible, offline, and pinnable).

| | |
|---|---|
| Upstream | https://github.com/termux/proot |
| Branch | `master` |
| Pinned commit | `cd02c79e0ff84db3a22616e49ad44b0cc22ef4f2` |
| License | GPL-2.0 (see `COPYING`) |

Only the buildable `src/` tree (plus `COPYING` / `README.md` / `SECURITY.md`)
is vendored; upstream `doc/` and `tests/` are omitted.

## Why the Termux fork (not proot-me/proot)

The upstream proot-me build is killed by Android's seccomp filter (SIGSYS /
signal 31) because the kernel blocks the ptrace syscall. The Termux fork carries
the kernel-hook patches needed to work around this on Android.

## How it is built

`proot/build-proot.sh` copies this tree into a fresh work dir, then applies our
build-time mutations **to the copy** (this vendored tree stays pristine):

1. strips the CPython extension (cross-compile incompatible),
2. drops the `--rosegment` loader flag (lld already separates rodata),
3. `fake_id0/stat.c`: report euid/egid (not the saved suid/sgid),
4. `patches/fakeid0-xattr.py`: xattr-backed persistent ownership + the
   statx handler + the socket-fd `fstat` fix.

The xattr storage logic is verified on a host by
`proot/patches/fakeid0-xattr-test/run.sh`.

## Updating the pin

To move to a newer upstream commit, re-vendor the `src/` tree from that commit,
update the **Pinned commit** above, and re-run the host test + a device build to
confirm our patches still apply (the patch script asserts via `must()` and will
fail loudly if upstream moved a line it edits).
