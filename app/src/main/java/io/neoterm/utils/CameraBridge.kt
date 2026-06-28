package io.neoterm.utils

import android.Manifest
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.graphics.Rect
import android.graphics.YuvImage
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraManager
import android.hardware.camera2.CaptureRequest
import android.media.Image
import android.media.ImageReader
import android.net.LocalServerSocket
import android.net.LocalSocket
import android.os.Build
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import android.util.Range
import android.util.Rational
import androidx.core.content.ContextCompat
import io.neoterm.component.config.NeoPreference
import java.io.ByteArrayOutputStream
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.util.concurrent.atomic.AtomicInteger

/**
 * Android-side camera bridge: exposes the device camera to the distro as an MJPEG stream, the
 * same way [PulseAudioBridge] exposes audio.
 *
 * A tiny HTTP server (app uid, holding the CAMERA permission) serves
 * `multipart/x-mixed-replace` JPEG frames on 127.0.0.1:[PORT]. Distro apps that accept a URL
 * (ffmpeg, OpenCV's `VideoCapture`, mpv, GStreamer, browsers under X11, …) read it via
 * `NEOTERM_CAMERA_URL=http://127.0.0.1:4715/video.mjpeg` (exported in ProotManager when the
 * camera is enabled in Settings).
 *
 * The camera is opened lazily — only while at least one client is connected — so it isn't held
 * (and the system camera-in-use indicator isn't lit) when nothing is reading it.
 *
 * Note: this is a stream bridge, not a `/dev/video0` device. Apps hardcoded to a V4L2 node won't
 * see it; that would need an LD_PRELOAD V4L2 shim or root (v4l2loopback).
 *
 * Started with the app by NeoTermService (proot only; chroot has real /dev access).
 */
object CameraBridge {
  private const val TAG = "CameraBridge"
  private const val PORT = 4715
  private const val BOUNDARY = "neotermframe"
  /** Target capture size; the closest supported YUV size is chosen. */
  private const val TARGET_W = 1280
  private const val TARGET_H = 720
  private const val JPEG_QUALITY = 80

  @Volatile private var running = false
  private var serverThread: Thread? = null
  private var serverSocket: ServerSocket? = null
  private var appContext: Context? = null

  // Camera state (guarded by `cameraLock`).
  private val cameraLock = Any()
  private var cameraThread: HandlerThread? = null
  private var cameraHandler: Handler? = null
  private var cameraDevice: CameraDevice? = null
  private var captureSession: CameraCaptureSession? = null
  private var imageReader: ImageReader? = null

  private val clientCount = AtomicInteger(0)
  private val frameLock = Object()
  @Volatile private var latestJpeg: ByteArray? = null
  /** Latest frame as an upright NV21 buffer (+ dims) for the V4L2 YUYV path. */
  @Volatile private var latestNv21: ByteArray? = null
  @Volatile private var nv21W = 0
  @Volatile private var nv21H = 0
  /** Latest frame as a RAW (pre-rotation, landscape) NV21 buffer — the default
   *  V4L2 path delivers this so /dev/video0 behaves like a landscape USB webcam. */
  @Volatile private var rawNv21: ByteArray? = null
  @Volatile private var rawW = 0
  @Volatile private var rawH = 0
  @Volatile private var frameSeq = 0
  /** Camera sensor mounting orientation in degrees; frames are rotated by this to be upright. */
  @Volatile private var sensorOrientation = 0

  // ── V4L2 bridge (io.neoterm.camera) — exposes a real /dev/video0 to proot ──
  private const val V4L2_SOCKET = "io.neoterm.camera"
  private var v4l2Thread: Thread? = null
  private var v4l2Server: LocalServerSocket? = null
  /** Requested capture size from a V4L2 START (0 = use the Settings default). */
  @Volatile private var reqW = 0
  @Volatile private var reqH = 0
  // Camera2-backed control values, applied to the repeating request.
  @Volatile private var ctrlEv = 0          // exposure compensation (steps)
  @Volatile private var ctrlAf = 1          // continuous autofocus on/off
  @Volatile private var ctrlZoom = 100      // zoom, percent of min (100 = 1x)
  @Volatile private var openW = 0           // currently-open capture size (pre-rotation)
  @Volatile private var openH = 0
  @Volatile private var v4lFourcc = "MJPG"  // negotiated V4L2 pixel format
  @Volatile private var camRotation = 0     // sensor mount rotation, cached for caps/size mapping
  @Volatile private var v4lStreaming = false // a V4L2 client is actively streaming
  private val httpClients = AtomicInteger(0) // MJPEG-over-HTTP consumers (need rotated JPEG)
  @Volatile private var ctrlWb = 1          // white balance (Camera2 AWB mode; 1 = auto)
  @Volatile private var ctrlAntibanding = 3 // anti-banding (0 off, 1 50Hz, 2 60Hz, 3 auto)
  @Volatile private var ctrlFacing = 0      // selected camera: 0 = back, 1 = front
  @Volatile private var ctrlFocusAbs = 0    // manual focus 0..100 (% near), used when AF off
  @Volatile private var ctrlExpAuto = 0     // 0 = auto exposure, 1 = manual
  @Volatile private var ctrlExpAbs = 0      // manual exposure (V4L2 100µs units), used when manual
  @Volatile private var ctrlIso = 0         // manual ISO/gain (sensor sensitivity), used when manual

  // V4L2 control ids (real V4L2 CIDs so generic control UIs recognise them).
  private const val CID_BRIGHTNESS = 0x00980900   // -> AE exposure compensation
  private const val CID_GAIN       = 0x00980913   // -> SENSOR_SENSITIVITY (ISO), manual
  private const val CID_POWER_LINE = 0x00980918   // V4L2_CID_POWER_LINE_FREQUENCY (menu, anti-banding)
  private const val CID_WHITE_BALANCE = 0x0098091C // V4L2_CID_AUTO_N_PRESET_WHITE_BALANCE (menu)
  private const val CID_EXPOSURE_AUTO = 0x009A0901 // V4L2_CID_EXPOSURE_AUTO (menu: auto/manual)
  private const val CID_EXPOSURE_ABS  = 0x009A0902 // V4L2_CID_EXPOSURE_ABSOLUTE (100µs units)
  private const val CID_FOCUS_ABS  = 0x009A090A   // V4L2_CID_FOCUS_ABSOLUTE (manual focus)
  private const val CID_FOCUS_AUTO = 0x009A090C   // V4L2_CID_FOCUS_AUTO (continuous AF on/off)
  private const val CID_ZOOM_ABS   = 0x009A090D   // V4L2_CID_ZOOM_ABSOLUTE -> zoom ratio (API 30+)
  private const val CID_CAMERA_SEL = 0x009A0950   // private: select back/front camera (menu)

  fun start(context: Context) {
    if (running) return
    if (!NeoPreference.isCameraEnabled()) return
    // The HTTP server can start without the permission, but it can't open the camera until it's
    // granted; mirror the mic and just don't start until enabled. The activity restarts us after
    // a grant.
    if (ContextCompat.checkSelfPermission(context, Manifest.permission.CAMERA)
      != PackageManager.PERMISSION_GRANTED
    ) return

    running = true
    io.neoterm.setup.proot.Kmsg.log("camera: MJPEG server starting on 127.0.0.1:$PORT/video.mjpeg")
    appContext = context.applicationContext
    serverThread = Thread({ serverLoop() }, "camera-bridge").apply { isDaemon = true; start() }
    // V4L2 bridge: distro apps open /dev/video0 natively (the proot shim proxies
    // its ioctls/read/mmap here over io.neoterm.camera).
    v4l2Thread = Thread({ v4l2ServerLoop() }, "camera-v4l2").apply { isDaemon = true; start() }
  }

  fun stop() {
    val was = running
    running = false
    runCatching { serverSocket?.close() }
    serverSocket = null
    serverThread = null
    runCatching { v4l2Server?.close() }
    v4l2Server = null
    v4l2Thread = null
    closeCamera()
    if (was) io.neoterm.setup.proot.Kmsg.log("camera: MJPEG server stopped")
  }

  /** Restart after the CAMERA permission is granted or the toggle changes. */
  fun restart(context: Context) {
    stop()
    start(context)
  }

  private fun serverLoop() {
    try {
      ServerSocket(PORT, 4, InetAddress.getByName("127.0.0.1")).use { server ->
        serverSocket = server
        while (running) {
          val client = try {
            server.accept()
          } catch (e: Exception) {
            if (running) Log.w(TAG, "accept failed", e)
            break
          }
          Thread({ handleClient(client) }, "camera-client").apply { isDaemon = true }.start()
        }
      }
    } catch (e: Exception) {
      Log.w(TAG, "camera server stopped", e)
    } finally {
      serverSocket = null
    }
  }

  private fun handleClient(socket: Socket) {
    val first = clientCount.incrementAndGet() == 1
    httpClients.incrementAndGet()
    if (first) openCamera()
    try {
      runCatching { socket.tcpNoDelay = true } // push frames out immediately (low latency)
      socket.use { s ->
        // Drain the request line(s); the path is ignored (we always serve the stream).
        s.getInputStream().let { input ->
          val buf = ByteArray(1024)
          // Best effort: read what's immediately available, don't block forever.
          s.soTimeout = 2000
          runCatching { input.read(buf) }
        }
        val out = s.getOutputStream()
        out.write(
          ("HTTP/1.0 200 OK\r\n" +
            "Connection: close\r\n" +
            "Cache-Control: no-cache, private\r\n" +
            "Pragma: no-cache\r\n" +
            "Content-Type: multipart/x-mixed-replace; boundary=$BOUNDARY\r\n\r\n").toByteArray()
        )
        out.flush()

        var lastSent: ByteArray? = null
        while (running && !s.isClosed) {
          val frame = latestJpeg
          if (frame != null && frame !== lastSent) {
            out.write(
              ("--$BOUNDARY\r\nContent-Type: image/jpeg\r\nContent-Length: ${frame.size}\r\n\r\n")
                .toByteArray()
            )
            out.write(frame)
            out.write("\r\n".toByteArray())
            out.flush()
            lastSent = frame
          } else {
            synchronized(frameLock) { frameLock.wait(500) }
          }
        }
      }
    } catch (e: Exception) {
      // Client went away — normal.
    } finally {
      httpClients.decrementAndGet()
      if (clientCount.decrementAndGet() == 0) closeCamera()
    }
  }

  // ---- Camera2 ----

  @android.annotation.SuppressLint("MissingPermission") // guarded by checkSelfPermission above
  private fun openCamera() {
    synchronized(cameraLock) {
      if (cameraDevice != null) return
      val ctx = appContext ?: return
      if (ContextCompat.checkSelfPermission(ctx, Manifest.permission.CAMERA)
        != PackageManager.PERMISSION_GRANTED
      ) return
      try {
        val cm = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
        val camId = pickCamera(cm) ?: return
        sensorOrientation = cm.getCameraCharacteristics(camId)
          .get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0
        val size = chooseSize(cm, camId)
        openW = size.first; openH = size.second

        val thread = HandlerThread("camera-capture").apply { start() }
        val handler = Handler(thread.looper)
        cameraThread = thread
        cameraHandler = handler

        val reader = ImageReader.newInstance(size.first, size.second, ImageFormat.YUV_420_888, 2)
        reader.setOnImageAvailableListener({ r ->
          val img = try { r.acquireLatestImage() } catch (e: Exception) { null } ?: return@setOnImageAvailableListener
          try {
            processImage(img)
          } catch (e: Exception) {
            // Skip a bad frame.
          } finally {
            img.close()
          }
        }, handler)
        imageReader = reader

        cm.openCamera(camId, object : CameraDevice.StateCallback() {
          override fun onOpened(device: CameraDevice) {
            synchronized(cameraLock) { cameraDevice = device }
            startSession(device, reader)
          }

          override fun onDisconnected(device: CameraDevice) {
            device.close()
            synchronized(cameraLock) { if (cameraDevice === device) cameraDevice = null }
          }

          override fun onError(device: CameraDevice, error: Int) {
            Log.w(TAG, "camera error $error")
            device.close()
            synchronized(cameraLock) { if (cameraDevice === device) cameraDevice = null }
          }
        }, handler)
      } catch (e: Exception) {
        Log.w(TAG, "openCamera failed", e)
        closeCamera()
      }
    }
  }

  @Suppress("DEPRECATION")
  private fun startSession(device: CameraDevice, reader: ImageReader) {
    try {
      val surface = reader.surface
      device.createCaptureSession(listOf(surface), object : CameraCaptureSession.StateCallback() {
        override fun onConfigured(session: CameraCaptureSession) {
          synchronized(cameraLock) {
            if (cameraDevice !== device) { session.close(); return }
            captureSession = session
          }
          try {
            val req = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
            req.addTarget(surface)
            applyControls(req)
            session.setRepeatingRequest(req.build(), null, cameraHandler)
          } catch (e: Exception) {
            Log.w(TAG, "setRepeatingRequest failed", e)
          }
        }

        override fun onConfigureFailed(session: CameraCaptureSession) {
          Log.w(TAG, "capture session config failed")
        }
      }, cameraHandler)
    } catch (e: Exception) {
      Log.w(TAG, "startSession failed", e)
    }
  }

  private fun closeCamera() {
    synchronized(cameraLock) {
      runCatching { captureSession?.close() }
      captureSession = null
      runCatching { cameraDevice?.close() }
      cameraDevice = null
      runCatching { imageReader?.close() }
      imageReader = null
      runCatching { cameraThread?.quitSafely() }
      cameraThread = null
      cameraHandler = null
      latestJpeg = null
    }
  }

  private fun pickCamera(cm: CameraManager): String? {
    return try {
      val ids = cm.cameraIdList
      val want = if (ctrlFacing == 1) CameraCharacteristics.LENS_FACING_FRONT
                 else CameraCharacteristics.LENS_FACING_BACK
      ids.firstOrNull { cm.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == want }
        ?: ids.firstOrNull { cm.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) == CameraCharacteristics.LENS_FACING_BACK }
        ?: ids.firstOrNull()
    } catch (e: Exception) {
      null
    }
  }

  /** Which camera facings exist → menu for camera selection (0 back, 1 front). */
  private fun availableFacings(): Map<Int, String> {
    val ctx = appContext ?: return mapOf(0 to "Back")
    return runCatching {
      val cm = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
      val out = LinkedHashMap<Int, String>()
      var back = false; var front = false
      for (id in cm.cameraIdList) {
        when (cm.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING)) {
          CameraCharacteristics.LENS_FACING_BACK -> back = true
          CameraCharacteristics.LENS_FACING_FRONT -> front = true
        }
      }
      if (back) out[0] = "Back"
      if (front) out[1] = "Front"
      if (out.isEmpty()) out[0] = "Back"
      out
    }.getOrDefault(mapOf(0 to "Back"))
  }

  private fun hasManualSensor(): Boolean {
    val caps = chars()?.get(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES) ?: return false
    return caps.contains(CameraCharacteristics.REQUEST_AVAILABLE_CAPABILITIES_MANUAL_SENSOR)
  }
  /** Minimum focus distance in diopters (0 = fixed-focus lens, no manual focus). */
  private fun minFocusDist(): Float =
    chars()?.get(CameraCharacteristics.LENS_INFO_MINIMUM_FOCUS_DISTANCE) ?: 0f
  /** Manual exposure range in V4L2 100µs units, capped to 1/5 s. */
  private fun expRange100us(): Pair<Int, Int> {
    val r = chars()?.get(CameraCharacteristics.SENSOR_INFO_EXPOSURE_TIME_RANGE) ?: return 1 to 1000
    val lo = (r.lower / 100_000L).coerceAtLeast(1L)
    val hi = (minOf(r.upper, 200_000_000L) / 100_000L).coerceAtLeast(lo + 1)
    return lo.toInt() to hi.toInt()
  }
  private fun isoRange(): Range<Int> =
    chars()?.get(CameraCharacteristics.SENSOR_INFO_SENSITIVITY_RANGE) ?: Range(100, 800)

  private fun chooseSize(cm: CameraManager, camId: String): Pair<Int, Int> {
    // A V4L2 client's requested size wins over the Settings default.
    val (tw, th) = if (reqW >= 2 && reqH >= 2) reqW to reqH else requestedSize()
    return try {
      val map = cm.getCameraCharacteristics(camId)
        .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
      val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)
      if (sizes.isNullOrEmpty()) {
        tw to th
      } else {
        // The size whose pixel count is closest to the requested one (devices rarely support
        // every exact size, so snap to the nearest supported).
        val target = tw.toLong() * th
        val best = sizes.minByOrNull { Math.abs(it.width.toLong() * it.height - target) }!!
        best.width to best.height
      }
    } catch (e: Exception) {
      tw to th
    }
  }

  /** The user-requested capture size ("WxH" from Settings), defaulting to the target box. */
  private fun requestedSize(): Pair<Int, Int> {
    return try {
      val parts = NeoPreference.getCameraResolution().split("x")
      val w = parts[0].trim().toInt()
      val h = parts[1].trim().toInt()
      if (w >= 2 && h >= 2) w to h else TARGET_W to TARGET_H
    } catch (e: Exception) {
      TARGET_W to TARGET_H
    }
  }

  /** Per-frame processing. Only does the work a current consumer needs: the raw
   *  (landscape) NV21 is always kept (cheap), but the full-frame rotation and the
   *  JPEG encode are skipped unless an HTTP client or an upright/MJPG V4L2 client
   *  is actually consuming them — so a YUYV-landscape grab (the low-latency path)
   *  pays neither. */
  private fun processImage(image: Image) {
    val raw = yuv420ToNv21(image)
    val rw = image.width; val rh = image.height
    val landscape = NeoPreference.isCameraV4l2Landscape()
    val http = httpClients.get() > 0
    val wantRot = http || (v4lStreaming && !landscape)
    val wantJpg = http || (v4lStreaming && v4lFourcc == "MJPG" && !landscape)
    var rot: ByteArray? = null; var rwd = rw; var rhd = rh
    if (wantRot) {
      val r = ((sensorOrientation % 360) + 360) % 360
      if (r != 0) { rot = rotateNv21(raw, rw, rh, r); if (r == 90 || r == 270) { rwd = rh; rhd = rw } }
      else rot = raw
    }
    val jpg = if (wantJpg && rot != null) nv21ToJpeg(rot, rwd, rhd) else null
    synchronized(frameLock) {
      rawNv21 = raw; rawW = rw; rawH = rh
      if (rot != null) { latestNv21 = rot; nv21W = rwd; nv21H = rhd }
      if (jpg != null) latestJpeg = jpg
      frameSeq++
      frameLock.notifyAll()
    }
  }

  /** Rotate an NV21 buffer by 90/180/270° clockwise. Returns a new buffer; for 90/270 the
   *  caller must swap width/height. */
  private fun rotateNv21(input: ByteArray, w: Int, h: Int, rotation: Int): ByteArray {
    val output = ByteArray(input.size)
    val ySize = w * h
    val cw = w / 2
    val ch = h / 2
    when (rotation) {
      90 -> {
        for (j in 0 until h) {
          val rowBase = j * w
          for (i in 0 until w) output[i * h + (h - 1 - j)] = input[rowBase + i]
        }
        for (cj in 0 until ch) for (ci in 0 until cw) {
          val src = ySize + cj * w + ci * 2
          val dst = ySize + ci * h + (ch - 1 - cj) * 2
          output[dst] = input[src]; output[dst + 1] = input[src + 1]
        }
      }
      270 -> {
        for (j in 0 until h) {
          val rowBase = j * w
          for (i in 0 until w) output[(w - 1 - i) * h + j] = input[rowBase + i]
        }
        for (cj in 0 until ch) for (ci in 0 until cw) {
          val src = ySize + cj * w + ci * 2
          val dst = ySize + (cw - 1 - ci) * h + cj * 2
          output[dst] = input[src]; output[dst + 1] = input[src + 1]
        }
      }
      180 -> {
        for (j in 0 until h) {
          val rowBase = j * w
          for (i in 0 until w) output[(h - 1 - j) * w + (w - 1 - i)] = input[rowBase + i]
        }
        for (cj in 0 until ch) for (ci in 0 until cw) {
          val src = ySize + cj * w + ci * 2
          val dst = ySize + (ch - 1 - cj) * w + (cw - 1 - ci) * 2
          output[dst] = input[src]; output[dst + 1] = input[src + 1]
        }
      }
      else -> return input
    }
    return output
  }

  /** Convert a YUV_420_888 [Image] to a packed NV21 byte array (handles row/pixel strides). */
  private fun yuv420ToNv21(image: Image): ByteArray {
    val width = image.width
    val height = image.height
    val ySize = width * height
    val nv21 = ByteArray(ySize + ySize / 2)

    val yPlane = image.planes[0]
    val uPlane = image.planes[1]
    val vPlane = image.planes[2]

    // --- Y plane ---
    val yBuffer = yPlane.buffer
    val yRowStride = yPlane.rowStride
    val yPixelStride = yPlane.pixelStride
    var pos = 0
    if (yPixelStride == 1 && yRowStride == width) {
      yBuffer.get(nv21, 0, ySize)
      pos = ySize
    } else {
      val row = ByteArray(yRowStride)
      for (r in 0 until height) {
        yBuffer.position(r * yRowStride)
        val len = minOf(yRowStride, row.size)
        yBuffer.get(row, 0, len)
        for (c in 0 until width) nv21[pos++] = row[c * yPixelStride]
      }
    }

    // --- interleaved VU (NV21) from the U and V planes ---
    val uBuffer = uPlane.buffer
    val vBuffer = vPlane.buffer
    val uRowStride = uPlane.rowStride
    val uPixelStride = uPlane.pixelStride
    val vRowStride = vPlane.rowStride
    val vPixelStride = vPlane.pixelStride
    val chromaHeight = height / 2
    val chromaWidth = width / 2
    var uvPos = ySize
    for (r in 0 until chromaHeight) {
      for (c in 0 until chromaWidth) {
        nv21[uvPos++] = vBuffer.get(r * vRowStride + c * vPixelStride) // V
        nv21[uvPos++] = uBuffer.get(r * uRowStride + c * uPixelStride) // U
      }
    }
    return nv21
  }

  // ──────────────────────── V4L2 bridge (io.neoterm.camera) ────────────────────────
  // The proot shim (uknl_cam_redirect.c, gated by UK_CAM) turns /dev/video0 into a real
  // V4L2 capture node and proxies its ioctls/read/mmap to this server. Protocol:
  //   CAPS                -> OK <nfmt> then per fmt "<FOURCC> <nsizes>" + "<w> <h> <fpsN> <fpsD>"
  //   START <FOURCC> <w> <h> -> OK | ERR
  //   FRAME [timeout_ms]  -> OK <len> + <len> bytes   (current frame, negotiated format)
  //   STOP                -> OK
  //   CTRL_LIST           -> OK <n> then "<id> <type> <min> <max> <step> <def> <flags> <name>"
  //   CTRL_MENU <id>      -> OK <n> then "<index> <name>"
  //   CTRL_GET <id>       -> OK <value> | ERR ;  CTRL_SET <id> <val> -> OK | ERR

  private fun v4l2ServerLoop() {
    try {
      LocalServerSocket(V4L2_SOCKET).use { server ->
        v4l2Server = server
        io.neoterm.setup.proot.Kmsg.log("camera: V4L2 bridge listening on @$V4L2_SOCKET (/dev/video0)")
        while (running) {
          val client = try { server.accept() } catch (e: Exception) { if (running) Log.w(TAG, "v4l2 accept failed", e); break }
          Thread({ handleV4l2Client(client) }, "camera-v4l2-client").apply { isDaemon = true }.start()
        }
      }
    } catch (e: Exception) {
      if (running) Log.w(TAG, "v4l2 server stopped", e)
    } finally {
      v4l2Server = null
    }
  }

  private fun handleV4l2Client(socket: LocalSocket) {
    var started = false
    try {
      val input = socket.inputStream
      val out = socket.outputStream
      val buf = StringBuilder()
      val rd = ByteArray(4096)
      fun readLine(): String? {
        while (true) {
          val nl = buf.indexOf("\n")
          if (nl >= 0) { val l = buf.substring(0, nl); buf.delete(0, nl + 1); return l }
          val n = input.read(rd)
          if (n < 0) return null
          buf.append(String(rd, 0, n, Charsets.US_ASCII))
        }
      }
      while (true) {
        val line = readLine() ?: break
        val p = line.trim().split(" ").filter { it.isNotEmpty() }
        if (p.isEmpty()) continue
        when (p[0]) {
          "CAPS" -> out.write(capsReply().toByteArray())
          "START" -> {
            val ok = if (p.size >= 4) {
              v4lFourcc = p[1]
              if (!started) { started = true; clientCount.incrementAndGet() }
              v4lStreaming = true
              ensureCaptureFor(p[2].toIntOrNull() ?: 640, p[3].toIntOrNull() ?: 480)
              true
            } else false
            out.write((if (ok) "OK\n" else "ERR\n").toByteArray())
          }
          "STOP" -> {
            v4lStreaming = false
            if (started) { started = false; if (clientCount.decrementAndGet() == 0) closeCamera() }
            out.write("OK\n".toByteArray())
          }
          "FRAME" -> writeFrame(out, p.getOrNull(1)?.toIntOrNull() ?: 1000)
          "CTRL_LIST" -> out.write(ctrlListReply().toByteArray())
          "CTRL_MENU" -> out.write(ctrlMenuReply(p.getOrNull(1)?.toLongOrNull()?.toInt() ?: -1).toByteArray())
          "CTRL_GET" -> {
            val v = ctrlGet(p.getOrNull(1)?.toLongOrNull()?.toInt() ?: -1)
            out.write((if (v != null) "OK $v\n" else "ERR\n").toByteArray())
          }
          "CTRL_SET" -> {
            val ok = ctrlSet(p.getOrNull(1)?.toLongOrNull()?.toInt() ?: -1, p.getOrNull(2)?.toIntOrNull() ?: 0)
            out.write((if (ok) "OK\n" else "ERR\n").toByteArray())
          }
          else -> out.write("ERR\n".toByteArray())
        }
        out.flush()
      }
    } catch (e: Exception) {
      // client gone — normal
    } finally {
      v4lStreaming = false
      if (started && clientCount.decrementAndGet() == 0) closeCamera()
      runCatching { socket.close() }
    }
  }

  /** Pick the device camera's characteristics (back-preferred), or null. */
  private fun chars(): CameraCharacteristics? {
    val ctx = appContext ?: return null
    return runCatching {
      val cm = ctx.getSystemService(Context.CAMERA_SERVICE) as CameraManager
      val id = pickCamera(cm) ?: return null
      cm.getCameraCharacteristics(id)
    }.getOrNull()
  }

  /** Post-rotation output sizes (what the guest will actually receive). Capped to
   *  ≤1080p: full-sensor YUV (e.g. 4080×3060) can't stream as a 30 fps preview and
   *  makes the capture session fail — so we never offer/capture it. */
  private fun outputSizes(): List<Pair<Int, Int>> {
    val ch = chars() ?: return listOf(1280 to 720, 640 to 480)
    camRotation = ((ch.get(CameraCharacteristics.SENSOR_ORIENTATION) ?: 0) % 360 + 360) % 360
    val map = ch.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP)
    val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888) ?: return listOf(1280 to 720, 640 to 480)
    // Landscape (default) keeps the native sensor orientation; upright swaps for 90/270.
    val swap = !NeoPreference.isCameraV4l2Landscape() && (camRotation == 90 || camRotation == 270)
    // Match common LANDSCAPE sizes against the sensor's native (landscape) sizes;
    // 1280×720 first so it's the sane default. Report each post-rotation.
    val wanted = listOf(1280 to 720, 640 to 480, 1920 to 1080, 320 to 240)
    val raw = sizes.map { it.width to it.height }.toSet()
    val picked = wanted.filter { raw.contains(it) }.toMutableList()
    if (picked.isEmpty()) {
      // Nearest available size to 1280×720 that streams (≤1920 wide).
      val target = 1280L * 720
      val best = sizes.filter { it.width <= 1920 && it.height <= 1920 }
        .minByOrNull { Math.abs(it.width.toLong() * it.height - target) }
        ?: sizes.minByOrNull { it.width.toLong() * it.height }!!
      picked.add(best.width to best.height)
    }
    return picked.map { if (swap) it.second to it.first else it }
  }

  private fun capsReply(): String {
    val sizes = outputSizes()
    val sb = StringBuilder("OK 2\n")
    for (fcc in listOf("MJPG", "YUYV")) {
      sb.append("$fcc ${sizes.size}\n")
      for ((w, h) in sizes) sb.append("$w $h 30 1\n")
    }
    return sb.toString()
  }

  /** Ensure the camera is capturing at the post-rotation size (w,h); reopen if it changed. */
  private fun ensureCaptureFor(w: Int, h: Int) {
    chars()?.get(CameraCharacteristics.SENSOR_ORIENTATION)?.let { camRotation = (it % 360 + 360) % 360 }
    val swap = !NeoPreference.isCameraV4l2Landscape() && (camRotation == 90 || camRotation == 270)
    // Clamp to ≤1080p — full-sensor YUV can't stream as a preview (capture fails).
    val cw = (if (swap) h else w).coerceIn(2, 1920)      // pre-rotation capture size
    val chh = (if (swap) w else h).coerceIn(2, 1920)
    synchronized(cameraLock) {
      if (cameraDevice != null && (openW != cw || openH != chh)) closeCamera()
    }
    reqW = cw; reqH = chh
    if (cameraDevice == null) openCamera()
  }

  private fun writeFrame(out: OutputStream, timeoutMs: Int) {
    // Wait for a fresh frame (or use the latest if one already exists).
    synchronized(frameLock) {
      val seq0 = frameSeq
      val deadline = System.currentTimeMillis() + timeoutMs
      while (frameSeq == seq0 && System.currentTimeMillis() < deadline) {
        val rem = deadline - System.currentTimeMillis()
        if (rem <= 0) break
        runCatching { frameLock.wait(rem) }
      }
    }
    val landscape = NeoPreference.isCameraV4l2Landscape()
    // Landscape (default): deliver the raw sensor frame. Upright: the rotated one.
    val src = if (landscape) rawNv21 else latestNv21
    val sw = if (landscape) rawW else nv21W
    val sh = if (landscape) rawH else nv21H
    val data: ByteArray? = when {
      src == null -> null
      v4lFourcc == "YUYV" -> nv21ToYuyv(src, sw, sh)
      landscape -> nv21ToJpeg(src, sw, sh)     // MJPG, native orientation
      else -> latestJpeg                        // MJPG, already-rotated (HTTP-shared)
    }
    if (data == null) { out.write("ERR\n".toByteArray()); return }
    out.write("OK ${data.size}\n".toByteArray())
    out.write(data)
  }

  /** Encode an NV21 buffer to JPEG (for the landscape MJPG path). */
  private fun nv21ToJpeg(nv21: ByteArray, w: Int, h: Int): ByteArray? {
    val out = ByteArrayOutputStream()
    return if (android.graphics.YuvImage(nv21, ImageFormat.NV21, w, h, null)
        .compressToJpeg(Rect(0, 0, w, h), JPEG_QUALITY, out)) out.toByteArray() else null
  }

  /** Pack an upright NV21 buffer into YUYV (YUY2): Y0 U Y1 V per pixel pair. */
  private fun nv21ToYuyv(nv21: ByteArray, w: Int, h: Int): ByteArray {
    val out = ByteArray(w * h * 2)
    val ySize = w * h
    var o = 0
    for (j in 0 until h) {
      val yRow = j * w
      val cRow = ySize + (j / 2) * w
      var i = 0
      while (i < w) {
        val y0 = nv21[yRow + i]
        val y1 = if (i + 1 < w) nv21[yRow + i + 1] else y0
        val ci = cRow + (i / 2) * 2
        val v = nv21[ci]
        val u = nv21[ci + 1]
        out[o++] = y0; out[o++] = u; out[o++] = y1; out[o++] = v
        i += 2
      }
    }
    return out
  }

  // ── controls mapped to Camera2 ──
  private fun evRange(): Range<Int> =
    chars()?.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE) ?: Range(0, 0)

  private fun maxZoomPercent(): Int {
    val ch = chars() ?: return 100
    return if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
      val r = ch.get(CameraCharacteristics.CONTROL_ZOOM_RATIO_RANGE)
      if (r != null) (r.upper * 100).toInt() else 100
    } else {
      val z = ch.get(CameraCharacteristics.SCALER_AVAILABLE_MAX_DIGITAL_ZOOM) ?: 1f
      (z * 100).toInt()
    }
  }

  // White-balance menu: index == Camera2 AWB mode value, filtered to supported.
  private val wbModes = linkedMapOf(
    1 to "Auto", 2 to "Incandescent", 3 to "Fluorescent",
    5 to "Daylight", 6 to "Cloudy", 8 to "Shade")
  private fun wbAvailable(): Map<Int, String> {
    val avail = chars()?.get(CameraCharacteristics.CONTROL_AWB_AVAILABLE_MODES)?.toSet() ?: return wbModes
    return wbModes.filterKeys { avail.contains(it) }.ifEmpty { wbModes }
  }
  private val antibandModes = linkedMapOf(0 to "Disabled", 1 to "50 Hz", 2 to "60 Hz", 3 to "Auto")

  private fun ctrlListReply(): String {
    val ev = evRange()
    val maxZoom = maxZoomPercent()
    val lines = ArrayList<String>()
    // id type min max step def flags name      (type: 1=int 2=bool 3=menu)
    lines.add("$CID_BRIGHTNESS 1 ${ev.lower} ${ev.upper} 1 0 0 Brightness")
    lines.add("$CID_FOCUS_AUTO 2 0 1 1 1 0 Focus, Automatic Continuous")
    if (maxZoom > 100) lines.add("$CID_ZOOM_ABS 1 100 $maxZoom 1 100 0 Zoom, Absolute")
    val wb = wbAvailable().keys.toList()
    if (wb.isNotEmpty()) lines.add("$CID_WHITE_BALANCE 3 ${wb.min()} ${wb.max()} 1 1 0 White Balance, Preset")
    lines.add("$CID_POWER_LINE 3 0 3 1 3 0 Power Line Frequency")
    // Camera selection (when more than one facing is present).
    val facings = availableFacings()
    if (facings.size > 1) lines.add("$CID_CAMERA_SEL 3 ${facings.keys.min()} ${facings.keys.max()} 1 0 0 Camera Selection")
    // Manual focus (only on autofocus lenses).
    if (minFocusDist() > 0f) lines.add("$CID_FOCUS_ABS 1 0 100 1 0 0 Focus, Absolute")
    // Manual exposure + ISO (only when the sensor supports manual control).
    if (hasManualSensor()) {
      val (elo, ehi) = expRange100us(); val iso = isoRange()
      lines.add("$CID_EXPOSURE_AUTO 3 0 1 1 0 0 Exposure, Auto")
      lines.add("$CID_EXPOSURE_ABS 1 $elo $ehi 1 $elo 0 Exposure, Absolute")
      lines.add("$CID_GAIN 1 ${iso.lower} ${iso.upper} 1 ${iso.lower} 0 ISO/Gain")
    }
    val sb = StringBuilder("OK ${lines.size}\n")
    for (l in lines) sb.append(l).append("\n")
    return sb.toString()
  }

  /** Menu items "OK <n>" + "<index> <name>" for a menu control (else "OK 0"). */
  private fun ctrlMenuReply(id: Int): String {
    val items = when (id) {
      CID_WHITE_BALANCE -> wbAvailable()
      CID_POWER_LINE -> antibandModes
      CID_CAMERA_SEL -> availableFacings()
      CID_EXPOSURE_AUTO -> linkedMapOf(0 to "Auto Mode", 1 to "Manual Mode")
      else -> emptyMap()
    }
    val sb = StringBuilder("OK ${items.size}\n")
    for ((idx, name) in items) sb.append("$idx $name\n")
    return sb.toString()
  }

  private fun ctrlGet(id: Int): Int? = when (id) {
    CID_BRIGHTNESS -> ctrlEv
    CID_FOCUS_AUTO -> ctrlAf
    CID_ZOOM_ABS -> ctrlZoom
    CID_WHITE_BALANCE -> ctrlWb
    CID_POWER_LINE -> ctrlAntibanding
    CID_CAMERA_SEL -> ctrlFacing
    CID_FOCUS_ABS -> ctrlFocusAbs
    CID_EXPOSURE_AUTO -> ctrlExpAuto
    // Lazily seed manual exposure/ISO to a valid default so reads aren't < min.
    CID_EXPOSURE_ABS -> { if (ctrlExpAbs == 0) ctrlExpAbs = expRange100us().first; ctrlExpAbs }
    CID_GAIN -> { if (ctrlIso == 0) ctrlIso = isoRange().lower; ctrlIso }
    else -> null
  }

  private fun ctrlSet(id: Int, value: Int): Boolean {
    when (id) {
      CID_BRIGHTNESS -> { val r = evRange(); ctrlEv = value.coerceIn(r.lower, r.upper) }
      CID_FOCUS_AUTO -> ctrlAf = if (value != 0) 1 else 0
      CID_ZOOM_ABS -> ctrlZoom = value.coerceIn(100, maxZoomPercent())
      CID_WHITE_BALANCE -> ctrlWb = if (wbAvailable().containsKey(value)) value else ctrlWb
      CID_POWER_LINE -> ctrlAntibanding = value.coerceIn(0, 3)
      CID_CAMERA_SEL -> {
        val want = if (value == 1) 1 else 0
        if (availableFacings().containsKey(want) && want != ctrlFacing) {
          ctrlFacing = want
          // Switch the physical camera: reopen with the other facing.
          synchronized(cameraLock) { if (cameraDevice != null) closeCamera() }
          if (v4lStreaming || httpClients.get() > 0) openCamera()
          return true   // openCamera resubmits with controls; skip reapply on a dead session
        }
      }
      CID_FOCUS_ABS -> ctrlFocusAbs = value.coerceIn(0, 100)
      CID_EXPOSURE_AUTO -> ctrlExpAuto = if (value != 0) 1 else 0
      CID_EXPOSURE_ABS -> { val (lo, hi) = expRange100us(); ctrlExpAbs = value.coerceIn(lo, hi) }
      CID_GAIN -> { val r = isoRange(); ctrlIso = value.coerceIn(r.lower, r.upper) }
      else -> return false
    }
    reapplyControls()
    return true
  }

  private fun applyControls(req: CaptureRequest.Builder) {
    // Focus: continuous AF, or OFF + manual focus distance.
    if (ctrlAf != 0) {
      req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
    } else {
      req.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_OFF)
      val mfd = minFocusDist()
      if (mfd > 0f) req.set(CaptureRequest.LENS_FOCUS_DISTANCE, (ctrlFocusAbs / 100f) * mfd)
    }
    if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R && ctrlZoom > 100)
      req.set(CaptureRequest.CONTROL_ZOOM_RATIO, ctrlZoom / 100f)
    req.set(CaptureRequest.CONTROL_AWB_MODE, ctrlWb)
    req.set(CaptureRequest.CONTROL_AE_ANTIBANDING_MODE, ctrlAntibanding)
    // Exposure: manual (AE off + sensor exposure/ISO) when requested + supported,
    // else auto AE with the user's exposure-compensation bias.
    if (ctrlExpAuto == 1 && hasManualSensor()) {
      req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_OFF)
      // Fall back to mid-range exposure / lowest ISO if unset, so manual isn't black.
      val (elo, ehi) = expRange100us(); val isr = isoRange()
      val exp = if (ctrlExpAbs in elo..ehi) ctrlExpAbs else (elo + ehi) / 2
      val iso = if (ctrlIso in isr.lower..isr.upper) ctrlIso else isr.lower
      runCatching { req.set(CaptureRequest.SENSOR_EXPOSURE_TIME, exp.toLong() * 100_000L) }
      runCatching { req.set(CaptureRequest.SENSOR_SENSITIVITY, iso) }
      runCatching { req.set(CaptureRequest.SENSOR_FRAME_DURATION, 33_333_333L) } // ~30 fps
    } else {
      req.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
      if ((evRange().upper - evRange().lower) != 0)
        req.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, ctrlEv)
    }
    // ── low-latency / smoothness tuning ──
    // Lock the frame-rate floor so auto-exposure can't drop to 15/7 fps in low
    // light (the usual cause of a laggy, stuttery preview).
    bestFpsRange()?.let { req.set(CaptureRequest.CONTROL_AE_TARGET_FPS_RANGE, it) }
    // No video stabilisation / minimal post-processing → fewer pipeline stages.
    runCatching { req.set(CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE, CaptureRequest.CONTROL_VIDEO_STABILIZATION_MODE_OFF) }
    runCatching { req.set(CaptureRequest.NOISE_REDUCTION_MODE, CaptureRequest.NOISE_REDUCTION_MODE_FAST) }
    runCatching { req.set(CaptureRequest.EDGE_MODE, CaptureRequest.EDGE_MODE_FAST) }
  }

  /** Pick a fixed-30 (or highest constant) target FPS range to keep the preview
   *  from dropping its frame rate in low light. */
  private fun bestFpsRange(): Range<Int>? {
    val ranges = chars()?.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_TARGET_FPS_RANGES) ?: return null
    // Prefer a constant 30 fps; else the constant range with the highest fps;
    // else the range with the highest lower bound (least likely to stutter).
    return ranges.firstOrNull { it.lower == 30 && it.upper == 30 }
      ?: ranges.filter { it.lower == it.upper }.maxByOrNull { it.upper }
      ?: ranges.maxByOrNull { it.lower }
  }

  /** Rebuild + re-submit the repeating request after a control change. */
  private fun reapplyControls() {
    synchronized(cameraLock) {
      val device = cameraDevice ?: return
      val session = captureSession ?: return
      val surface = imageReader?.surface ?: return
      runCatching {
        val req = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
        req.addTarget(surface)
        applyControls(req)
        session.setRepeatingRequest(req.build(), null, cameraHandler)
      }
    }
  }
}
