# Chip‑agnostic framework for real kernel modules (.ko) — direction analysis

> **DECISION (rejected): the .ko / LKL direction is NOT pursued.** After review we
> stay with the existing uKernel **shim + `.so`** approach (see `DESIGN.md`): a
> chip‑agnostic framework where each chip's vendor driver is compiled to a `.so`
> and plugged in later; the driver is not needed for the framework itself. This
> document is kept only as the record of why `.ko`/LKL was considered and dropped
> (real‑`.ko` loading needs the real kernel ABI → LKL → too heavy / version‑locked
> for this target).

---


Revised requirement (supersedes the per‑chip `.so` plan in `DESIGN.md` for the
*framework* layer):

1. **Any chip** must work.
2. The **driver comes from the guest via `modprobe`** — we do not pre‑build it.
3. It must run a **real kernel module (`.ko`)**, not a recompiled `.so`.
4. Deliver only the **fully prepared framework** now; drivers come later.

## Why the current uKernel approach can't meet this

uKernel compiles each vendor driver **to a `.so`** and links it against a
**hand‑written shim** whose `struct` layouts are filled in iteratively per driver.
That is excellent for proving one chip, but:

- A real `.ko` is an ELF **relocatable kernel object** built against a **specific
  kernel version's ABI** — struct layouts, inlined header code, and `CONFIG_*` are
  *baked into the object*. "Any chip via the distro's `modprobe`" means loading
  objects built for the **guest distro's kernel**, whose ABI our hand‑shim does not
  match. You cannot hand‑maintain byte‑compatible layouts for the whole
  `cfg80211`/`mac80211`/`netdev`/`usb` surface across kernel versions.
- `.so` ≠ `.ko`: a `.so` is a pre‑linked shared object we built; a `.ko` is what
  `modprobe` loads. Requirement (3) explicitly rules out the `.so` path.

So the from‑scratch shim is the wrong foundation for *this* goal. It stays useful
only as the proof that "an unmodified Wi‑Fi driver can be driven over our usbfs fd
path" — which we now generalise.

## The only way a real, unmodified `.ko` runs correctly

The surrounding kernel ABI must be the **real** one. Two ways to get that:

| Option | What it is | Meets "any chip + guest modprobe + real .ko"? |
|---|---|---|
| **A. hand‑shim + userspace `insmod`** | write an in‑process ELF `.ko` loader, resolve symbols against the hand‑shim | **No.** Still ABI‑locked to whatever the shim happens to mirror; every new driver re‑opens the layout/iteration work. Fragile. |
| **B. LKL (Linux Kernel Library)** | compile the **real upstream kernel** as a userspace library (`arch/lkl`), with the **real** module loader, `cfg80211`/`mac80211`/`nl80211`/`netdev`/`usb‑core` | **Yes.** `.ko`s built against the matching kernel headers load via the **real** `init_module`; the real wireless stack is present; any in‑tree (or DKMS‑built) driver works. |
| C. UML / microVM (KVM) | a real kernel in a VM with USB passthrough | No — needs root/kvm; too heavy for proot/Android. |

**Recommendation: B (LKL), with a tailored config.** It is the architecture that
directly delivers the stated goal: real kernel, real modules, real
`cfg80211`/`mac80211`/`nl80211`, and — as a bonus — a real netdev + TCP/IP stack
(which is exactly the L3 gap the per‑chip plan flagged at W5).

## The LKL‑based framework (chip‑agnostic, driver‑from‑guest)

```
guest: modprobe 88XXau   iw   wpa_supplicant   dhcpcd   ip   ping
        │ finit_module(.ko)         │ AF_NETLINK / AF_PACKET / AF_INET / ioctl
        ▼ (proot UK_WIFI redirect — generic syscall forwarding)
   ┌──────────────────────────────────────────────────────────────┐
   │ ukwifid (app‑side daemon, NDK/bionic)                         │
   │   liblkl  =  REAL kernel as a library                         │
   │     • real module loader  → loads the guest's .ko             │
   │     • REAL cfg80211 / mac80211 / nl80211 / netdev / TCP-IP    │
   │     • USB core + **usbfs passthrough HCD** (the one new       │
   │       kernel-side driver we write)                            │
   └───────────────┬──────────────────────────────────────────────┘
                   │ URB → USBDEVFS_SUBMITURB/REAPURB on the chip fd
                   ▼  (fd from io.neoterm.usb, SCM_RIGHTS — Phase 2)
              real USB Wi‑Fi chip
```

What we build (the "prepared framework"), all driver‑agnostic:

1. **`liblkl` for aarch64/bionic** with a pinned kernel version and a config that
   enables: `MODULES` (+ unsigned/forced load), `CFG80211`, `MAC80211`, `NL80211`,
   `USB`, `NET`, the netdev/TCP‑IP core — but **no specific Wi‑Fi drivers** (those
   arrive as `.ko`). NDK build, like `ukfs`.
2. **usbfs passthrough HCD** — a small in‑LKL host controller driver that turns
   Linux `urb`s into `USBDEVFS_SUBMITURB`/`REAPURBNDELAY` on the chip fd we get
   from `io.neoterm.usb`. This is the single novel kernel‑side component; the URB↔
   usbfs mapping already exists in `hcd/usbfs_hcd.c` and our Phase‑2 proxy.
3. **`uknl_wifi_redirect.c` (proot, `UK_WIFI`)** — *generic* syscall forwarding,
   not nl80211‑specific: the guest's `AF_NETLINK` (GENERIC+ROUTE), `AF_PACKET`,
   wext `ioctl`s, and `finit_module`/`init_module`/`delete_module` are forwarded to
   the corresponding **`lkl_sys_*`** calls in the daemon. Because LKL has the real
   netlink/module syscalls, this works for *any* driver and *any* nl80211 tool with
   no per‑command code.
4. **module delivery** — `modprobe` in the guest loads `/lib/modules/<ver>/…/*.ko`;
   we intercept `finit_module` and hand the bytes to `lkl_sys_init_module`. For the
   ABI to match, the guest's modules must be built for the **pinned LKL kernel
   version** — so we ship a matching `linux-headers`‑style package (or prebuilt
   `.ko`s) so `apt install …`/DKMS produce loadable modules. This is the realistic
   "any chip via modprobe" contract.
5. **`UsbWifiBridge.kt`** + **Settings "USB Wi‑Fi"** + **`UK_WIFI`** gate +
   **`/sys/class/net` / `/sys/class/ieee80211`** surfaced from LKL — the same
   bridge/toggle pattern as every other capability.
6. **L3 (bonus, now natural):** since LKL has a real netdev + TCP/IP stack, guest
   `AF_INET` sockets can be forwarded to LKL too, giving transparent IP over the
   chip — the hard W5 problem solved by construction rather than per‑app helpers.

## Honest costs & risks of the LKL direction

- **Build complexity & size.** LKL is a real kernel build; aarch64/bionic/NDK
  cross‑compile needs care (it has been done, but expect patching). The lib is
  multi‑MB. The wireless+USB config grows it.
- **The passthrough HCD is real kernel work.** It must present a believable HCD so
  USB‑core enumerates the already‑open device and routes URBs to usbfs. Doable
  (conceptually a usbip/vhci backed by usbfs), but it is the main new driver.
- **Kernel‑version pinning.** "Any chip via the distro's own `modprobe`" is only
  literally true when the guest's `.ko` matches the pinned LKL version; otherwise
  we provide matching headers/prebuilt modules. This is inherent to real `.ko`
  loading, not a shortcoming of the design.
- **Threading/timing.** LKL runs the kernel on host threads; the USB pump,
  workqueues and timers must be driven — manageable (LKL has a host‑ops model),
  but it is more moving parts than the single‑threaded shim.
- **Effort.** This is the largest of the bridges so far. It is, however, the only
  path that satisfies all three requirements at once.

## Decision needed

The three requirements (any chip / guest `modprobe` / real `.ko`) point to **LKL**.
The from‑scratch shim cannot meet them; option A (userspace `insmod` against the
shim) inherits the shim's ABI fragility. Before investing in the LKL build, this
direction (and the kernel‑version‑pinning consequence for guest modules) should be
confirmed — see the chat for the go/no‑go.
