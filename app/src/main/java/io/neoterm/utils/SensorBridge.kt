package io.neoterm.utils

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.hardware.Sensor
import android.hardware.SensorEvent
import android.hardware.SensorEventListener
import android.hardware.SensorManager
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.BatteryManager
import android.os.FileObserver
import android.os.Handler
import android.os.HandlerThread
import android.os.ParcelFileDescriptor
import android.os.SystemClock
import android.system.Os
import android.system.OsConstants
import io.neoterm.backend.Pty
import io.neoterm.component.config.NeoPreference
import io.neoterm.component.config.NeoTermPath
import io.neoterm.setup.proot.Kmsg
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.Locale
import java.util.concurrent.ConcurrentHashMap
import kotlin.math.roundToInt
import kotlin.math.roundToLong

/**
 * Android-side **sensor + battery bridge** that presents a faithful, kernel-style
 * interface to the proot distro, so the *standard* Linux power/sensor stack works
 * unmodified — no custom client needed:
 *
 *  - **Battery / power** → a fake `/sys/class/power_supply/{BAT0,AC0,USB}` tree.
 *    `upower`, `acpi`, `tlp`-style scripts read it directly.
 *  - **Sensors (polled)** → a fake `/sys/bus/iio/devices/iio:deviceN/` tree per
 *    present sensor (accel/gyro/magn as `_raw` + `_scale`, light/proximity/
 *    pressure/temp/humidity as `_input`). `iio_info`, `cat`, scripts work.
 *  - **Sensors (buffered)** → for accel/gyro/magn/light, the full IIO buffer
 *    interface: `scan_elements/`, `buffer{,0}/enable`, and a `/dev/iio:deviceN`
 *    character device backed by a PTY. When the guest enables the buffer, packed
 *    little-endian s32 scan records are streamed at the sensor rate. `iio_readdev`
 *    and **iio-sensor-proxy** (`monitor-sensor`) work, so e.g. desktop auto-rotate
 *    / adaptive-brightness logic runs on top.
 *
 * The /sys trees are real host dirs bound onto the guest (Android SELinux blocks
 * the real /sys readdir), like the USB-serial `/sys/class/tty` bind. The IIO char
 * device reuses the USB-serial open-redirect: proot asks the `io.neoterm.iio`
 * socket "PATH iio:deviceN" and rewrites the open to the live PTY slave.
 *
 * Updates are event-driven (a [SensorEventListener] + the sticky battery
 * broadcast), throttled per source and de-duplicated, so idle sensors cost no I/O.
 * Gated by [NeoPreference.isSensorsEnabled]; started/stopped with NeoTermService.
 * Only the eight no-permission sensors + battery (heart-rate/step excluded).
 */
object SensorBridge {
  private const val TAG = "Sensor"
  private const val MIN_INTERVAL_MS = 200L
  private const val IIO_SOCKET = "io.neoterm.iio"
  private const val SCAN_TYPE = "le:s32/32>>0"

  private val psDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-power-supply")
  // Bound onto /sys/bus/iio (the PARENT), not just .../devices: libiio's context
  // creation also touches /sys/bus/iio, and the real Android one is SELinux-denied
  // (EACCES) — shadowing the whole subtree keeps libiio from escaping the bind.
  private val iioDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-bus-iio")
  private val iioDevDir = File(iioDir, "devices")
  // libiio's local_create_context() ALSO scans /sys/class/hwmon and
  // /sys/kernel/debug/iio; on Android those resolve to the real /sys (hwmon is
  // SELinux-denied, /sys/kernel/debug is 0700 debugfs) and any scan error aborts
  // the whole context with EACCES. Shadow them with empty dirs so the scans
  // succeed (find nothing) instead of hitting the denied paths.
  private val hwmonDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-class-hwmon")
  private val debugIioDir = File("${NeoTermPath.PROOT_ROOT_PATH}/sys-kernel-debug-iio")
  private val batDir = File(psDir, "BAT0")
  private val acDir = File(psDir, "AC0")
  private val usbDir = File(psDir, "USB")

  private val SUPPORTED = intArrayOf(
    Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE, Sensor.TYPE_MAGNETIC_FIELD,
    Sensor.TYPE_LIGHT, Sensor.TYPE_PROXIMITY, Sensor.TYPE_PRESSURE,
    Sensor.TYPE_AMBIENT_TEMPERATURE, Sensor.TYPE_RELATIVE_HUMIDITY
  )

  // Sensors that also expose the buffered (streaming) IIO interface.
  private val BUFFERED = setOf(
    Sensor.TYPE_ACCELEROMETER, Sensor.TYPE_GYROSCOPE,
    Sensor.TYPE_MAGNETIC_FIELD, Sensor.TYPE_LIGHT
  )

  @Volatile private var started = false
  private var appContext: Context? = null
  private var sensorManager: SensorManager? = null
  private var sensorThread: HandlerThread? = null
  private var sensorHandler: Handler? = null
  private val sensorObjs = ConcurrentHashMap<Int, Sensor>() // type -> Sensor (for rate changes)
  private var batteryReceiver: BroadcastReceiver? = null
  private var iioServer: LocalServerSocket? = null

  private val devDirs = ConcurrentHashMap<Int, File>() // type -> iio:deviceN dir
  private val devIndex = HashMap<Int, Int>()         // type -> N (populated before threads read)
  private val typeByIndex = HashMap<Int, Int>()      // N -> type (populated before threads read)
  private val lastTick = HashMap<Int, Long>()        // type -> last write (throttle)
  private val lastWritten = ConcurrentHashMap<String, String>() // path -> content (de-dup)

  // Buffered-stream state (cross-thread: server / observer / sensor threads).
  private val ptyMaster = ConcurrentHashMap<Int, ParcelFileDescriptor>() // type -> PTY master
  private val ptySlave = ConcurrentHashMap<Int, String>()                // type -> /dev/pts/N
  private val pumping = ConcurrentHashMap<Int, Boolean>()                // type -> streaming?
  private val enabledIdx = ConcurrentHashMap<Int, IntArray>()            // type -> enabled chans
  private val observers = ArrayList<FileObserver>()

  private val listener = object : SensorEventListener {
    override fun onSensorChanged(e: SensorEvent) {
      val type = e.sensor.type
      val dir = devDirs[type] ?: return
      // Buffered stream: emit EVERY sample (un-throttled) so iio_readdev fills fast.
      if (pumping[type] == true) emitScan(type, e.values)
      // Polled sysfs files: throttle to ~5 Hz (plenty for cat / iio_info).
      val now = SystemClock.elapsedRealtime()
      if (now - (lastTick[type] ?: 0L) >= MIN_INTERVAL_MS) {
        lastTick[type] = now
        for ((n, c) in dynamicFiles(type, e.values)) write(dir, n, c)
      }
    }

    override fun onAccuracyChanged(s: Sensor?, accuracy: Int) {}
  }

  /** Host dir → guest path binds for the fake /sys trees (empty when disabled). */
  fun sysfsBinds(): List<Pair<String, String>> {
    if (!NeoPreference.isSensorsEnabled()) return emptyList()
    val out = ArrayList<Pair<String, String>>()
    if (psDir.isDirectory || psDir.mkdirs()) out.add(psDir.absolutePath to "/sys/class/power_supply")
    iioDevDir.mkdirs()
    if (iioDir.isDirectory || iioDir.mkdirs()) out.add(iioDir.absolutePath to "/sys/bus/iio")
    // Empty shadows so libiio's hwmon / debugfs scans succeed instead of EACCES.
    if (hwmonDir.isDirectory || hwmonDir.mkdirs()) out.add(hwmonDir.absolutePath to "/sys/class/hwmon")
    if (debugIioDir.isDirectory || debugIioDir.mkdirs()) out.add(debugIioDir.absolutePath to "/sys/kernel/debug/iio")
    return out
  }

  @Synchronized
  fun start(context: Context) {
    if (started) return
    if (!NeoPreference.isSensorsEnabled()) return
    appContext = context.applicationContext
    started = true
    psDir.mkdirs(); iioDevDir.mkdirs()
    registerBattery()
    registerSensors()
    startIioServer()
    startBufferPoller()
    Kmsg.log("sensors: bridge active — /sys/class/power_supply, /sys/bus/iio/devices")
  }

  @Synchronized
  fun stop() {
    if (!started) return
    started = false
    sensorManager?.let { runCatching { it.unregisterListener(listener) } }
    batteryReceiver?.let { r -> runCatching { appContext?.unregisterReceiver(r) } }
    batteryReceiver = null
    runCatching { sensorThread?.quitSafely() }
    sensorThread = null
    sensorHandler = null
    sensorManager = null
    sensorObjs.clear()
    observers.forEach { runCatching { it.stopWatching() } }
    observers.clear()
    runCatching { iioServer?.close() }
    iioServer = null
    ptyMaster.values.forEach { runCatching { it.close() } }
    ptyMaster.clear(); ptySlave.clear(); pumping.clear(); enabledIdx.clear()
    devDirs.clear()
    devIndex.clear(); typeByIndex.clear()
    lastTick.clear(); lastWritten.clear()
    Kmsg.log("sensors: bridge stopped")
  }

  fun restart(context: Context) { stop(); start(context) }

  // ── sensors (IIO) ────────────────────────────────────────────────────────
  private fun registerSensors() {
    val ctx = appContext ?: return
    val sm = ctx.getSystemService(Context.SENSOR_SERVICE) as? SensorManager ?: return
    sensorManager = sm
    val thread = HandlerThread("sensor-capture").apply { start() }
    sensorThread = thread
    val handler = Handler(thread.looper)
    sensorHandler = handler
    var idx = 0
    for (type in SUPPORTED) {
      val s = sm.getDefaultSensor(type) ?: continue
      sensorObjs[type] = s
      val dir = File(iioDevDir, "iio:device$idx").apply { mkdirs() }
      devDirs[type] = dir
      devIndex[type] = idx
      typeByIndex[idx] = type
      write(dir, "name", staticName(type) + "\n")
      staticScale(type)?.let { (n, c) -> write(dir, n, c + "\n") }
      if (type in BUFFERED) scaffoldBuffer(type, dir, idx)
      runCatching { sm.registerListener(listener, s, SensorManager.SENSOR_DELAY_NORMAL, handler) }
      Kmsg.log("sensors: iio:device$idx <- ${staticName(type)} (${s.name})")
      idx++
    }
  }

  private fun staticName(type: Int): String = when (type) {
    Sensor.TYPE_ACCELEROMETER -> "accel_3d"
    Sensor.TYPE_GYROSCOPE -> "gyro_3d"
    Sensor.TYPE_MAGNETIC_FIELD -> "magn_3d"
    Sensor.TYPE_LIGHT -> "als"
    Sensor.TYPE_PROXIMITY -> "proximity"
    Sensor.TYPE_PRESSURE -> "pressure"
    Sensor.TYPE_AMBIENT_TEMPERATURE -> "ambient_temp"
    Sensor.TYPE_RELATIVE_HUMIDITY -> "humidity"
    else -> "unknown"
  }

  private fun staticScale(type: Int): Pair<String, String>? = when (type) {
    Sensor.TYPE_ACCELEROMETER -> "in_accel_scale" to "0.000001"
    Sensor.TYPE_GYROSCOPE -> "in_anglvel_scale" to "0.000001"
    Sensor.TYPE_MAGNETIC_FIELD -> "in_magn_scale" to "0.000001"
    else -> null
  }

  private fun dynamicFiles(type: Int, v: FloatArray): Map<String, String> = when (type) {
    Sensor.TYPE_ACCELEROMETER -> mapOf(
      "in_accel_x_raw" to rawMicro(v, 0), "in_accel_y_raw" to rawMicro(v, 1), "in_accel_z_raw" to rawMicro(v, 2))
    Sensor.TYPE_GYROSCOPE -> mapOf(
      "in_anglvel_x_raw" to rawMicro(v, 0), "in_anglvel_y_raw" to rawMicro(v, 1), "in_anglvel_z_raw" to rawMicro(v, 2))
    Sensor.TYPE_MAGNETIC_FIELD -> mapOf(
      "in_magn_x_raw" to rawMicro(v, 0), "in_magn_y_raw" to rawMicro(v, 1), "in_magn_z_raw" to rawMicro(v, 2))
    Sensor.TYPE_LIGHT -> mapOf("in_illuminance_input" to f3(v, 0))
    Sensor.TYPE_PROXIMITY -> mapOf("in_proximity_input" to f3(v, 0))
    Sensor.TYPE_PRESSURE -> mapOf("in_pressure_input" to f3(v, 0))
    Sensor.TYPE_AMBIENT_TEMPERATURE -> mapOf("in_temp_input" to f3(v, 0))
    Sensor.TYPE_RELATIVE_HUMIDITY -> mapOf("in_humidityrelative_input" to f3(v, 0))
    else -> emptyMap()
  }

  private fun rawMicro(v: FloatArray, i: Int): String =
    (if (i < v.size) (v[i].toDouble() * 1e6).roundToLong() else 0L).toString() + "\n"

  private fun f3(v: FloatArray, i: Int): String =
    String.format(Locale.ROOT, "%.3f", if (i < v.size) v[i] else 0f) + "\n"

  // ── buffered IIO interface (scan_elements + buffer + /dev/iio:deviceN) ────
  private fun channelNames(type: Int): List<String> = when (type) {
    Sensor.TYPE_ACCELEROMETER -> listOf("in_accel_x", "in_accel_y", "in_accel_z")
    Sensor.TYPE_GYROSCOPE -> listOf("in_anglvel_x", "in_anglvel_y", "in_anglvel_z")
    Sensor.TYPE_MAGNETIC_FIELD -> listOf("in_magn_x", "in_magn_y", "in_magn_z")
    Sensor.TYPE_LIGHT -> listOf("in_illuminance")
    else -> emptyList()
  }

  /** The s32 sample value for one channel (matches the polled _raw / _scale). */
  private fun channelValue(type: Int, ci: Int, v: FloatArray): Int = when (type) {
    Sensor.TYPE_LIGHT -> (if (v.isNotEmpty()) v[0].toDouble() else 0.0).roundToInt()
    else -> (if (ci < v.size) v[ci].toDouble() * 1e6 else 0.0).roundToInt()
  }

  @Suppress("DEPRECATION") // FileObserver(String, …) — broad minSdk compatibility
  private fun scaffoldBuffer(type: Int, dir: File, idx: Int) {
    write(dir, "dev", "247:$idx\n")
    if (type == Sensor.TYPE_LIGHT) write(dir, "in_illuminance_scale", "1.000000\n")
    val se = File(dir, "scan_elements").apply { mkdirs() }
    channelNames(type).forEachIndexed { i, ch ->
      write(se, "${ch}_en", "0\n")
      write(se, "${ch}_index", "$i\n")
      write(se, "${ch}_type", "$SCAN_TYPE\n")
    }
    // libiio v0 uses buffer/, newer uses buffer0/ — provide and watch both.
    for (bn in arrayOf("buffer", "buffer0")) {
      val b = File(dir, bn).apply { mkdirs() }
      write(b, "enable", "0\n"); write(b, "length", "128\n"); write(b, "watermark", "1\n")
      // Watch MODIFY too (not just CLOSE_WRITE): libiio may hold buffer/enable
      // open across the write, so CLOSE_WRITE alone would never fire.
      val obs = object : FileObserver(b.absolutePath, FileObserver.MODIFY or FileObserver.CLOSE_WRITE) {
        override fun onEvent(event: Int, path: String?) {
          if (path == null || path.endsWith("enable")) onBufferToggle(type)
        }
      }
      observers.add(obs)
      runCatching { obs.startWatching() }
    }
  }

  /** inotify/FileObserver doesn't cross the proot boundary reliably, so also poll
   *  the enable files of opened buffered devices and toggle the pump from there. */
  private fun startBufferPoller() {
    Thread({
      while (started) {
        try { Thread.sleep(100) } catch (e: InterruptedException) { break }
        for (type in BUFFERED) if (ptySlave.containsKey(type)) onBufferToggle(type)
      }
    }, "iio-buffer-poll").apply { isDaemon = true; start() }
  }

  /** The guest wrote buffer{,0}/enable — (re)read the enable + enabled channels
   *  and start/stop the scan-record pump for this device. */
  private fun onBufferToggle(type: Int) {
    val dir = devDirs[type] ?: return
    val on = arrayOf("buffer", "buffer0").any {
      runCatching { File(File(dir, it), "enable").readText().trim() }.getOrNull() == "1"
    }
    if (on) {
      val chans = channelNames(type)
      val se = File(dir, "scan_elements")
      val en = ArrayList<Int>()
      chans.forEachIndexed { i, ch ->
        if (runCatching { File(se, "${ch}_en").readText().trim() }.getOrNull() == "1") en.add(i)
      }
      if (en.isEmpty()) en.addAll(chans.indices)   // none flagged → stream all
      enabledIdx[type] = en.toIntArray()
      ensurePty(type)
      if (pumping[type] != true) {
        pumping[type] = true
        setRate(type, SensorManager.SENSOR_DELAY_GAME)   // ~50 Hz so the stream fills fast
        Kmsg.log("sensors: iio:device${devIndex[type]} buffer enabled (${en.size} ch)")
      }
    } else if (pumping[type] == true) {
      pumping[type] = false
      setRate(type, SensorManager.SENSOR_DELAY_NORMAL)   // back to low power
      Kmsg.log("sensors: iio:device${devIndex[type]} buffer disabled")
    }
  }

  /** Re-register a sensor at a new delay (faster while its buffer streams). */
  private fun setRate(type: Int, delay: Int) {
    val sm = sensorManager ?: return
    val s = sensorObjs[type] ?: return
    val h = sensorHandler ?: return
    runCatching { sm.unregisterListener(listener, s); sm.registerListener(listener, s, delay, h) }
  }

  /** One packed little-endian scan record of the enabled channels → PTY master. */
  private fun emitScan(type: Int, v: FloatArray) {
    val pfd = ptyMaster[type] ?: return
    val idxs = enabledIdx[type] ?: return
    if (idxs.isEmpty()) return
    val bb = ByteBuffer.allocate(idxs.size * 4).order(ByteOrder.LITTLE_ENDIAN)
    for (ci in idxs) bb.putInt(channelValue(type, ci, v))
    // Master is non-blocking: a slow/absent reader yields EAGAIN → drop the sample
    // rather than stall the sensor thread.
    runCatching { Os.write(pfd.fileDescriptor, bb.array(), 0, bb.capacity()) }
  }

  @Synchronized
  private fun ensurePty(type: Int): String? {
    ptySlave[type]?.let { return it }
    if (type !in BUFFERED) return null
    val out = IntArray(1)
    val slave = Pty.open(out) ?: return null
    val pfd = ParcelFileDescriptor.adoptFd(out[0])
    runCatching { Os.fcntlInt(pfd.fileDescriptor, OsConstants.F_SETFL, OsConstants.O_NONBLOCK) }
    ptyMaster[type] = pfd
    ptySlave[type] = slave
    return slave
  }

  // Control socket: proot's open-redirect asks "PATH iio:deviceN" → live PTY slave.
  private fun startIioServer() {
    val srv = runCatching { LocalServerSocket(IIO_SOCKET) }.getOrNull() ?: run {
      Kmsg.log("sensors: iio control socket bind failed"); return
    }
    iioServer = srv
    Thread({
      while (true) {
        val c = runCatching { srv.accept() }.getOrNull() ?: break
        runCatching { handleIio(c) }
        runCatching { c.close() }
      }
    }, "iio-control").apply { isDaemon = true; start() }
  }

  private fun handleIio(c: LocalSocket) {
    val line = c.inputStream.bufferedReader().readLine()?.trim() ?: return
    val out = c.outputStream
    fun reply(s: String) { out.write((s + "\n").toByteArray()); out.flush() }
    val parts = line.split(" ")
    if (parts.getOrNull(0) == "PATH") {
      val n = parts.getOrNull(1)?.removePrefix("iio:device")?.toIntOrNull()
      val type = if (n != null) typeByIndex[n] else null
      val pts = if (type != null) ensurePty(type) else null
      reply(pts ?: "NAK")
    } else reply("NAK")
  }

  // ── battery (power_supply) ───────────────────────────────────────────────
  private fun registerBattery() {
    batDir.mkdirs(); acDir.mkdirs(); usbDir.mkdirs()
    val r = object : BroadcastReceiver() {
      override fun onReceive(c: Context, i: Intent) = updateBattery(i)
    }
    batteryReceiver = r
    val sticky = runCatching {
      appContext?.registerReceiver(r, IntentFilter(Intent.ACTION_BATTERY_CHANGED))
    }.getOrNull()
    sticky?.let { updateBattery(it) }
  }

  private fun updateBattery(i: Intent) {
    val level = i.getIntExtra(BatteryManager.EXTRA_LEVEL, -1)
    val scale = i.getIntExtra(BatteryManager.EXTRA_SCALE, -1)
    val pct = if (level >= 0 && scale > 0) level * 100 / scale else -1
    val status = i.getIntExtra(BatteryManager.EXTRA_STATUS, BatteryManager.BATTERY_STATUS_UNKNOWN)
    val health = i.getIntExtra(BatteryManager.EXTRA_HEALTH, BatteryManager.BATTERY_HEALTH_UNKNOWN)
    val plugged = i.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0)
    val voltageMv = i.getIntExtra(BatteryManager.EXTRA_VOLTAGE, -1)
    val tempTenths = i.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, Int.MIN_VALUE)
    val tech = i.getStringExtra(BatteryManager.EXTRA_TECHNOLOGY)?.takeIf { it.isNotBlank() } ?: "Li-ion"
    val present = i.getBooleanExtra(BatteryManager.EXTRA_PRESENT, true)
    val statusStr = statusStr(status)
    val healthStr = healthStr(health)
    val capStr = capLevel(pct, status)

    val bm = appContext?.getSystemService(Context.BATTERY_SERVICE) as? BatteryManager
    val curNow = bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CURRENT_NOW)
      ?.takeIf { it != Int.MIN_VALUE && it != 0 }
    val chargeCounter = bm?.getIntProperty(BatteryManager.BATTERY_PROPERTY_CHARGE_COUNTER)
      ?.takeIf { it != Int.MIN_VALUE && it != 0 }
    // acpi/upower compute the % and rate from charge_now / charge_full, NOT from
    // `capacity`. Android only gives the *current* charge (CHARGE_COUNTER, µAh) and
    // the %; derive a consistent full-charge so charge_now/charge_full == capacity.
    val chargeFull = if (chargeCounter != null && pct > 0) chargeCounter.toLong() * 100 / pct else null

    write(batDir, "type", "Battery\n")
    write(batDir, "present", if (present) "1\n" else "0\n")
    write(batDir, "technology", "$tech\n")
    write(batDir, "status", "$statusStr\n")
    if (pct >= 0) {
      write(batDir, "capacity", "$pct\n")
      write(batDir, "capacity_level", "$capStr\n")
    }
    write(batDir, "health", "$healthStr\n")
    if (voltageMv >= 0) write(batDir, "voltage_now", "${voltageMv * 1000L}\n")   // µV
    if (tempTenths != Int.MIN_VALUE) write(batDir, "temp", "$tempTenths\n")       // tenths °C
    curNow?.let { write(batDir, "current_now", "$it\n") }                          // µA
    chargeCounter?.let {
      write(batDir, "charge_counter", "$it\n")                                     // µAh
      write(batDir, "charge_now", "$it\n")                                         // µAh (= remaining)
    }
    chargeFull?.let {
      write(batDir, "charge_full", "$it\n")                                        // µAh
      write(batDir, "charge_full_design", "$it\n")
    }
    write(batDir, "uevent", buildUevent(statusStr, present, tech, pct, capStr, healthStr,
      voltageMv, tempTenths, curNow, chargeCounter, chargeFull))

    val acOnline = plugged and (BatteryManager.BATTERY_PLUGGED_AC or BatteryManager.BATTERY_PLUGGED_WIRELESS) != 0
    val usbOnline = plugged and BatteryManager.BATTERY_PLUGGED_USB != 0
    write(acDir, "type", "Mains\n"); write(acDir, "online", if (acOnline) "1\n" else "0\n")
    write(usbDir, "type", "USB\n"); write(usbDir, "online", if (usbOnline) "1\n" else "0\n")
  }

  private fun buildUevent(
    status: String, present: Boolean, tech: String, pct: Int, capLevel: String,
    health: String, voltageMv: Int, tempTenths: Int, curNow: Int?, chargeNow: Int?, chargeFull: Long?
  ): String = buildString {
    append("POWER_SUPPLY_NAME=BAT0\n")
    append("POWER_SUPPLY_TYPE=Battery\n")
    append("POWER_SUPPLY_STATUS=$status\n")
    append("POWER_SUPPLY_PRESENT=${if (present) 1 else 0}\n")
    append("POWER_SUPPLY_TECHNOLOGY=$tech\n")
    if (pct >= 0) { append("POWER_SUPPLY_CAPACITY=$pct\n"); append("POWER_SUPPLY_CAPACITY_LEVEL=$capLevel\n") }
    append("POWER_SUPPLY_HEALTH=$health\n")
    if (voltageMv >= 0) append("POWER_SUPPLY_VOLTAGE_NOW=${voltageMv * 1000L}\n")
    if (tempTenths != Int.MIN_VALUE) append("POWER_SUPPLY_TEMP=$tempTenths\n")
    curNow?.let { append("POWER_SUPPLY_CURRENT_NOW=$it\n") }
    chargeNow?.let { append("POWER_SUPPLY_CHARGE_NOW=$it\n") }
    chargeFull?.let { append("POWER_SUPPLY_CHARGE_FULL=$it\n"); append("POWER_SUPPLY_CHARGE_FULL_DESIGN=$it\n") }
  }

  private fun statusStr(s: Int): String = when (s) {
    BatteryManager.BATTERY_STATUS_CHARGING -> "Charging"
    BatteryManager.BATTERY_STATUS_DISCHARGING -> "Discharging"
    BatteryManager.BATTERY_STATUS_FULL -> "Full"
    BatteryManager.BATTERY_STATUS_NOT_CHARGING -> "Not charging"
    else -> "Unknown"
  }

  private fun healthStr(h: Int): String = when (h) {
    BatteryManager.BATTERY_HEALTH_GOOD -> "Good"
    BatteryManager.BATTERY_HEALTH_OVERHEAT -> "Overheat"
    BatteryManager.BATTERY_HEALTH_DEAD -> "Dead"
    BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE -> "Over voltage"
    BatteryManager.BATTERY_HEALTH_COLD -> "Cold"
    BatteryManager.BATTERY_HEALTH_UNSPECIFIED_FAILURE -> "Unspecified failure"
    else -> "Unknown"
  }

  private fun capLevel(pct: Int, status: Int): String = when {
    status == BatteryManager.BATTERY_STATUS_FULL || pct >= 100 -> "Full"
    pct in 0..5 -> "Critical"
    pct in 6..15 -> "Low"
    pct >= 80 -> "High"
    else -> "Normal"
  }

  // ── file writer (de-duplicated) ──────────────────────────────────────────
  private fun write(dir: File, name: String, content: String) {
    val f = File(dir, name)
    val path = f.absolutePath
    if (lastWritten[path] == content) return
    runCatching { f.writeText(content) }.onSuccess { lastWritten[path] = content }
  }
}
