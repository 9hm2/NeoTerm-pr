#!/usr/bin/env python3
# Generate UsbSerialIds.kt — the USB-serial probe table — from the Linux kernel
# drivers/usb/serial/{ftdi_sio,cp210x,ch341,pl2303}.c VID:PID tables, mapped to
# the usb-serial-for-android driver classes. No manifest device_filter is used;
# the table recognises broadcast-attached devices at runtime by VID:PID.
#
# Usage: gen_usb_serial_ids.py <kernel/drivers/usb/serial dir> [out.kt]
import re, sys, glob, os
SRC = sys.argv[1]
OUT = sys.argv[2] if len(sys.argv) > 2 else "UsbSerialIds.kt"
defs={}; refs={}
dp=re.compile(r'#\s*define\s+([A-Za-z_]\w*)\s+(0x[0-9A-Fa-f]+|\d+)\b')
rp=re.compile(r'#\s*define\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*$')
for f in glob.glob(SRC+"/*.h")+glob.glob(SRC+"/*.c"):
    for ln in open(f, errors='replace'):
        m=dp.search(ln)
        if m: v=m.group(2); defs[m.group(1)]=int(v,16) if v.lower().startswith('0x') else int(v)
        else:
            r=rp.search(ln.strip())
            if r: refs[r.group(1)]=r.group(2)
for _ in range(6):
    for k,v in list(refs.items()):
        if k not in defs and v in defs: defs[k]=defs[v]
def res(t):
    t=t.strip()
    if re.fullmatch(r'0x[0-9A-Fa-f]+',t): return int(t,16)
    if re.fullmatch(r'\d+',t): return int(t)
    return defs.get(t)
dev=re.compile(r'USB_DEVICE(?:_INTERFACE_NUMBER|_INTERFACE_CLASS|_INTERFACE_PROTOCOL|_AND_INTERFACE_INFO|_VER)?\s*\(\s*([0-9A-Fa-fx]+|[A-Za-z_]\w*)\s*,\s*([0-9A-Fa-fx]+|[A-Za-z_]\w*)\s*(?:,|\))')
FAM=[('ftdi_sio.c','F'),('cp210x.c','C'),('ch341.c','H'),('pl2303.c','P')]
tbl={}
for cf,ch in FAM:
    p=os.path.join(SRC,cf)
    if not os.path.exists(p): continue
    for m in dev.finditer(open(p,errors='replace').read()):
        vid=res(m.group(1)); pid=res(m.group(2))
        if vid is None or pid is None: continue
        if not(0<vid<=0xffff and 0<=pid<=0xffff): continue
        tbl.setdefault((vid,pid), ch)   # first driver wins
rows=sorted(tbl.items())
packed="".join(f"{v:04x}{p:04x}{c}" for (v,p),c in rows)
kt='''package io.neoterm.setup.usbserial

/**
 * GENERATED — do not edit. Regenerate with tools/gen_usb_serial_ids.py from the
 * Linux kernel drivers/usb/serial/{ftdi_sio,cp210x,ch341,pl2303}.c VID:PID tables.
 *
 * USB-serial probe table: maps a device's VID:PID to the usb-serial-for-android
 * driver that handles its chip. Built from the kernel so virtually every known
 * FTDI/CP210x/CH34x/PL2303 clone is recognised — far beyond the library's own
 * built-in list. No AndroidManifest device_filter (devices.xml) is used; devices
 * are recognised at runtime from the USB-attached broadcast by VID:PID.
 *
 * CDC-ACM is class-based (USB interface class 0x02/0x0a), not VID:PID — match it
 * separately with the library's CdcAcmSerialDriver when this table misses.
 */
object UsbSerialIds {
  /** Driver tag -> usb-serial-for-android driver simple class name. */
  val DRIVERS = mapOf(
    'F' to "FtdiSerialDriver",
    'C' to "Cp21xxSerialDriver",
    'H' to "Ch34xSerialDriver",
    'P' to "ProlificSerialDriver",
  )

  // Packed fixed-width records: 4 hex VID + 4 hex PID + 1 driver tag.
  private const val DATA =
%s

  private val map: HashMap<Int, Char> by lazy {
    val m = HashMap<Int, Char>(DATA.length / 9 + 16)
    var i = 0
    while (i + 9 <= DATA.length) {
      val vid = DATA.substring(i, i + 4).toInt(16)
      val pid = DATA.substring(i + 4, i + 8).toInt(16)
      m[(vid shl 16) or pid] = DATA[i + 8]
      i += 9
    }
    m
  }

  /** usb-serial-for-android driver class name for this VID:PID, or null. */
  fun driverFor(vid: Int, pid: Int): String? =
    map[(vid shl 16) or pid]?.let { DRIVERS[it] }

  /** Number of known VID:PID pairs (kernel-derived). */
  val size: Int get() = map.size
}
'''
# chunk the packed string into <=60k literals concatenated (Kotlin const limit ~64k)
chunks=[packed[i:i+8000] for i in range(0,len(packed),8000)]
lit=" +\n".join('    "%s"'%c for c in chunks)
open(OUT,"w").write(kt % lit)
print(f"wrote {OUT}: {len(rows)} VID:PID pairs "
      f"(F={sum(1 for _,c in rows if c=='F')}, C={sum(1 for _,c in rows if c=='C')}, "
      f"H={sum(1 for _,c in rows if c=='H')}, P={sum(1 for _,c in rows if c=='P')})")
