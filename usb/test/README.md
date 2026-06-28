# Host test harness for the no-patch libusb-under-proot work

Uses **umockdev** to mock a USB device in userspace (no real device needed).

```sh
apt-get install -y libusb-1.0-0-dev umockdev libumockdev-dev
cc umock_enum.c -o umock_enum $(pkg-config --cflags --libs umockdev-1.0) -lusb-1.0
LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libumockdev-preload.so ./umock_enum
```

Expected:
```
syspath=/sys/devices/1-1
libusb_init=0
devices=1
  bus=1 addr=2 1234:5678 class=0
```

This proves stock (udev-built) libusb enumerates a device purely from a readable
sysfs tree (subsystem/uevent/busnum/devnum/speed/descriptors) — i.e. a fake
`/sys/bus/usb` populated from `io.neoterm.usb` makes unmodified libusb list
devices with no patch and no ioctl. See ../DESIGN-no-patch.md.
