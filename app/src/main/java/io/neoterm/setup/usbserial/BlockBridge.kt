package io.neoterm.setup.usbserial

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbInterface
import android.hardware.usb.UsbManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import io.neoterm.component.config.NeoPreference
import io.neoterm.component.config.NeoTermPath
import io.neoterm.setup.proot.Kmsg
import io.neoterm.utils.NLog
import java.io.File
import java.io.InputStream
import java.io.RandomAccessFile
import java.io.OutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * USB Mass-Storage block-device bridge. Exposes a connected USB pendrive's raw
 * sectors to the proot distro as a block device (/dev/uksd0) via the proot block
 * proxy (enter.c open-redirect + read/write/lseek/ioctl) over the io.neoterm.block
 * abstract socket — so `dd`, `fdisk`, `parted`, `mkfs.*`, `fsck.*`, `mtools` and a
 * userspace FS layer all work on ANY filesystem, with no root.
 *
 * The chip is driven app-side by SCSI Bulk-Only-Transport (CBW/CSW, READ(10)/
 * WRITE(10)/READ_CAPACITY(10)/SYNCHRONIZE CACHE) over the bulk endpoints — the same
 * protocol the kernel usb-storage driver speaks. FS-independent: only sector I/O.
 *
 * Control protocol (io.neoterm.block, newline-framed commands + binary payloads):
 *   OPEN | SIZE          -> "OK <bytes> <sector>\n"
 *   READ <off> <len>     -> "OK <len>\n" + <len> bytes        | "ERR\n"
 *   WRITE <off> <len>    -> (then <len> bytes) -> "OK\n"       | "ERR\n"
 *   FLUSH                -> "OK\n"
 *
 * Gated by [NeoPreference.isUsbStorageEnabled]; events logged to dmesg ([Kmsg]).
 */
object BlockBridge {
  private const val TAG = "UsbBlock"
  private const val SOCKET = "io.neoterm.block"
  private const val CBW_SIG = 0x43425355      // "USBC"
  private const val CSW_SIG = 0x53425355      // "USBS"
  private const val MAX_SECTORS_PER_OP = 256  // cap a single SCSI READ/WRITE (≤128KB @512)

  private var conn: UsbDeviceConnection? = null
  private var iface: UsbInterface? = null
  private var epIn: UsbEndpoint? = null
  private var epOut: UsbEndpoint? = null
  private var deviceName: String? = null
  @Volatile private var sectorSize = 512
  @Volatile private var totalBytes = 0L
  private var tag = 1

  private val marker = File("${NeoTermPath.PROOT_ROOT_PATH}/sysdata/uksd0")
  private var server: LocalServerSocket? = null
  @Volatile private var started = false
  private val io = Any()   // serialises SCSI transactions (single in-flight BOT)

  /** Clear a bulk-endpoint STALL: CLEAR_FEATURE(ENDPOINT_HALT) — UsbDeviceConnection
   *  has no clearHalt(), so issue the control transfer directly. */
  private fun clearHalt(ep: UsbEndpoint) {
    runCatching { conn?.controlTransfer(0x02, 1, 0, ep.address, null, 0, 1000) }
  }

  // ── recognition ───────────────────────────────────────────────────────────
  /** A USB Mass-Storage (SCSI transparent, Bulk-Only-Transport) interface. */
  fun isMassStorage(device: UsbDevice): Boolean = massInterface(device) != null

  private fun massInterface(device: UsbDevice): UsbInterface? {
    for (i in 0 until device.interfaceCount) {
      val it = device.getInterface(i)
      if (it.interfaceClass == UsbConstants.USB_CLASS_MASS_STORAGE &&
        it.interfaceSubclass == 0x06 /* SCSI transparent */ &&
        it.interfaceProtocol == 0x50 /* Bulk-Only Transport */) return it
    }
    return null
  }

  // ── server lifecycle ──────────────────────────────────────────────────────
  @Synchronized
  fun ensureReady() {
    if (started) return
    if (!NeoPreference.isUsbStorageEnabled()) return
    val srv = try { LocalServerSocket(SOCKET) } catch (e: Exception) {
      Kmsg.log("usb-block: control socket bind failed: ${e.message}"); return
    }
    server = srv
    started = true
    Thread({
      Kmsg.log("usb-block: ready — block device /dev/uksd0 (when a pendrive is attached)")
      while (true) {
        val c = try { srv.accept() } catch (e: Exception) { break }
        Thread({ runCatching { serve(c) }; runCatching { c.close() } }, "uksd-conn")
          .apply { isDaemon = true; start() }
      }
    }, "uksd-control").apply { isDaemon = true; start() }
  }

  // ── attach / detach ───────────────────────────────────────────────────────
  @Synchronized
  fun attach(usb: UsbManager, device: UsbDevice) {
    ensureReady()
    if (!NeoPreference.isUsbStorageEnabled()) return
    if (deviceName != null) { Kmsg.log("usb-block: another block device already bound — ignoring ${device.deviceName}"); return }
    val msif = massInterface(device) ?: return
    val id = "%04x:%04x".format(device.vendorId, device.productId)
    if (!usb.hasPermission(device)) { Kmsg.log("usb-block: $id detected — waiting for USB permission"); return }
    val c = runCatching { usb.openDevice(device) }.getOrNull()
      ?: run { Kmsg.log("usb-block: $id openDevice() failed"); return }
    if (!c.claimInterface(msif, true)) { runCatching { c.close() }; Kmsg.log("usb-block: $id claimInterface failed"); return }
    var bin: UsbEndpoint? = null; var bout: UsbEndpoint? = null
    for (e in 0 until msif.endpointCount) {
      val ep = msif.getEndpoint(e)
      if (ep.type == UsbConstants.USB_ENDPOINT_XFER_BULK) {
        if (ep.direction == UsbConstants.USB_DIR_IN) bin = ep else bout = ep
      }
    }
    if (bin == null || bout == null) { runCatching { c.releaseInterface(msif); c.close() }; Kmsg.log("usb-block: $id no bulk endpoints"); return }
    conn = c; iface = msif; epIn = bin; epOut = bout; deviceName = device.deviceName
    // READ_CAPACITY(10) (after a TEST UNIT READY to clear the power-on UNIT ATTENTION).
    runCatching { scsi(byteArrayOf(0,0,0,0,0,0), null, 0) }              // TEST UNIT READY
    val cap = runCatching { readCapacity() }.getOrNull()
    if (cap == null) { teardown("$id: READ CAPACITY failed"); return }
    sectorSize = cap.second; totalBytes = (cap.first + 1L) * cap.second
    // Size the bound /dev/uksd0 marker (sparse) so the guest's native fstat/SEEK_END
    // report the real capacity (the proxy overrides actual read/write/lseek/ioctl).
    runCatching { marker.parentFile?.mkdirs(); RandomAccessFile(marker, "rw").use { it.setLength(totalBytes) } }
    val gb = totalBytes / (1024.0 * 1024 * 1024)
    Kmsg.log("usb-block: /dev/uksd0 <- $id  %.1f GB  (sector %d, %d sectors)".format(gb, sectorSize, cap.first + 1))
    NLog.e(TAG, "attached $id ${totalBytes}B sector=$sectorSize")
    // (Re)start the FS daemon — a prior detach kills it (see onDetached); a fresh
    // ukfsd is needed so the guest can mount this newly attached device. No-op if
    // it is already running.
    runCatching { FsBridge.ensureReady() }
  }

  @Synchronized
  fun onDetached(device: UsbDevice?) {
    if (device != null && device.deviceName == deviceName) {
      teardown("/dev/uksd0: pendrive disconnected")
      // Tear down the FS daemon too: it holds the (now dead) mount, and killing it
      // drops the proot redirect's io.neoterm.fs connection so the guest's next
      // access sees the device is gone and auto-clears the /mnt mount point
      // (otherwise it would keep showing the unplugged disk's stale contents).
      runCatching { FsBridge.stopAll() }
    }
  }

  @Synchronized
  fun stopAll() {
    teardown("/dev/uksd0: released (service stop)")
    runCatching { server?.close() }; server = null; started = false
  }

  private fun teardown(msg: String) {
    runCatching { wbFlush() }   // flush any buffered writes while the device is still up
    iface?.let { i -> runCatching { conn?.releaseInterface(i) } }
    runCatching { conn?.close() }
    conn = null; iface = null; epIn = null; epOut = null; deviceName = null; totalBytes = 0
    runCatching { RandomAccessFile(marker, "rw").use { it.setLength(0) } }   // shrink the marker back
    Kmsg.log("usb-block: $msg")
  }

  // ── SCSI Bulk-Only Transport ──────────────────────────────────────────────
  /** One BOT transaction: CBW → optional data phase → CSW. dataIn != null = read.
   *  Returns the bytes transferred (>=0) or -1 on transport failure. */
  private fun scsi(cdb: ByteArray, data: ByteArray?, dataLen: Int, dataIn: Boolean = false): Int = synchronized(io) {
    val c = conn ?: return -1; val out = epOut ?: return -1; val inn = epIn ?: return -1
    val t = tag++
    val cbw = ByteBuffer.allocate(31).order(ByteOrder.LITTLE_ENDIAN)
    cbw.putInt(CBW_SIG); cbw.putInt(t); cbw.putInt(dataLen)
    cbw.put((if (dataIn) 0x80 else 0x00).toByte())   // flags: dir
    cbw.put(0)                                        // LUN
    cbw.put(cdb.size.toByte())                        // CDB length
    cbw.put(cdb); while (cbw.position() < 31) cbw.put(0)
    if (c.bulkTransfer(out, cbw.array(), 31, 5000) != 31) return -1
    var moved = 0
    if (dataLen > 0 && data != null) {
      val ep = if (dataIn) inn else out
      while (moved < dataLen) {
        val chunk = minOf(dataLen - moved, 16384)
        val tmp = if (dataIn) ByteArray(chunk) else data.copyOfRange(moved, moved + chunk)
        val n = c.bulkTransfer(ep, tmp, chunk, 15000)
        if (n < 0) { clearHalt(ep); break }
        if (dataIn) System.arraycopy(tmp, 0, data, moved, n)
        moved += n
        if (n < chunk) break
      }
    }
    // CSW
    val csw = ByteArray(13)
    var n = c.bulkTransfer(inn, csw, 13, 5000)
    if (n != 13) { clearHalt(inn); n = c.bulkTransfer(inn, csw, 13, 5000) }
    if (n != 13) return -1
    val cb = ByteBuffer.wrap(csw).order(ByteOrder.LITTLE_ENDIAN)
    if (cb.int != CSW_SIG) return -1
    cb.int /* tag */; cb.int /* residue */
    val status = cb.get().toInt() and 0xff
    if (status == 2) return -1                         // phase error → transport fail
    return if (status == 0) moved else -2              // 1 = CHECK CONDITION (caller may retry)
  }

  private fun readCapacity(): Pair<Long, Int>? {
    val buf = ByteArray(8)
    // retry once on CHECK CONDITION (fresh-media UNIT ATTENTION)
    repeat(3) {
      val r = scsi(byteArrayOf(0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0), buf, 8, dataIn = true)
      if (r == 8) {
        val bb = ByteBuffer.wrap(buf).order(ByteOrder.BIG_ENDIAN)
        val lastLba = bb.int.toLong() and 0xffffffffL
        val blk = bb.int
        if (blk in 1..65536) return Pair(lastLba, blk)
      }
      runCatching { scsi(byteArrayOf(0,0,0,0,0,0), null, 0) }    // TEST UNIT READY to clear UA
    }
    return null
  }

  /** SCSI READ(10) of [count] sectors at [lba] into [dst] (at offset 0). */
  private fun read10(lba: Long, count: Int, dst: ByteArray): Boolean {
    val cdb = scsi10(0x28, lba, count)
    repeat(2) { if (scsi(cdb, dst, count * sectorSize, dataIn = true) == count * sectorSize) return true }
    return false
  }

  /** SCSI WRITE(10) of [count] sectors at [lba] from [src]. */
  private fun write10(lba: Long, count: Int, src: ByteArray): Boolean {
    val cdb = scsi10(0x2a, lba, count)
    repeat(2) { if (scsi(cdb, src, count * sectorSize) == count * sectorSize) return true }
    return false
  }

  private fun scsi10(op: Int, lba: Long, count: Int): ByteArray = byteArrayOf(
    op.toByte(), 0,
    (lba ushr 24).toByte(), (lba ushr 16).toByte(), (lba ushr 8).toByte(), lba.toByte(),
    0, (count ushr 8).toByte(), count.toByte(), 0)

  // ── byte-granular access (sector read-modify-write) ───────────────────────
  private fun readAt(off: Long, reqLen: Int): ByteArray? {
    if (off < 0 || reqLen <= 0) return null
    if (off >= totalBytes) return ByteArray(0)            // EOF
    val len = minOf(reqLen.toLong(), totalBytes - off).toInt()   // clamp to device end
    val first = off / sectorSize
    val last = (off + len - 1) / sectorSize
    val out = ByteArray(len); var done = 0
    var lba = first
    while (lba <= last) {
      val count = minOf((last - lba + 1).toInt(), MAX_SECTORS_PER_OP)
      val sec = ByteArray(count * sectorSize)
      if (!read10(lba, count, sec)) return null
      val secStart = lba * sectorSize
      val from = maxOf(off, secStart)
      val to = minOf(off + len, secStart + count.toLong() * sectorSize)
      val n = (to - from).toInt()
      System.arraycopy(sec, (from - secStart).toInt(), out, (from - off).toInt(), n)
      done += n; lba += count
    }
    return if (done == len) out else null
  }

  private fun writeRaw(off: Long, data: ByteArray): Boolean {
    val len = data.size
    if (off < 0 || len <= 0 || off + len > totalBytes) return false
    val first = off / sectorSize
    val last = (off + len - 1) / sectorSize
    var lba = first
    var dpos = 0
    while (lba <= last) {
      val count = minOf((last - lba + 1).toInt(), MAX_SECTORS_PER_OP)
      val secStart = lba * sectorSize
      val secBytes = count * sectorSize
      val from = maxOf(off, secStart)
      val to = minOf(off + len, secStart + secBytes.toLong())
      val n = (to - from).toInt()
      val aligned = (from == secStart) && (n == secBytes)
      val sec: ByteArray
      if (aligned) {
        sec = data.copyOfRange(dpos, dpos + n)
      } else {                                           // read-modify-write the partial sectors
        sec = ByteArray(secBytes)
        if (!read10(lba, count, sec)) return false
        System.arraycopy(data, dpos, sec, (from - secStart).toInt(), n)
      }
      if (!write10(lba, count, sec)) return false
      dpos += n; lba += count
    }
    return true
  }

  // ── write-back coalescing (turns mkfs/dd's many small sequential writes into a
  //    few large SCSI WRITE(10)s; flushed on a gap, a 4MB cap, an overlapping read,
  //    a FLUSH command (guest close/fsync) and detach) ──────────────────────────
  private var wbStart = -1L
  private val wbBuf = java.io.ByteArrayOutputStream()
  private val WB_MAX = 4 * 1024 * 1024

  private fun wbFlush(): Boolean {
    if (wbStart < 0 || wbBuf.size() == 0) { wbStart = -1; wbBuf.reset(); return true }
    val data = wbBuf.toByteArray(); val at = wbStart
    wbBuf.reset(); wbStart = -1
    return writeRaw(at, data)
  }

  private fun writeAt(off: Long, data: ByteArray): Boolean {
    if (off < 0 || data.isEmpty() || off + data.size > totalBytes) return false
    if (wbStart >= 0 && off != wbStart + wbBuf.size()) { if (!wbFlush()) return false }
    if (wbStart < 0) wbStart = off
    wbBuf.write(data)
    return if (wbBuf.size() >= WB_MAX) wbFlush() else true
  }

  /** SCSI read with write-back coherency: flush only if the read overlaps the buffer. */
  private fun readCoherent(off: Long, len: Int): ByteArray? {
    if (wbStart >= 0 && off < wbStart + wbBuf.size() && off + len > wbStart) {
      if (!wbFlush()) return null
    }
    return readAt(off, len)
  }

  // ── control server ────────────────────────────────────────────────────────
  private fun serve(c: LocalSocket) {
    val inp = c.inputStream; val out = c.outputStream
    while (true) {
      val line = readLine(inp) ?: return
      if (line.isEmpty()) continue
      val p = line.split(" ")
      synchronized(this) {
        when (p[0]) {
          "OPEN", "SIZE" -> reply(out, if (deviceName != null) "OK $totalBytes $sectorSize\n" else "ERR\n")
          "READ" -> {
            val off = p.getOrNull(1)?.toLongOrNull(); val len = p.getOrNull(2)?.toIntOrNull()
            val data = if (off != null && len != null) readCoherent(off, len) else null
            if (data == null) reply(out, "ERR\n")
            else { out.write("OK ${data.size}\n".toByteArray()); out.write(data); out.flush() }
          }
          "WRITE" -> {
            val off = p.getOrNull(1)?.toLongOrNull(); val len = p.getOrNull(2)?.toIntOrNull()
            if (off == null || len == null || len <= 0) { reply(out, "ERR\n") }
            else {
              val data = readN(inp, len)
              reply(out, if (data != null && writeAt(off, data)) "OK\n" else "ERR\n")
            }
          }
          "FLUSH" -> { wbFlush(); runCatching { scsi(byteArrayOf(0x35, 0, 0, 0, 0, 0, 0, 0, 0, 0), null, 0) }; reply(out, "OK\n") }
          else -> reply(out, "ERR\n")
        }
      }
    }
  }

  private fun reply(out: OutputStream, s: String) { out.write(s.toByteArray()); out.flush() }

  private fun readLine(inp: InputStream): String? {
    val sb = StringBuilder()
    while (true) {
      val b = inp.read()
      if (b < 0) return if (sb.isEmpty()) null else sb.toString()
      if (b == '\n'.code) return sb.toString()
      sb.append(b.toChar())
      if (sb.length > 256) return sb.toString()
    }
  }

  private fun readN(inp: InputStream, len: Int): ByteArray? {
    val buf = ByteArray(len); var off = 0
    while (off < len) {
      val n = inp.read(buf, off, len - off)
      if (n < 0) return null
      off += n
    }
    return buf
  }
}
