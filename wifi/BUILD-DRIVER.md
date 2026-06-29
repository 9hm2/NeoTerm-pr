# USB Wi-Fi a proot guestben — teljes build, nulláról

Ez a recept a **teljes nullától** elvisz egy üres telefonig, és a végén egy
valódi USB Wi-Fi dongle-ön `scan` → WPA2 → DHCP → `ping` fut a proot Linux
guestből — **root nélkül**, guest-kernelmodul nélkül, `LD_PRELOAD` nélkül. A
drivert **a telefonon, a guesten belül** fordítjuk (nem PC-n, nem NDK-val).

> A keretrendszer chip-agnosztikus, és **driver nélkül** szállít. A saját
> dongle-öd vendor driver forrását te adod hozzá; minden más (kernel-API shim,
> cfg80211, usbfs HCD, nl80211/rtnetlink bridge) már a `libukwifid.so`-ban van.

---

## Hogyan áll össze (1 perces modell)

```
  guest:  modprobe rtl8812au         iw / wpa_supplicant / dhclient
            │ connect()                   │ AF_NETLINK (nl80211) / AF_PACKET
            │ @io.neoterm.wifi            ▼  (proot UK_WIFI redirect)
            ▼  (guest wrapper, direct)    │
  app:    libukwifid.so  ── dlopen $UK_WIFI_MODDIR/rtl8812au.so ──┐
            kernel-API shim + cfg80211 + usbfs HCD                │
            └── io.neoterm.usb (SCM_RIGHTS fd) ── valódi USB dongle┘
```
(A modul-betöltés a wrappertől megy közvetlenül a daemonhoz; az nl80211/AF_PACKET
adatsík továbbra is a proot redirecten át — azokat a syscallokat az Android nem
tiltja, így a proot elkapja.)

- A `build-driver.sh` egy pici guest-oldali **`modprobe`/`lsmod`/`rmmod`/`insmod`
  wrappert** telepít (`/usr/local/sbin`, elöl a PATH-ban), ami **közvetlenül** a
  `@io.neoterm.wifi` daemonnal beszél (`UK_OP_MODPROBE` → `dlopen
  $UK_WIFI_MODDIR/<név>.so`). **Miért nem a sima modprobe?** Androidon az
  app-sandbox seccomp-szűrője a `finit_module`/`init_module`/`delete_module`
  syscallt `ENOSYS`-szal **tiltja**, és ez a tiltás **felülírja** a proot
  trace-kérését (ERRNO ≻ TRACE) — így a modul-syscallt **semmilyen** userspace
  tracer nem tudja elfogni. A guest viszont közös network namespace-ben van a
  daemonnal, így a wrapper egyszerűen `connect()`-el a socketre (engedélyezett
  syscall). A `.ko` ettől még egy minimális, érvényes ELF (depmod/modinfo kompat).
- A driver `.so` a **bionic** daemonban fut, ezért **libc-független** kell
  legyen → `-nostdlib`-bal linkeljük (a részletes „miért” a `build-driver.sh`
  fejlécében).

---

## 0. Telefon-oldal — a keretrendszer bekapcsolása (egyszeri)

1. Telepítsd a NeoTerm APK-t (ezzel a Wi-Fi ággal buildelve), és indítsd el.
2. **Settings → General → USB Wi-Fi (modprobe)** → **ON**.
   Ez:
   - elindítja a `libukwifid.so` daemont az `@io.neoterm.wifi` socketen,
   - beállítja a guest előtt: `UK_WIFI=1` (proot redirect aktív),
     `UK_WIFI_MODDIR=<rootfs>/lib/ukwifi`, `UK_WIFI_FW_DIR=<rootfs>/lib/firmware`,
   - beköti a fake `/sys/class/net`, `/sys/class/ieee80211`, `/proc/modules`-t.
3. Kapcsold be a **USB** togglet is (Settings → USB), és add meg az
   `io.neoterm.usb` hozzáférést a dongle-höz, amikor a rendszer kéri — a Wi-Fi a
   USB bridge-et használja a chip eléréséhez.
4. Dugd be az USB Wi-Fi dongle-t (OTG).

> Ezen a ponton a daemon fut, de **nincs driver** — a `wlan0` még nem létezik.

---

## 1. A proot guest telepítése (ha még nincs)

A NeoTerm telepítőjéből telepíts egy glibc disztrót (pl. **Kali** vagy
**Debian/Ubuntu**). Lépj be a guest shellbe. Innentől minden parancs **a
guesten belül** fut.

```sh
# ellenőrizd, hogy a redirect aktív (a togglenak ON-nak kell lennie):
echo "$UK_WIFI"            # -> 1
echo "$UK_WIFI_MODDIR"     # -> .../lib/ukwifi   (ide kerül a driver .so)
uname -r                   # az Android kernel verziója — ez lesz a .ko útvonalban
```

---

## 2. Build-eszközök telepítése a guestben

```sh
apt update
apt install -y build-essential git
```

- **Kernel-headerre (`linux-headers-*`) NINCS szükség** — a shim helyettesíti.
- **Firmware**: csak ha a chiped külső firmware-t tölt. Az RTL8811AU/8812AU
  beágyazza, nem kell. Ha kell (pl. rtlwifi-s chip):
  `apt install -y firmware-realtek` — a `request_firmware()` a guest
  `/lib/firmware`-ből olvas (`$UK_WIFI_FW_DIR`).

---

## 3. A shim-headerek a guestbe

A driver a uKernel **fake kernel-headerei** ellen fordul (ezek pótolják a
`linux/*`, `net/*` kernel API-t). Két mód:

```sh
# A) klónozd ezt a repót a guestben (a build-script innen veszi a headert):
git clone <ennek-a-repónak-az-URL-je> ~/neoterm
cd ~/neoterm

# B) vagy csak a header-könyvtárat másold be a guestbe és mutass rá:
#    (a host app/src/main/cpp/ukfs/include könyvtár tartalmát)
#    export SHIM_INC=~/ukfs-include
```

Ellenőrzés: léteznie kell a `app/src/main/cpp/ukfs/include/net/cfg80211.h`-nak
(vagy `$SHIM_INC/net/cfg80211.h`-nak).

---

## 4. A chip vendor driver forrása a guestbe

Példa RTL8811AU/8812AU/8821AU-ra (a uKernel projekt ezt bizonyította):

```sh
git clone https://github.com/aircrack-ng/rtl8812au ~/rtl8812au
```

Más chiphez a megfelelő **vendor** driver forrását klónozd (az in-kernel
`rtl8xxxu` ezeket a chipeket nem fedi le). cfg80211-es drivert válassz, ne
wext-eset.

---

## 5. Build (a guestben)

```sh
cd ~/neoterm
wifi/build-driver.sh ~/rtl8812au rtl8812au -DCONFIG_RTL8812A -DCONFIG_RTL8821A
```

A script:
1. minden driver `.c`-t a **fake kernel-headerekkel előre**
   (`-I .../ukfs/include`), `-ffreestanding -fno-stack-protector` fordít;
2. `-shared -nostdlib --unresolved-symbols=ignore-all` linkel →
   libc-független `rtl8812au.so`;
3. telepíti `$UK_WIFI_MODDIR`-be (`/lib/ukwifi/rtl8812au.so`);
4. létrehozza a `.ko` **névhordozót**
   `/lib/modules/$(uname -r)/kernel/drivers/net/wireless/ukwifi/rtl8812au.ko`
   és lefuttat egy `depmod`-ot.

### Chip CONFIG flagek
A vendor fák `CONFIG_*` makrók mögé rejtik a funkciókat (ezeket normál esetben a
Kbuild adja). A **chip-választást** te adod át `-D…`-ként (a parancsban:
`-DCONFIG_RTL8812A -DCONFIG_RTL8821A`).

A többi Realtek-flaget a script **automatikusan** beállítja (a bizonyított
uKernel `rtl8812au.mk`-ból), köztük a **legfontosabbat**:
`-DDM_ODM_SUPPORT_TYPE=0x04` (ODM_CE = Linux). **Enélkül a `hal/phydm` alrendszer
NEM fordul** (`enumerator value … not an integer constant`, `field 'mpt_dig_timer'
has incomplete type`, `ASSOCIATE_ENTRY_NUM undeclared` — ~140 fájl). Továbbá:
`-DCONFIG_LITTLE_ENDIAN -DCONFIG_WIFI_MONITOR -DCONFIG_MONITOR_MODE_XMIT
-DCONFIG_TXPWR_BY_RATE=1 -DDRV_NAME -DEFUSE_MAP_PATH`. Ezt a csoportot a
`$RTW_CONFIG` env-vel írhatod felül (nem-Realtek chipnél: `RTW_CONFIG=""`).
A `-DCONFIG_IOCTL_CFG80211` mindig be van kapcsolva — a keretrendszer cfg80211-et
beszél, nem wext-et.

### Nem-cél fájlok kihagyása
A script **nem fordítja le mind a 209 forrást** — a vendor fa más buszok/
platformok/chipek fájljait is tartalmazza, amiket a Kbuild USB-STA buildnél
sosem fordít (és nálunk hiányzó headereken buknának: `mach/*.h`,
`linux/mmc/sdio_func.h`, `linux/jhash.h`). A proven build ~145 fájlt fordított.
Az alapértelmezett kihagyás (felülírható a `$EXCLUDE` env-vel, kiterjesztett regex):

```
platform/*  ·  *sdio*/*gspi*/*pci*  ·  rhashtable.c  ·  rtw_mp*/rtw_bt_mp/rtw_eeprom/rtw_ioctl_rtl
```

Ha egy chip más feature-`.c`-t igényel: add hozzá a `-DCONFIG_FOO`-t, vagy bővítsd
a `$EXCLUDE`-ot. Mindent fordítani (nem-Realtek driver): `EXCLUDE='' …`.

---

## 6. Betöltés + teszt (a guestből)

```sh
hash -r                       # a shell vegye fel a /usr/local/sbin/modprobe wrappert
modprobe rtl8812au            # a wrapper -> @io.neoterm.wifi daemon (insmod is ezt használja)
lsmod | grep rtl8812au        # a daemontól
ip link                       # wlan0 megjelenik
iw dev                        # phy#0 / wlan0, csatornák

# felhozás + scan
ip link set wlan0 up
iw dev wlan0 scan | grep SSID

# WPA2
cat > /tmp/wpa.conf <<EOF
network={ ssid="YOURSSID" psk="YOURPASS" }
EOF
wpa_supplicant -i wlan0 -c /tmp/wpa.conf -B
dhclient wlan0                # vagy: udhcpc -i wlan0
ping -c3 1.1.1.1

# monitor / injection (opcionális)
iw dev wlan0 set type monitor && ip link set wlan0 up
```

`rmmod rtl8812au` (vagy `modprobe -r`) kiadja (daemon `dlclose` + `module_exit`).

---

## Hibakeresés

| tünet | ok / megoldás |
|---|---|
| `modprobe: nincs modul .so` a `ukwifid.log`-ban | a `.so` nincs `$UK_WIFI_MODDIR`-ben — buildelj újra, vagy ellenőrizd a toggle `UK_WIFI_MODDIR`-jét |
| `dlopen … cannot locate symbol` insmodkor | egy kernel API, amit a driver kér, még nincs a shimben — jegyezd a szimbólum nevét; ez shim-hiány, nem build-flag |
| `nincs firmware: …` | telepítsd a chip firmware-ét a guest `/lib/firmware`-be (pl. `apt install firmware-realtek`) |
| compile hiba egy feature-fájlon | add a megfelelő `-DCONFIG_*`-ot, vagy hagyd ki azt a forrást |
| `wlan0` sosem jelenik meg | a probe nem talált eszközt — `lsusb` lássa a dongle-t, és a **USB** toggle/`io.neoterm.usb` legyen fent |
| `echo $UK_WIFI` üres | a USB Wi-Fi toggle nincs ON, vagy a guestet a bekapcsolás előtt indítottad — indítsd újra a guestet |

Logok: a daemon a `filesDir/ukwifid.log`-ba ír (a probe/modprobe/firmware sorok
magyarul, `ukwifi/…` prefixszel).
