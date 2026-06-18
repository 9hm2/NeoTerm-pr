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
import android.os.Handler
import android.os.HandlerThread
import android.util.Log
import androidx.core.content.ContextCompat
import io.neoterm.component.config.NeoPreference
import java.io.ByteArrayOutputStream
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
  /** Camera sensor mounting orientation in degrees; frames are rotated by this to be upright. */
  @Volatile private var sensorOrientation = 0

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
    appContext = context.applicationContext
    serverThread = Thread({ serverLoop() }, "camera-bridge").apply { isDaemon = true; start() }
  }

  fun stop() {
    running = false
    runCatching { serverSocket?.close() }
    serverSocket = null
    serverThread = null
    closeCamera()
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

        val thread = HandlerThread("camera-capture").apply { start() }
        val handler = Handler(thread.looper)
        cameraThread = thread
        cameraHandler = handler

        val reader = ImageReader.newInstance(size.first, size.second, ImageFormat.YUV_420_888, 2)
        reader.setOnImageAvailableListener({ r ->
          val img = try { r.acquireLatestImage() } catch (e: Exception) { null } ?: return@setOnImageAvailableListener
          try {
            val jpeg = yuvToJpeg(img)
            if (jpeg != null) {
              latestJpeg = jpeg
              synchronized(frameLock) { frameLock.notifyAll() }
            }
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
            val req = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW).apply {
              addTarget(surface)
              set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
            }
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
      ids.firstOrNull {
        cm.getCameraCharacteristics(it).get(CameraCharacteristics.LENS_FACING) ==
          CameraCharacteristics.LENS_FACING_BACK
      } ?: ids.firstOrNull()
    } catch (e: Exception) {
      null
    }
  }

  private fun chooseSize(cm: CameraManager, camId: String): Pair<Int, Int> {
    val (tw, th) = requestedSize()
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

  private fun yuvToJpeg(image: Image): ByteArray? {
    var nv21 = yuv420ToNv21(image)
    var w = image.width
    var h = image.height
    // Rotate to upright using the sensor mounting orientation (back cameras are usually 90°),
    // otherwise the stream comes out sideways.
    val rot = ((sensorOrientation % 360) + 360) % 360
    if (rot != 0) {
      nv21 = rotateNv21(nv21, w, h, rot)
      if (rot == 90 || rot == 270) { val t = w; w = h; h = t }
    }
    val yuv = YuvImage(nv21, ImageFormat.NV21, w, h, null)
    val out = ByteArrayOutputStream()
    return if (yuv.compressToJpeg(Rect(0, 0, w, h), JPEG_QUALITY, out))
      out.toByteArray() else null
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
}
