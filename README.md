NeoTerm
=======
![](https://img.shields.io/badge/language-Kotlin-green.svg)
![](https://img.shields.io/badge/license-GPLv3-000000.svg)
![](https://img.shields.io/badge/arch-arm64--v8a-blue.svg)

A modern-designed Android terminal emulator — now a self-contained **Linux
workstation in an app**: a full **proot Linux distro runtime**, an **embedded X11
server**, **native audio (in & out)**, and a suite of **hardware bridges** that
expose the phone's camera, GPS, sensors, USB-serial adapters and USB mass storage
to the distro — all in a single APK, **no root and no companion app**.

### Our Pledge

Originally a front end for Termux, this fork grows NeoTerm into a complete Linux
environment on Android: run a real distro (apt/apk/pacman), launch graphical apps
on a built-in X server, get sound in and out, and reach real device hardware
through standard Linux interfaces — without rooting the device.

---

## Highlights

- **Self-contained Linux distros in proot** — Ubuntu / Kali / Alpine / Arch, no root.
- **Embedded X11 server** (Termux:X11 baked into the APK) for graphical apps.
- **Native audio** — output *and* microphone via a bundled PulseAudio.
- **Camera** → a real `/dev/video0` (V4L2) the distro opens natively
  (OpenCV/ffmpeg/GStreamer/cheese), plus an MJPEG stream for URL-aware apps.
- **GPS** → a built-in **gpsd**-protocol server; `cgps`/`gpspipe` just work.
- **Sensors & battery** → a faithful `/sys` IIO + `power_supply` tree (`iio_info`,
  `upower`, `monitor-sensor`, auto-rotate).
- **USB-serial** → hot-pluggable `/dev/ttyUSB*` (FTDI/CP210x/CH34x/PL2303/CDC-ACM).
- **USB mass storage & disk images** → mount **vfat / exfat / ntfs3 / ext4** with
  partitions and **loop devices**, parsed entirely in userspace.
- **FUSE in proot** → a working **`/dev/fuse`**, so userspace filesystems
  (**sshfs, rclone**) and **AppImages** (e.g. **Raspberry Pi Imager**) run
  unmodified — no root.
- A modern, multi-tab terminal UI (ViewPager2 tabs, swipe + dot indicator,
  configurable cursor, extra-keys, color schemes, fonts).

> **Architecture:** the Linux runtime and hardware bridges require **arm64-v8a
> (aarch64)**. The terminal UI itself runs on Android 8.0+ (minSdk 26).

### How the hardware bridges work (in one paragraph)

Android sandboxes apps and blocks the real `/dev` and `/sys`, so NeoTerm can't
just hand the kernel devices to the distro. Instead each capability runs as a tiny
**app-side server** (holding the relevant Android permission) and is exposed to the
guest through a **standard Linux interface** — a localhost TCP port, a fake `/sys`
tree, or a *virtual device node* that proot's syscall redirect maps to the live
app-side endpoint. The result: unmodified Linux clients (gpsd tools, `iio-sensor-
proxy`, `mount`, `ffmpeg`, …) work as if the hardware were local. Every bridge is
opt-in in **Settings** and only consumes power/permissions while something is
actually using it; notable events are logged to the in-app `dmesg`.

---

## Linux distro runtime (proot)

NeoTerm ships a bundled `proot` and can install and run full Linux root
filesystems entirely in app-private storage:

| Distro               | Package manager |
|----------------------|-----------------|
| Ubuntu 24.04 LTS     | `apt`           |
| Kali Linux (rolling) | `apt`           |
| Alpine Linux 3.20    | `apk`           |
| Arch Linux           | `pacman`        |

- Runs under proot (ptrace-based fake chroot): a fake `root` (uid/gid 0) inside,
  no real namespaces or device root needed.
- Kernel/pseudo filesystems (`/dev`, `/proc`, `/sys`, `/dev/pts`, …) are bound
  into the guest, plus a writable `/dev/shm` (needed by browsers and many apps).
- External storage is mapped to `/sdcard` when available.
- Package operations (update / upgrade / install / search) are exposed through the
  UI and mapped to each distro's native package manager.

## Graphical apps — embedded X11

The X server is **built into NeoTerm** (the Termux:X11 native server runs in its
own process); there is **no separate app to install**.

- Start/stop the X server from NeoTerm; its status is shown in NeoTerm's own
  notification (no second notification).
- Guest sessions automatically export `DISPLAY=:0`, so GUI apps just work.
- A lightweight stack (xterm, openbox, fonts, xkb data) installs with one tap via
  the built-in "X11 environment" setup.
- Sensible defaults are pre-set for desktop apps under proot (browser sandbox
  flags, no xdg-desktop-portal stalls).
- A **D-Bus session bus** is started automatically (proot has no systemd/user
  session to provide one), so apps that need it — notifications, GSettings,
  single-instance handling, Raspberry Pi Imager — no longer fail with
  "No D-Bus session bus available".

## Audio — output and microphone

Audio is provided by a **PulseAudio cross-built for Android**, bundled in the APK.
It runs as the app (no root, no proot) and is reachable from the distro via
`PULSE_SERVER=127.0.0.1:4713`.

- **Output:** a callback-clocked **AAudio sink** (`neoterm`) — smooth, drift-free
  playback for terminal and X11 apps. Audio starts with the app, so even plain CLI
  tools (mpv, `paplay`, …) have sound.
- **Microphone (opt-in):** an **AAudio source** (`neoterm_mic`) captures the device
  mic for recording apps in the distro (`parecord`, browser WebRTC, …).
  - Toggle under **Settings → General → Microphone** (off by default).
  - Enabling requests the `RECORD_AUDIO` permission and loads the source live;
    disabling unloads it — no app restart.
  - Tuned for usable loudness out of the box (VOICE_RECOGNITION preset + default
    software gain); fine-tune live with `pactl set-source-volume`.

## Camera — real `/dev/video0` (V4L2) + MJPEG stream

The phone camera is exposed to the distro **two ways**, both no-root:

**1. Native `/dev/video0` (V4L2).** A proot syscall shim (`uknl_cam_redirect.c`,
gated by `UK_CAM`) turns a bound marker into a real V4L2 capture device and proxies
every V4L2 `ioctl`/`read`/`mmap`/`poll` on it to the app-side `CameraBridge` over the
abstract socket `io.neoterm.camera`. So apps hardcoded to a camera device open it
natively — `cv2.VideoCapture(0)`, `ffmpeg -f v4l2 -i /dev/video0`, GStreamer
`v4l2src`, `cheese`, `guvcview`, `v4l2-ctl`, browsers' `getUserMedia` under X11.

- **Formats:** `MJPG` (JPEG passthrough, zero re-encode) and `YUYV` (raw), at the
  camera's real resolutions (enumerated via `VIDIOC_ENUM_FRAMESIZES`).
- **I/O methods:** `MMAP` (the guest mmaps the marker file; frames are `pwrite()`n
  into it at the buffer offset — no mmap interception), `USERPTR`, and `read()`.
- **Controls** are proxied to Camera2 and show up under `v4l2-ctl --list-ctrls`:
  brightness (→ AE exposure compensation), continuous autofocus, and zoom
  (`CONTROL_ZOOM_RATIO`, Android 11+).
- A fake `/sys/dev/char/81:0/uevent` (`DEVNAME=video0`) lets `v4l2-ctl`/`libv4l`
  classify the node; `NEOTERM_CAMERA_V4L2=/dev/video0` is exported as a hint.

**2. MJPEG-over-HTTP** (unchanged). A tiny HTTP server serves
`multipart/x-mixed-replace` JPEG frames on **`127.0.0.1:4715/video.mjpeg`** —
handy for URL-aware apps; `NEOTERM_CAMERA_URL` points at it.

- **Lazy:** the camera is opened only while a client is connected/streaming, so the
  device isn't held (and the system "camera in use" indicator isn't lit) when idle.
- Toggle under **Settings → General → Camera**.
- *Limitation:* only the proot guest sees `/dev/video0` (not native Android apps);
  `V4L2_MEMORY_DMABUF` is unsupported (apps fall back to `MMAP`).

## GPS — gpsd protocol

NeoTerm speaks the **gpsd client protocol** directly, so the distro's `libgps`
clients work with **no gpsd installed**. It listens on the gpsd default
**`127.0.0.1:2947`** and serves `VERSION` / `DEVICES` / `WATCH` / `TPV` / `SKY` /
`POLL` JSON.

- Position/velocity (`TPV`) is built from Android `Location` (lat/lon/alt, speed,
  track, and the full set of gpsd error estimates epx/epy/epv/eps/epd); the sky
  view (`SKY`) from `GnssStatus` (per-satellite PRN/elevation/azimuth/SNR/used),
  enriched with real PDOP/HDOP/VDOP parsed from the chip's NMEA `GSA` sentence.
- GNSS is powered **lazily**, only while a client is connected.
- Clients just run e.g. `cgps`, `gpspipe`, `gpsmon`, `foxtrotgps` — they default to
  `localhost:2947`. Toggle under **Settings → General → GPS** (needs location
  permission).

## Sensors & battery

A **sensor + battery bridge** presents a faithful, kernel-style interface so the
*standard* Linux power/sensor stack runs unmodified — no custom client:

- **Battery / power** → a fake `/sys/class/power_supply/{BAT0,AC0,USB}` tree;
  `upower`, `acpi`, `tlp`-style scripts read it directly.
- **Sensors (polled)** → a fake `/sys/bus/iio/devices/iio:deviceN/` per present
  sensor (accel/gyro/magn as `_raw` + `_scale`; light/proximity/pressure/temp/
  humidity as `_input`). `iio_info`, `cat`, scripts work.
- **Sensors (buffered)** → the full IIO buffer interface (`scan_elements/`,
  `buffer/enable`, a `/dev/iio:deviceN` char device backed by a PTY) streaming
  packed little-endian s32 records at the sensor rate. `iio_readdev` and
  **iio-sensor-proxy** (`monitor-sensor`) work — so desktop auto-rotate /
  adaptive-brightness runs on top.

The `/sys` trees are real host dirs bound onto the guest (Android SELinux blocks
the real `/sys` readdir). Updates are event-driven and throttled, so idle sensors
cost no I/O. Covers the eight permission-free sensors + battery (heart-rate/step
excluded). Toggle under **Settings → General → Sensors**.

## USB-serial

`/dev/ttyUSB<n>` is a **virtual, hot-pluggable** port: proot's open-redirect maps
an open of `/dev/ttyUSBn` to the live PTY of the attached adapter.

- Chips driven app-side by *usb-serial-for-android*: **FTDI, CP210x, CH34x,
  PL2303, and CDC-ACM**.
- True hotplug: a port exists only while a device is attached, disconnects cleanly
  on unplug (the guest's read returns EOF), and the **same `ttyUSB` number is
  reused on replug**.
- Full terminal control: the guest's termios (baud / data bits / stop / parity) is
  programmed onto the chip, and modem lines (DTR/RTS/CTS/DSR/DCD/RI) + BREAK are
  proxied through proot's ioctl redirect — so `pyserial`, `esptool`, `avrdude`,
  `minicom`, `screen` work, including auto-reset.
- Toggle under **Settings → General → USB serial**.

## USB mass storage & disk images

NeoTerm parses real filesystems **entirely in userspace** — no root, no kernel
mount. A bundled `ukfsd` daemon runs the actual in-tree Linux FS drivers against
the raw device, and proot's VFS-redirect routes the guest's `mount` and every path
syscall under it to that daemon.

- **Filesystems:** **vfat, exfat, ntfs3, ext4** (+jbd2 journal) — read and write,
  with the FS auto-detected at mount.
- **USB pendrive:** insert a drive (with USB storage enabled) and mount it:
  `mount /dev/uksd0 /mnt`. Sectors flow over a USB SCSI bridge in the app.
- **Partitions:** MBR and GPT tables are parsed; each partition is a node
  (`/dev/uksd0p1`, `/dev/uksd0p2`, …) and **several can be mounted at once** — e.g.
  a card's FAT boot and ext4 root simultaneously.
- **Loop devices — mount a downloaded image like on a PC:** the kernel loop ioctls
  are emulated, so `losetup -fP disk.img` + `mount /dev/loop0p2 /mnt`, and
  `mount -o loop[,offset=N] -t ext4 disk.img /mnt`, both work. The loop nodes
  behave as real block devices, so `mkfs.*`, `blkid`, `fdisk`, `dd` and `df`
  operate on them correctly.
- Toggle under **Settings → General → USB storage**. See
  [`docs/USB_STORAGE_MOUNT.md`](docs/USB_STORAGE_MOUNT.md) for the architecture.

## FUSE filesystems & AppImages

A normal Linux `mount.fuse` needs `open("/dev/fuse")` + `mount(2)` as real root —
neither is available under proot. NeoTerm gives the guest a **real, working
`/dev/fuse`** so **unmodified libfuse programs just work**, no root.

There is no kernel FUSE driver to talk to, so NeoTerm **plays the kernel**: the
guest's libfuse daemon (sshfs, squashfuse, …) opens our `/dev/fuse` and runs its
normal session loop, while a userspace FUSE engine inside proot speaks the FUSE
wire protocol to it and serves the mountpoint to the rest of the distro.

- **Userspace filesystems:** **sshfs** and **rclone mount** work — `sshfs
  host:/path /mnt`, then read/write under `/mnt` as usual.
- **AppImages:** type-2 AppImages mount their bundled squashfs via FUSE, so apps
  like the **Raspberry Pi Imager** run straight from the `.AppImage` — including
  its `lsblk`-based drive enumeration over `/dev/uksd0`.
- **Concurrent & re-entrant:** several processes can use one FUSE mount at once
  (e.g. the app plus a `lsblk` it spawns), and **multiple FUSE mounts** coexist.
- **Clean lifecycle:** mounts are torn down when unmounted *or* when the owning
  app exits/is interrupted, so repeated launch → quit → relaunch stays reliable.

Nothing to toggle — it's part of the proot runtime whenever a distro is running.

## Terminal UI

- **Tabs:** multiple sessions rendered in a **ViewPager2** — **swipe left/right to
  switch**, with a thin **dot indicator** under the toolbar showing the active tab.
- **Cursor style:** choose **block / underline / bar** (Settings → General →
  cursor style); apps can still override it at runtime via `DECSCUSR`.
- **Extra-keys row:** customizable Ctrl/Alt/Esc/arrows and more.
- **Volume keys as special keys** (optional): Volume Down → Ctrl, Volume Up → Fn.
- Word- or character-based IME, auto-completion, back-key-as-Escape, full-screen
  mode, color schemes and fonts — all in Settings.

## Settings overview

Most capabilities are opt-in toggles under **Settings → General**:

| Setting        | What it enables                                              |
|----------------|-------------------------------------------------------------|
| Microphone     | AAudio mic source in PulseAudio (`RECORD_AUDIO`)            |
| Camera         | MJPEG camera stream on `127.0.0.1:4715` (`CAMERA`)          |
| GPS            | gpsd-protocol server on `127.0.0.1:2947` (location perm)    |
| Sensors        | `/sys` IIO + `power_supply` trees + `/dev/iio:deviceN`      |
| USB serial     | hot-pluggable `/dev/ttyUSB*`                                |
| USB storage    | `/dev/uksd0[pN]` + loop devices + the userspace FS engine   |
| Cursor style   | block / underline / bar                                     |
| Volume as keys | Volume Down → Ctrl, Volume Up → Fn                          |

## Building

The release APK is produced by CI (`.github/workflows/build-neoterm.yml`), which
also cross-compiles the bundled `proot` binary, the Android PulseAudio (see
`audio/build-pulseaudio.sh`), and the `ukfsd` USB-storage FS engine + FS drivers
(`app/src/main/cpp/ukfs/`, see `ukfs.cmake`). The distro root filesystems and the
proot binary are published as release assets and downloaded on demand by the app.

The USB-storage stack has its own test suites:
`app/src/main/cpp/ukfs/test/run_host_tests.sh` (engine + cross-compile) and
`run_proot_it.sh` (end-to-end through real proot: vfat/exfat/ntfs3/ext4, multi-
partition, and loop devices).

### Help & Documentation

View on [GitBook](https://neoterm.gitbooks.io/neoterm-wiki/content) ·
View on [GitHub](https://github.com/NeoTerm/NeoTerm-Wiki)

### License

GPLv3.
