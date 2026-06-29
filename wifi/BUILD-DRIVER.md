# Building & testing a Wi-Fi driver on-device (inside the proot guest)

This is the end-to-end recipe to take a chip's **vendor kernel driver source**,
build it **inside the proot Linux guest** (no PC, no NDK), and load it with
`insmod`/`modprobe` so `iw`/`wpa_supplicant`/DHCP drive a real USB Wi-Fi dongle —
no root, no guest kernel module, no LD_PRELOAD.

> The framework is chip-agnostic and ships **without any driver**. You supply the
> driver source for *your* dongle; everything else (kernel-API shim, cfg80211,
> usbfs HCD, nl80211/rtnetlink bridge) is already in `libukwifid.so`.

---

## How it fits together (1-minute model)

```
  guest:  insmod rtl8812au.ko        iw / wpa_supplicant / dhclient
            │ finit_module(fd)            │ AF_NETLINK (nl80211) / AF_PACKET
            ▼  (proot UK_WIFI redirect)   ▼  (proot UK_WIFI redirect)
  app:    libukwifid.so  ── dlopen $UK_WIFI_MODDIR/rtl8812au.so ──┐
            kernel-API shim + cfg80211 + usbfs HCD                │
            └── io.neoterm.usb (SCM_RIGHTS fd) ── real USB dongle ┘
```

- `insmod`'s `finit_module` is trapped by the proot redirect; it reads the fd's
  path, extracts the module **name**, and asks the daemon to `dlopen`
  `$UK_WIFI_MODDIR/<name>.so`. **The `.ko` bytes are never used** — it's only a
  name carrier. The real code is the `.so` you build below.
- The driver `.so` runs in the **bionic** daemon, so it must be **libc-agnostic**
  → we link it `-nostdlib`. See the header of `build-driver.sh` for the full why.

---

## Prerequisites (one-time, in the guest)

```sh
# 1) a native compiler
apt update && apt install -y build-essential git

# 2) the uKernel fake-kernel headers (what the driver compiles against).
#    Either clone this repo in the guest …
git clone <this-repo> ~/neoterm && cd ~/neoterm
#    … or, if you only want the headers, copy this one dir into the guest and
#    export SHIM_INC to point at it:
#        export SHIM_INC=~/ukfs-include   # contains net/cfg80211.h, linux/…, …

# 3) your dongle's vendor driver SOURCE (example: RTL8811AU/8812AU/8821AU)
git clone https://github.com/aircrack-ng/rtl8812au ~/rtl8812au
```

You do **not** need kernel headers (`linux-headers-*`) — the shim replaces them.
You do **not** need `firmware-realtek` unless your chip loads external firmware
(RTL8811AU/8812AU embed it). If it does, install it; `request_firmware()` reads
from the guest `/lib/firmware` (`$UK_WIFI_FW_DIR`).

Make sure the **USB Wi-Fi** toggle is ON in NeoTerm Settings (it starts the
daemon and binds the sysfs/`/proc/modules` views and `$UK_WIFI_MODDIR`).

---

## Build

```sh
cd ~/neoterm
wifi/build-driver.sh ~/rtl8812au rtl8812au -DCONFIG_RTL8812A -DCONFIG_RTL8821A
```

What it does:
1. compiles every `.c` in the driver tree with the **fake kernel headers first**
   (`-I app/src/main/cpp/ukfs/include`), `-ffreestanding -fno-stack-protector`;
2. links `-shared -nostdlib --unresolved-symbols=ignore-all` →
   a libc-agnostic `rtl8812au.so`;
3. installs it to `/lib/ukwifi/rtl8812au.so` (`$UK_WIFI_MODDIR`);
4. creates the `.ko` **name carrier** at
   `/lib/modules/$(uname -r)/kernel/drivers/net/wireless/ukwifi/rtl8812au.ko`
   and runs `depmod`.

### Chip CONFIG flags
Vendor trees gate features behind `CONFIG_*` macros that the out-of-tree Kbuild
normally sets. Pass the ones for your chip as `-D…`. Common rtl8812au set:

```
-DCONFIG_RTL8812A -DCONFIG_RTL8821A -DCONFIG_IOCTL_CFG80211 \
-DRTW_USE_CFG80211_STA_EVENT -DCONFIG_CONCURRENT_MODE \
-DCONFIG_WIFI_MONITOR -DCONFIG_MONITOR_MODE_XMIT
```

If a compile fails with “undeclared `CONFIG_FOO`” or a whole feature `.c` erroring
out, either add `-DCONFIG_FOO` or trim that source (point `build-driver.sh` at a
subdir / pre-prune the tree). `-DCONFIG_IOCTL_CFG80211` is **required** — the
framework speaks cfg80211, not wext.

---

## Load & test (from the guest)

```sh
insmod /lib/modules/$(uname -r)/kernel/drivers/net/wireless/ukwifi/rtl8812au.ko
#  …or simply:  modprobe rtl8812au

lsmod | grep rtl8812au           # served from the daemon via /proc/modules
ip link                          # wlan0 should appear
iw dev                           # phy#0 / wlan0, channels

# bring it up + scan
ip link set wlan0 up
iw dev wlan0 scan | grep SSID

# WPA2
cat > /tmp/wpa.conf <<EOF
network={ ssid="YOURSSID" psk="YOURPASS" }
EOF
wpa_supplicant -i wlan0 -c /tmp/wpa.conf -B
dhclient wlan0        # or: udhcpc -i wlan0
ping -c3 1.1.1.1

# monitor / injection (optional)
iw dev wlan0 set type monitor && ip link set wlan0 up
```

`rmmod rtl8812au` (or `modprobe -r`) unloads it (daemon `dlclose` + `module_exit`).

---

## Troubleshooting

| symptom | cause / fix |
|---|---|
| `modprobe: nincs modul .so` in `ukwifid.log` | the `.so` isn't in `$UK_WIFI_MODDIR` — rerun the build, or check the toggle set `UK_WIFI_MODDIR=<rootfs>/lib/ukwifi` |
| `dlopen … cannot locate symbol` at insmod | a kernel API the driver needs isn't in the shim yet — note the symbol; it's a shim gap, not a build flag |
| `nincs firmware: …` | install your chip's firmware into the guest `/lib/firmware` (e.g. `apt install firmware-realtek`) |
| compile error on a feature file | add the matching `-DCONFIG_*`, or drop that source |
| `wlan0` never appears | probe found no device — check `lsusb` sees the dongle and the **USB** toggle/`io.neoterm.usb` is up (Wi-Fi reuses the USB bridge) |

Logs: the daemon writes `filesDir/ukwifid.log` (probe/modprobe/firmware lines are
in Hungarian, prefixed `ukwifi/…`).
