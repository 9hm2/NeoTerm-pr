# USB Wi‑Fi in proot — feasibility & toggle‑based wiring (preparation)

Goal: bring a **real USB Wi‑Fi adapter** (RTL8811AU and friends) up inside the
distro the same way the filesystem stack was added — an **NDK‑built app‑side
daemon** + a **proot syscall redirect** + a **Kotlin bridge** + a **Settings
toggle** + a **`UK_*` env gate**, with **no `LD_PRELOAD` and no guest changes**.

This document assesses the uploaded **uKernel** project (a proven userspace
Wi‑Fi stack) for buildability into **bionic/aarch64** and lays out the wiring,
so the implementation can start from a known plan.

> **DECISION (active plan):** we build on the uKernel **shim + `.so`** approach.
> The `.ko`/LKL alternative was analysed and **rejected** (see
> `DESIGN-ko-framework.md`). The framework is **chip‑agnostic**: the daemon loads
> a chip's vendor driver `.so` at runtime, so **no driver is bundled now** — only
> the fully‑prepared, driver‑independent framework is built; a specific chip's
> `.so` is plugged in later. (No `modprobe`/`.ko` path.)

---

## 1. What the uKernel project already is (and proves)

uKernel runs the **unmodified `rtl8812au` (88XXau) kernel driver compiled to a
`.so`** in userspace. A from‑scratch **`libkernel_shim.so`** implements the
kernel API the driver calls (printk, slab, sk_buff, netdev, sync, timers,
workqueues, USB‑core); a custom **`cfg80211.so`** provides wiphy/scan/bss/
connect/key; the driver's USB I/O goes through an **HCD bridge** to the real
chip. Standard Linux tools (`iw`, `wpa_supplicant`, `dhcpcd`, `ping`,
`airmon‑ng`, `airodump‑ng`, `aireplay‑ng`) drive it through an **`nl80211`
bridge**.

Proven end‑to‑end on the real chip, from userspace: scan → WPA2 association →
4‑way handshake → DHCP (real IP) → `ping` → monitor mode → **real RF injection**.

**The decisive fact for us:** the HCD backend already in the tree —
`hcd/usbfs_hcd.c` — talks to **`io.neoterm.usb`** (our Phase‑2 fd server): it
sends `LIST`, finds the `vid:pid` path, receives the device's **usbfs fd via
`SCM_RIGHTS`**, and drives it with `USBDEVFS_*` ioctls (sync `CONTROL`/`BULK`,
async `SUBMITURB`/`REAPURBNDELAY`) — **no libusb** (their aarch64 libusb event
loop crashed; ours is the same conclusion). So the Wi‑Fi daemon consumes exactly
the fd path we built, runs **app‑side like `ukfsd`**, and needs proot only for
the *guest‑facing* control plane.

---

## 2. The cfg80211 / mac80211 / nl80211 question — answered

| Layer | In uKernel today | Verdict for us |
|---|---|---|
| **cfg80211** | a **custom ~400‑line `.so`** (not the kernel's ~30k‑line cfg80211) implementing just what a full‑MAC driver needs: `wiphy_new/register`, `cfg80211_scan_done/inform_bss`, `cfg80211_connect_result`, `add_key`, channel/monitor | **Keep the custom shim.** It is small, bionic‑portable, and sufficient. Vendoring the real kernel cfg80211 would drag in nl80211.c, regulatory, RCU, genl, netns — not worth it. |
| **mac80211** | a **25‑line stub** (`alloc_hw`/`register_hw` only) | **Not needed for full‑MAC drivers.** `rtl8812au` brings its own MAC and talks cfg80211 directly. A *real* mac80211 (for soft‑MAC chips like ath9k_htc, mt7601u, rtl8187, carl9170) is a **huge, deeply kernel‑entangled** port (rate control, aggregation, TX/RX fast path, hrtimers, RCU, crypto) — explicitly out of scope. Strategy: **per‑chip full‑MAC vendor drivers** (each ships its own MAC) over the custom cfg80211 shim. |
| **nl80211** | a **userspace genetlink dispatcher** living in the `LD_PRELOAD` bridge (`bridge/nl80211_cmds.c` + `nl_dispatch.c`), plus rtnetlink, AF_PACKET and an ICMP socket | **Reuse the message logic, relocate the transport.** The byte‑level handlers stay; the `LD_PRELOAD` interception is replaced by a **proot redirect** (no preload in the guest). |

So the answer to "can nl80211/cfg80211/mac80211 be built into bionic": **cfg80211
yes (custom shim, easy); nl80211 yes (as message logic in the daemon, fed by a
proot socket redirect); mac80211 — only the stub, and that's the right call —
full mac80211 is not a realistic bionic port and isn't needed for full‑MAC USB
Wi‑Fi.**

---

## 3. Mapping onto the established "filesystem method"

The filesystem feature is the template (`app/src/main/cpp/ukfs/` → `libukfsd.so`
launched by `FsBridge`, serving `io.neoterm.fs`, with a proot VFS‑redirect, a
`/sys/block` bridge, a Settings toggle and `UK_FS`/`UK_BLOCK` gates). Wi‑Fi maps
one‑to‑one:

| Filesystem stack | Wi‑Fi analogue |
|---|---|
| `libukfsd.so` (NDK daemon, in `jniLibs`) | **`libukwifid.so`** — uKernel `userver` + `libkernel_shim` + `cfg80211.so` + `rtl8812au.so` + `usbfs_hcd`, linked/loaded as one app‑side daemon |
| serves `io.neoterm.fs` | serves **`io.neoterm.wifi`** (the `UK_OP_*` protocol already exists in `server/userver.c`) |
| gets sectors from `BlockBridge` over `io.neoterm.block` | gets the **usbfs fd from `UsbBridge`** over `io.neoterm.usb` (already implemented in `usbfs_hcd.c`) |
| `FsBridge.kt` launches/supervises it | **`UsbWifiBridge.kt`** launches/supervises it; requests USB permission so the chip is `[granted]` in the `io.neoterm.usb` LIST; claims the chip so the generic raw‑fd path doesn't double‑own it (same pattern as the serial/block bridges) |
| proot **VFS‑redirect** routes guest path/`mount` syscalls | proot **`UK_WIFI` redirect** routes the guest control plane (below) |
| `/sys/block` + `/sys/dev/block` bridge | **`/sys/class/net/wlan0` + `/sys/class/ieee80211/phy0`** bridge (a `UsbSysfsBridge`‑style fake tree) |
| Settings → "USB storage" → `UK_BLOCK`/`UK_FS` | Settings → **"USB Wi‑Fi"** → **`UK_WIFI`** |

### The one genuinely new piece: the control‑plane redirect

The `LD_PRELOAD` bridge already shows exactly what the guest's Wi‑Fi tooling
touches, and it already converts each into a socketpair to the daemon. proot does
the same at the syscall layer (this is the new `uknl_wifi_redirect.c`, gated by
`UK_WIFI`, modelled on `uknl_usb_redirect.c`):

- **`socket(AF_NETLINK, *, NETLINK_GENERIC)`** and **`NETLINK_ROUTE`** → connect a
  socket to `io.neoterm.wifi`; the daemon runs the genl/nl80211 family and
  rtnetlink (`bridge/nl80211_cmds.c`, `rtnetlink.c` move server‑side). `sendmsg`/
  `recvmsg` on that fd are ferried.
- **`socket(AF_PACKET, …)`** → monitor/injection + EAPOL data path to the daemon
  (`bridge/packet_sock.c` logic, server‑side).
- **`socket(AF_INET, …, IPPROTO_ICMP)`** → `ping` over the chip (or rely on the
  guest's own stack once an IP is configured; see §6 limits).
- **wext `ioctl`s** (`SIOCGIWFREQ`, `SIOCSIWMODE`, `SIOCGIFHWADDR`, …) → daemon.
- **`/sys/class/net/*` and `/sys/class/ieee80211/*` reads** → the fake sysfs
  bridge (no redirect needed; it's a bound tree, like `/sys/block`).
- **`if_nametoindex`** resolves against the fake `/sys/class/net/wlan0`.

Because proot already binds real mounts and traps sockets per‑`UK_*` gate, this
slots in beside the USB/cam/block redirects with the same machinery
(`set_sysnum(PR_void)` + poke result, or socketpair substitution as the USB shim
does for netlink).

---

## 4. Bionic build assessment (per component)

Current build: `gcc` + glibc + `-D_GNU_SOURCE`, kernel UAPI from a prepared
`linux/` tree (`build/build_driver.sh`, `build/rtl8812au.mk`). Target: NDK clang,
aarch64, bionic — exactly what `ukfs.cmake` already does for the FS drivers.

| Component | LOC | Bionic portability | Effort / risk |
|---|---|---|---|
| `libkernel_shim` (core/net/usb) | ~moderate | pthread + malloc + usbfs ioctls — the same primitives `ukfs`'s shim already uses on bionic | **Low.** Reuse ukfs's shim conventions. |
| `usbfs_hcd.c` | 213 | pure POSIX + `linux/usbdevice_fs.h` + abstract socket; already bionic‑shaped | **Very low.** Compiles almost as‑is. |
| `cfg80211.so` (custom) | ~400 | plain C, malloc/string/time | **Low.** |
| `mac80211` stub | 25 | trivial | **Low** (kept as stub). |
| `rtl8812au.so` | ~390k (145 files) | C, kernel‑style; compiles clean vs the shim + kernel UAPI on gcc | **Medium.** clang‑vs‑gcc diagnostics, a few bionic gaps (`error()`, some str/locale helpers), and the kernel UAPI must be vendored (already done for ukfs). The FS drivers were a comparable lift and succeeded. |
| nl80211/rtnetlink/packet message logic | ~900 | self‑contained byte builders/parsers | **Low–medium** to relocate from preload into the daemon. |
| **new** `uknl_wifi_redirect.c` (proot) | — | new, modelled on the USB shim | **Medium.** The real new work; netlink/packet ferrying + wext + sysfs. |

No blocker is fundamental. The two real efforts are (a) the NDK build of the
~390k‑LOC vendor driver (mechanical, compiler‑driven, like ukfs), and (b) the new
proot control‑plane redirect (design‑complete here).

---

## 5. Phased roadmap (toggle‑based, revertible)

Each phase is independently testable; everything gates on `UK_WIFI` and a
Settings toggle, master stays shippable.

- **W0 — vendoring & NDK build.** Import uKernel `shim/`, `stack/cfg80211`,
  `hcd/usbfs_hcd.c`, `server/userver.c` and a pinned `rtl8812au` snapshot under
  `app/src/main/cpp/ukwifi/`; add `ukwifi.cmake` (mirror `ukfs.cmake`) producing
  `libukwifid.so`. Goal: it links for aarch64/bionic, 0 unresolved symbols.
- **W1 — daemon brings the chip up.** `UsbWifiBridge.kt` launches `libukwifid.so`;
  it acquires the chip fd from `io.neoterm.usb`, runs `module_init` →
  `usb_register` → `probe` → `ndo_open` (firmware + register sequences). Success =
  `dmesg` shows `wlan0` + dual‑band wiphy on the real chip. (uKernel's F8c/F9.)
- **W2 — sysfs bridge.** Fake `/sys/class/net/wlan0`, `/sys/class/ieee80211/phy0`,
  `/sys/class/net/wlan0/device → …` so `iw dev`, `ip link`, `if_nametoindex` see
  the interface. (Like `UsbSysfsBridge`/`/sys/block`.)
- **W3 — control plane redirect.** `uknl_wifi_redirect.c`: AF_NETLINK
  (GENERIC+ROUTE) ↔ `io.neoterm.wifi`; move `nl80211_cmds`/`rtnetlink` server‑side.
  Goal: `iw dev wlan0 scan` returns real APs in the guest.
- **W4 — WPA2 + data path.** AF_PACKET (EAPOL) + key install; `wpa_supplicant -D
  nl80211` reaches CONNECTED; encrypted data frames flow.
- **W5 — IP.** Bring the interface's IP/route up for the guest (see §6 — TUN/TAP
  is blocked, so either a small in‑daemon DHCP+L3 shim feeding the guest, or a
  user‑space `uk_dhcp`/`uk_ping` analogue; decide at W5).
- **W6 — monitor/injection (optional).** AF_PACKET monitor RX + radiotap TX so
  `airmon‑ng`/`airodump‑ng`/`aireplay‑ng` work (uKernel already proves this).

---

## 6. Risks & limits (known up front)

- **No TUN/TAP, no kernel netdev in the guest.** Android blocks `/dev/net/tun`
  and there is no real `wlan0` netdev — the interface is *emulated*. L3 for the
  guest (real IP, generic `ping`, arbitrary sockets routed over the chip) needs an
  in‑proot L3 path or per‑app helpers, exactly the gap uKernel hit. Plain
  `ping`/DHCP are solved with dedicated clients there; full transparent L3 for
  *every* guest program is the hard part and should be scoped explicitly at W5.
- **One chip family at a time.** Each adapter needs its full‑MAC vendor driver
  built into the daemon. RTL8811AU/12AU is proven; others (rtl8821cu, mt7601u‑
  fullmac, …) are separate W0‑style imports. mac80211 soft‑MAC chips are out of
  scope (see §2).
- **USB reset flakiness.** uKernel notes occasional chip wedging after many
  reset cycles (physical replug clears it). Our daemon should start with retry and
  treat `USBDEVFS_RESET` as best‑effort (already so in `usbfs_hcd.c`).
- **Legal/scope.** The aircrack‑ng injection path is for *authorised* testing on
  *your own* network only; ship it behind the toggle and document it as such.

---

## 7. Bottom line

The hard, uncertain part — *can an unmodified kernel Wi‑Fi driver be driven from
userspace over our USB fd path?* — is **already answered yes** by uKernel, and
its HCD backend is **already wired to `io.neoterm.usb`**. What remains is
**engineering on a known pattern**: an NDK build (like `ukfs`) and one new proot
control‑plane redirect (like `uknl_usb_redirect.c`). cfg80211 stays a small
custom shim; mac80211 stays a stub; nl80211 becomes daemon‑side message logic fed
by the redirect. No `LD_PRELOAD`, no guest changes, fully behind a `UK_WIFI`
toggle — consistent with every other bridge.
