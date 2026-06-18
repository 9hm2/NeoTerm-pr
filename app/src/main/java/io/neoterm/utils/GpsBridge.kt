package io.neoterm.utils

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.hardware.GeomagneticField
import android.location.GnssStatus
import android.location.Location
import android.location.LocationListener
import android.location.LocationManager
import android.location.OnNmeaMessageListener
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import androidx.core.content.ContextCompat
import io.neoterm.component.config.NeoPreference
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone
import java.util.concurrent.CopyOnWriteArraySet

/**
 * Android-side GPS bridge that speaks the **gpsd client protocol** directly, so the distro's
 * libgps clients (cgps, gpspipe, gpsmon, foxtrotgps, …) work with NO gpsd installed: it listens
 * on the gpsd default 127.0.0.1:2947 and serves VERSION / DEVICES / WATCH / TPV / SKY / POLL JSON.
 *
 * Position/velocity (TPV) is built from [Location] (lat/lon/alt, speed, track and the full set of
 * gpsd error estimates epx/epy/epv/eps/epd); the sky view (SKY) from [GnssStatus] (per-satellite
 * PRN/elevation/azimuth/SNR/used), enriched with real PDOP/HDOP/VDOP parsed from the chip's NMEA
 * GSA sentence when available. GNSS is powered lazily, only while a client is connected.
 *
 * Started with the app by NeoTermService (proot only). Clients just run e.g. `cgps` — they default
 * to localhost:2947.
 */
object GpsBridge {
  private const val TAG = "GpsBridge"
  private const val PORT = 2947 // gpsd default client port
  private const val DEVICE = "neoterm-gps"

  @Volatile private var running = false
  private var serverThread: Thread? = null
  private var serverSocket: ServerSocket? = null
  private var appContext: Context? = null

  private val clients = CopyOnWriteArraySet<Client>()

  private val locationLock = Any()
  private var locationThread: HandlerThread? = null
  private var locationHandler: Handler? = null
  private var locationStarted = false
  private var nmeaListener: OnNmeaMessageListener? = null
  private var locationListener: LocationListener? = null
  private var gnssCallback: GnssStatus.Callback? = null

  @Volatile private var latestLocation: Location? = null
  @Volatile private var latestGnss: GnssStatus? = null
  /** Vertical speed (m/s) derived from successive altitudes, lightly smoothed. */
  @Volatile private var latestClimb = Double.NaN
  @Volatile private var prevAlt = 0.0
  @Volatile private var prevAltMillis = 0L
  /** [pdop, hdop, vdop] parsed from the latest NMEA GSA sentence, or null. */
  @Volatile private var dop: DoubleArray? = null

  private val isoFmt = SimpleDateFormat("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'", Locale.US)
    .apply { timeZone = TimeZone.getTimeZone("UTC") }

  private class Client(val socket: Socket, val out: OutputStream) {
    @Volatile var watching = false
  }

  fun start(context: Context) {
    if (running) return
    if (!NeoPreference.isGpsEnabled()) return
    if (ContextCompat.checkSelfPermission(context, Manifest.permission.ACCESS_FINE_LOCATION)
      != PackageManager.PERMISSION_GRANTED
    ) return
    running = true
    appContext = context.applicationContext
    serverThread = Thread({ serverLoop() }, "gpsd-bridge").apply { isDaemon = true; start() }
  }

  fun stop() {
    running = false
    runCatching { serverSocket?.close() }
    serverSocket = null
    serverThread = null
    for (c in clients) runCatching { c.socket.close() }
    clients.clear()
    stopLocation()
  }

  fun restart(context: Context) {
    stop()
    start(context)
  }

  private fun serverLoop() {
    try {
      ServerSocket(PORT, 8, InetAddress.getByName("127.0.0.1")).use { server ->
        serverSocket = server
        while (running) {
          val client = try {
            server.accept()
          } catch (e: Exception) {
            if (running) Log.w(TAG, "accept failed", e)
            break
          }
          Thread({ handleClient(client) }, "gpsd-client").apply { isDaemon = true }.start()
        }
      }
    } catch (e: Exception) {
      // Most likely the port is taken (a real gpsd is already running). The bridge can't bind.
      Log.w(TAG, "gpsd server stopped (port $PORT in use?)", e)
    } finally {
      serverSocket = null
    }
  }

  private fun handleClient(socket: Socket) {
    val out = try { socket.getOutputStream() } catch (e: Exception) { return }
    val client = Client(socket, out)
    clients.add(client)
    if (clients.size == 1) startLocation()
    try {
      runCatching { socket.tcpNoDelay = true }
      // gpsd announces itself on connect.
      send(client, versionJson())
      send(client, devicesJson())
      val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
      while (running && !socket.isClosed) {
        val line = reader.readLine() ?: break
        // A line may carry several ';'-separated commands.
        for (raw in line.split(";")) handleCommand(client, raw.trim())
      }
    } catch (e: Exception) {
      // Client went away — normal.
    } finally {
      clients.remove(client)
      runCatching { socket.close() }
      if (clients.isEmpty()) stopLocation()
    }
  }

  private fun handleCommand(client: Client, cmd: String) {
    if (cmd.isEmpty()) return
    when {
      cmd.startsWith("?WATCH") -> {
        val enable = !cmd.contains("\"enable\":false")
        client.watching = enable
        send(client, watchJson(enable))
        send(client, devicesJson())
        if (enable) {
          latestLocation?.let { send(client, tpvJson(it)) }
          send(client, skyJson(latestGnss))
        }
      }
      cmd.startsWith("?POLL") -> send(client, pollJson())
      cmd.startsWith("?VERSION") -> send(client, versionJson())
      cmd.startsWith("?DEVICES") -> send(client, devicesJson())
    }
  }

  private fun send(client: Client, obj: JSONObject) {
    try {
      val bytes = (obj.toString() + "\r\n").toByteArray()
      synchronized(client.out) { client.out.write(bytes); client.out.flush() }
    } catch (e: Exception) {
      clients.remove(client)
      runCatching { client.socket.close() }
    }
  }

  private fun broadcast(obj: JSONObject) {
    for (c in clients) if (c.watching) send(c, obj)
  }

  // ---- Location / NMEA sources ----

  @android.annotation.SuppressLint("MissingPermission") // guarded by checkSelfPermission in start()
  private fun startLocation() {
    synchronized(locationLock) {
      if (locationStarted) return
      val ctx = appContext ?: return
      if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.ACCESS_FINE_LOCATION)
        != PackageManager.PERMISSION_GRANTED
      ) return
      try {
        val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager
        val thread = HandlerThread("gps-capture").apply { start() }
        val handler = Handler(thread.looper)
        locationThread = thread
        locationHandler = handler

        val listener = LocationListener { loc ->
          // Derive vertical speed (climb) from the altitude change since the last fix; Android
          // doesn't expose it directly. Lightly EMA-smoothed because GPS altitude is jittery.
          if (loc.hasAltitude() && prevAltMillis > 0L) {
            val dt = (loc.time - prevAltMillis) / 1000.0
            if (dt in 0.05..10.0) {
              val raw = (loc.altitude - prevAlt) / dt
              latestClimb = if (latestClimb.isNaN()) raw else 0.5 * latestClimb + 0.5 * raw
            }
          }
          if (loc.hasAltitude()) { prevAlt = loc.altitude; prevAltMillis = loc.time }
          latestLocation = loc
          broadcast(tpvJson(loc))
        }
        locationListener = listener

        val gnss = object : GnssStatus.Callback() {
          override fun onSatelliteStatusChanged(status: GnssStatus) {
            latestGnss = status
            broadcast(skyJson(status))
          }
        }
        gnssCallback = gnss
        lm.registerGnssStatusCallback(gnss, handler)

        // Raw NMEA only to mine real PDOP/HDOP/VDOP out of the GSA sentence.
        val nmea = OnNmeaMessageListener { message, _ -> if (message != null) parseGsaDop(message) }
        nmeaListener = nmea
        lm.addNmeaListener(nmea, handler)

        if (lm.allProviders.contains(LocationManager.GPS_PROVIDER)) {
          lm.requestLocationUpdates(LocationManager.GPS_PROVIDER, 1000L, 0f, listener, thread.looper)
        }
        locationStarted = true
      } catch (e: Exception) {
        Log.w(TAG, "startLocation failed", e)
        stopLocation()
      }
    }
  }

  private fun stopLocation() {
    synchronized(locationLock) {
      val ctx = appContext
      if (ctx != null) {
        runCatching {
          val lm = ctx.getSystemService(Context.LOCATION_SERVICE) as LocationManager
          nmeaListener?.let { lm.removeNmeaListener(it) }
          gnssCallback?.let { lm.unregisterGnssStatusCallback(it) }
          locationListener?.let { lm.removeUpdates(it) }
        }
      }
      nmeaListener = null
      gnssCallback = null
      locationListener = null
      runCatching { locationThread?.quitSafely() }
      locationThread = null
      locationHandler = null
      locationStarted = false
      latestLocation = null
      latestGnss = null
      dop = null
      latestClimb = Double.NaN
      prevAlt = 0.0
      prevAltMillis = 0L
    }
  }

  private fun parseGsaDop(sentence: String) {
    try {
      val body = sentence.substringAfter('$', "").substringBefore('*')
      val t = body.split(",")
      if (t.isNotEmpty() && t[0].endsWith("GSA") && t.size >= 18) {
        val p = t[15].toDoubleOrNull()
        val h = t[16].toDoubleOrNull()
        val v = t[17].toDoubleOrNull()
        if (p != null && h != null && v != null) dop = doubleArrayOf(p, h, v)
      }
    } catch (e: Exception) {
      // ignore malformed
    }
  }

  // ---- gpsd JSON reports ----

  private fun iso(t: Long): String = synchronized(isoFmt) {
    isoFmt.format(Date(if (t > 0) t else System.currentTimeMillis()))
  }

  private fun JSONObject.putNum(key: String, v: Double) {
    if (!v.isNaN() && !v.isInfinite()) put(key, v)
  }

  private fun versionJson(): JSONObject = JSONObject().apply {
    // libgps clients (cgps, …) compare these to the gpsd they were built against and print a
    // cosmetic warning on mismatch (functionality is unaffected — the JSON wire format is the
    // same). Track the current stable gpsd so the common distro clients don't warn; bump if a
    // newer gpsd changes its release/protocol numbers.
    put("class", "VERSION")
    put("release", "3.27.5")
    put("rev", "neoterm")
    put("proto_major", 16)
    put("proto_minor", 1)
  }

  private fun deviceJson(): JSONObject = JSONObject().apply {
    put("class", "DEVICE")
    put("path", DEVICE)
    put("driver", "NMEA0183")
    put("activated", iso(0))
    put("flags", 1)
    put("native", 0)
    put("cycle", 1.0)
  }

  private fun devicesJson(): JSONObject = JSONObject().apply {
    put("class", "DEVICES")
    put("devices", JSONArray().put(deviceJson()))
  }

  private fun watchJson(enable: Boolean): JSONObject = JSONObject().apply {
    put("class", "WATCH")
    put("enable", enable)
    put("json", true)
    put("nmea", false)
    put("raw", 0)
    put("scaled", false)
    put("timing", false)
    put("split24", false)
    put("pps", false)
    put("device", DEVICE)
  }

  private fun tpvJson(loc: Location): JSONObject = JSONObject().apply {
    put("class", "TPV")
    put("device", DEVICE)
    put("mode", if (loc.hasAltitude()) 3 else 2)
    put("time", iso(loc.time))
    put("ept", 0.005)
    putNum("lat", loc.latitude)
    putNum("lon", loc.longitude)
    if (loc.hasAltitude()) {
      putNum("altHAE", loc.altitude)
      putNum("alt", loc.altitude)
    }
    // Mean-sea-level altitude is only provided by Android on API 33+.
    if (android.os.Build.VERSION.SDK_INT >= 33 && loc.hasMslAltitude()) {
      putNum("altMSL", loc.mslAltitudeMeters)
    }
    if (loc.hasAccuracy()) {
      val acc = loc.accuracy.toDouble()
      putNum("eph", acc); putNum("epx", acc); putNum("epy", acc)
      // Spherical (3D) error: horizontal combined with vertical.
      val epv = if (loc.hasVerticalAccuracy()) loc.verticalAccuracyMeters.toDouble() else 0.0
      putNum("sep", Math.sqrt(acc * acc + epv * epv))
    }
    if (loc.hasVerticalAccuracy()) putNum("epv", loc.verticalAccuracyMeters.toDouble())
    if (loc.hasSpeed()) putNum("speed", loc.speed.toDouble())
    if (loc.hasSpeedAccuracy()) putNum("eps", loc.speedAccuracyMetersPerSecond.toDouble())
    if (loc.hasBearing()) {
      putNum("track", loc.bearing.toDouble())
      // Magnetic track/variation from the geomagnetic model (gpsd's magvar is West-positive).
      val decl = GeomagneticField(
        loc.latitude.toFloat(), loc.longitude.toFloat(),
        (if (loc.hasAltitude()) loc.altitude else 0.0).toFloat(),
        if (loc.time > 0) loc.time else System.currentTimeMillis()
      ).declination.toDouble()
      putNum("magvar", -decl)
      putNum("magtrack", ((loc.bearing.toDouble() - decl) % 360 + 360) % 360)
    }
    if (loc.hasBearingAccuracy()) putNum("epd", loc.bearingAccuracyDegrees.toDouble())
    val climb = latestClimb
    if (!climb.isNaN()) putNum("climb", climb)
    // ECEF position from lat/lon/alt (gpsd would otherwise derive this from the receiver).
    val ecef = geodeticToEcef(loc.latitude, loc.longitude, if (loc.hasAltitude()) loc.altitude else 0.0)
    putNum("ecefx", ecef[0]); putNum("ecefy", ecef[1]); putNum("ecefz", ecef[2])
    // ECEF velocity: build the local ENU velocity (horizontal from speed/track, vertical from
    // climb) and rotate it into ECEF.
    if (loc.hasSpeed() && loc.hasBearing()) {
      val sp = loc.speed.toDouble()
      val tr = Math.toRadians(loc.bearing.toDouble())
      val vE = sp * Math.sin(tr)
      val vN = sp * Math.cos(tr)
      val vU = if (!climb.isNaN()) climb else 0.0
      val v = enuVelToEcef(loc.latitude, loc.longitude, vE, vN, vU)
      putNum("ecefvx", v[0]); putNum("ecefvy", v[1]); putNum("ecefvz", v[2])
    }
  }

  private fun skyJson(gnss: GnssStatus?): JSONObject = JSONObject().apply {
    put("class", "SKY")
    put("device", DEVICE)
    latestLocation?.let { put("time", iso(it.time)) }
    val arr = JSONArray()
    var used = 0
    val usedEl = ArrayList<Double>()
    val usedAz = ArrayList<Double>()
    if (gnss != null) {
      // Android reports one entry per satellite *signal*, so a sat tracked on multiple bands
      // (e.g. L1+L5) appears 2-3 times. Collapse to one entry per physical satellite
      // (constellation+svid), keeping the strongest signal and "used" if any signal was used —
      // otherwise the Seen/Used counts are inflated and the sky list shows duplicates.
      val merged = LinkedHashMap<Long, IntArray>() // key -> [gnssid, svid, used(0/1)]
      val elOf = LinkedHashMap<Long, Float>()
      val azOf = LinkedHashMap<Long, Float>()
      val ssOf = LinkedHashMap<Long, Float>()
      for (i in 0 until gnss.satelliteCount) {
        val c = gnss.getConstellationType(i)
        val svid = gnss.getSvid(i)
        val key = (c.toLong() shl 32) or (svid.toLong() and 0xffffffffL)
        val ss = gnss.getCn0DbHz(i)
        val u = if (gnss.usedInFix(i)) 1 else 0
        val cur = merged[key]
        if (cur == null) {
          merged[key] = intArrayOf(gnssIdFor(c), svid, u)
          elOf[key] = gnss.getElevationDegrees(i)
          azOf[key] = gnss.getAzimuthDegrees(i)
          ssOf[key] = ss
        } else {
          if (u == 1) cur[2] = 1
          if (ss > (ssOf[key] ?: 0f)) ssOf[key] = ss
        }
      }
      for ((key, v) in merged) {
        val el = (elOf[key] ?: 0f).toDouble()
        val az = (azOf[key] ?: 0f).toDouble()
        if (v[2] == 1) { used++; usedEl.add(el); usedAz.add(az) }
        arr.put(JSONObject().apply {
          put("PRN", v[1])
          put("svid", v[1])
          put("gnssid", v[0])
          putNum("el", el)
          putNum("az", az)
          putNum("ss", (ssOf[key] ?: 0f).toDouble())
          put("used", v[2] == 1)
        })
      }
    }

    // DOPs the way gpsd computes them: from the used satellites' geometry. Falls back to the
    // PDOP/HDOP/VDOP parsed from the chip's NMEA GSA when there aren't enough sats to solve.
    // Order: [gdop, pdop, hdop, vdop, tdop, xdop, ydop].
    val geo = computeDops(usedEl, usedAz)
    val d = geo ?: dop?.let {
      doubleArrayOf(Double.NaN, it[0], it[1], it[2], Double.NaN, Double.NaN, Double.NaN)
    }
    if (d != null) {
      putNum("gdop", d[0]); putNum("pdop", d[1]); putNum("hdop", d[2]); putNum("vdop", d[3])
      putNum("tdop", d[4]); putNum("xdop", d[5]); putNum("ydop", d[6])
    }

    put("nSat", arr.length())
    put("uSat", used)
    put("satellites", arr)
  }

  private fun pollJson(): JSONObject = JSONObject().apply {
    put("class", "POLL")
    put("time", iso(0))
    val loc = latestLocation
    put("active", if (loc != null) 1 else 0)
    put("tpv", JSONArray().apply { loc?.let { put(tpvJson(it)) } })
    put("sky", JSONArray().put(skyJson(latestGnss)))
  }

  /**
   * Dilution-of-precision values from the geometry of the satellites used in the fix (the same
   * computation gpsd does in fill_dop): build the line-of-sight matrix A (rows of
   * [cos el·sin az, cos el·cos az, sin el, 1]), invert AᵀA and read the diagonal.
   * Returns [gdop, pdop, hdop, vdop, tdop, xdop, ydop], or null if it can't be solved.
   */
  private fun computeDops(elDeg: List<Double>, azDeg: List<Double>): DoubleArray? {
    val n = elDeg.size
    if (n < 4) return null
    val h = Array(4) { DoubleArray(4) }
    for (i in 0 until n) {
      val el = Math.toRadians(elDeg[i])
      val az = Math.toRadians(azDeg[i])
      val ce = Math.cos(el)
      val row = doubleArrayOf(ce * Math.sin(az), ce * Math.cos(az), Math.sin(el), 1.0)
      for (r in 0 until 4) for (c in 0 until 4) h[r][c] += row[r] * row[c]
    }
    val q = invert4(h) ?: return null
    fun s(x: Double) = if (x > 0) Math.sqrt(x) else Double.NaN
    val xdop = s(q[0][0]); val ydop = s(q[1][1]); val vdop = s(q[2][2]); val tdop = s(q[3][3])
    val hdop = s(q[0][0] + q[1][1])
    val pdop = s(q[0][0] + q[1][1] + q[2][2])
    val gdop = s(q[0][0] + q[1][1] + q[2][2] + q[3][3])
    return doubleArrayOf(gdop, pdop, hdop, vdop, tdop, xdop, ydop)
  }

  /** Invert a 4×4 matrix via Gauss-Jordan with partial pivoting; null if (near-)singular. */
  private fun invert4(m: Array<DoubleArray>): Array<DoubleArray>? {
    val n = 4
    val a = Array(n) { i -> DoubleArray(2 * n) { j -> if (j < n) m[i][j] else if (j - n == i) 1.0 else 0.0 } }
    for (col in 0 until n) {
      var piv = col
      for (r in col + 1 until n) if (Math.abs(a[r][col]) > Math.abs(a[piv][col])) piv = r
      if (Math.abs(a[piv][col]) < 1e-12) return null
      val tmp = a[col]; a[col] = a[piv]; a[piv] = tmp
      val pv = a[col][col]
      for (j in 0 until 2 * n) a[col][j] /= pv
      for (r in 0 until n) if (r != col) {
        val f = a[r][col]
        if (f != 0.0) for (j in 0 until 2 * n) a[r][j] -= f * a[col][j]
      }
    }
    return Array(n) { i -> DoubleArray(n) { j -> a[i][j + n] } }
  }

  /** WGS84 geodetic (deg, deg, metres HAE) to ECEF metres. */
  private fun geodeticToEcef(latDeg: Double, lonDeg: Double, h: Double): DoubleArray {
    val a = 6378137.0
    val f = 1.0 / 298.257223563
    val e2 = f * (2 - f)
    val lat = Math.toRadians(latDeg)
    val lon = Math.toRadians(lonDeg)
    val sinLat = Math.sin(lat)
    val cosLat = Math.cos(lat)
    val nN = a / Math.sqrt(1 - e2 * sinLat * sinLat)
    return doubleArrayOf(
      (nN + h) * cosLat * Math.cos(lon),
      (nN + h) * cosLat * Math.sin(lon),
      (nN * (1 - e2) + h) * sinLat
    )
  }

  /** Rotate a local ENU (east, north, up) velocity into ECEF at the given geodetic position. */
  private fun enuVelToEcef(latDeg: Double, lonDeg: Double, vE: Double, vN: Double, vU: Double): DoubleArray {
    val lat = Math.toRadians(latDeg)
    val lon = Math.toRadians(lonDeg)
    val sLat = Math.sin(lat); val cLat = Math.cos(lat)
    val sLon = Math.sin(lon); val cLon = Math.cos(lon)
    return doubleArrayOf(
      -sLon * vE - sLat * cLon * vN + cLat * cLon * vU,
      cLon * vE - sLat * sLon * vN + cLat * sLon * vU,
      cLat * vN + sLat * vU
    )
  }

  /** Map an Android constellation to the gpsd/u-blox gnssid. */
  private fun gnssIdFor(constellation: Int): Int = when (constellation) {
    GnssStatus.CONSTELLATION_GPS -> 0
    GnssStatus.CONSTELLATION_SBAS -> 1
    GnssStatus.CONSTELLATION_GALILEO -> 2
    GnssStatus.CONSTELLATION_BEIDOU -> 3
    GnssStatus.CONSTELLATION_QZSS -> 5
    GnssStatus.CONSTELLATION_GLONASS -> 6
    GnssStatus.CONSTELLATION_IRNSS -> 7
    else -> 0
  }
}
