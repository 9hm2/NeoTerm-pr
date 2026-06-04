package io.neoterm.services

import android.annotation.SuppressLint
import android.app.*
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.Uri
import android.net.wifi.WifiManager
import android.os.Binder
import android.os.Build
import android.os.IBinder
import android.os.PowerManager
import android.provider.Settings
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.NotificationCompat
import io.neoterm.R
import io.neoterm.backend.EmulatorDebug
import io.neoterm.backend.TerminalSession
import io.neoterm.component.session.ShellParameter
import io.neoterm.component.session.XParameter
import io.neoterm.component.session.XSession
import io.neoterm.ui.term.NeoTermActivity
import io.neoterm.utils.NLog
import io.neoterm.utils.Terminals


/**
 * @author kiva
 */

class NeoTermService : Service() {
  inner class NeoTermBinder : Binder() {
    var service = this@NeoTermService
  }

  private val serviceBinder = NeoTermBinder()
  private val mTerminalSessions = ArrayList<TerminalSession>()
  private val mXSessions = ArrayList<XSession>()
  private var mWakeLock: PowerManager.WakeLock? = null
  private var mWifiLock: WifiManager.WifiLock? = null

  // The embedded X server runs in its own process (com.termux.x11.NeoX11Service).
  // We keep it alive by binding it with BIND_IMPORTANT, so it needs no
  // notification of its own — its status is shown in this service's notification.
  private var x11Bound = false
  private var x11Running = false
  private val x11Connection = object : ServiceConnection {
    override fun onServiceConnected(name: ComponentName?, service: IBinder?) {}
    override fun onServiceDisconnected(name: ComponentName?) {}
  }

  override fun onCreate() {
    super.onCreate()
    createNotificationChannel()
    startForeground(NOTIFICATION_ID, createNotification())
    // Wake lock on by default (keep the CPU running). Don't pop the battery-
    // optimization dialog at startup — that's only prompted on a manual acquire.
    acquireLock(promptBatteryOpt = false)
    // Start the Android-side PulseAudio with the app, so terminal apps (not just
    // X11) can play audio via PULSE_SERVER=127.0.0.1:4713.
    io.neoterm.utils.PulseAudioBridge.start(this)
  }

  override fun onBind(intent: Intent): IBinder? {
    return serviceBinder
  }

  override fun onStartCommand(intent: Intent, flags: Int, startId: Int): Int {
    val action = intent.action
    when (action) {
      ACTION_SERVICE_STOP -> {
        for (i in mTerminalSessions.indices)
          mTerminalSessions[i].finishIfRunning()
        teardownAndStop()
      }

      ACTION_ACQUIRE_LOCK -> acquireLock(promptBatteryOpt = true)

      ACTION_RELEASE_LOCK -> releaseLock()

      ACTION_X11_START -> startX11Server(intent)

      ACTION_X11_STOP -> stopX11Server()
    }

    return Service.START_NOT_STICKY
  }

  override fun onDestroy() {
    stopForeground(true)
    runCatching {
      (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(NOTIFICATION_ID)
    }
    stopX11Server()
    io.neoterm.utils.PulseAudioBridge.stop()

    for (i in mTerminalSessions.indices)
      mTerminalSessions[i].finishIfRunning()
    mTerminalSessions.clear()
  }

  /**
   * Bind the embedded X server's dedicated process with BIND_IMPORTANT so it
   * stays alive at this (foreground) service's priority — no separate
   * notification needed. The TMPDIR/XKB config travels in the bind intent.
   */
  private fun startX11Server(intent: Intent) {
    if (!x11Bound) {
      val bindIntent = Intent(this, com.termux.x11.NeoX11Service::class.java)
        .putExtra(com.termux.x11.NeoX11Service.EXTRA_TMPDIR, intent.getStringExtra(com.termux.x11.NeoX11Service.EXTRA_TMPDIR))
        .putExtra(com.termux.x11.NeoX11Service.EXTRA_XKB, intent.getStringExtra(com.termux.x11.NeoX11Service.EXTRA_XKB))
      x11Bound = bindService(
        bindIntent, x11Connection,
        Context.BIND_AUTO_CREATE or Context.BIND_IMPORTANT
      )
    }
    x11Running = true
    updateNotification()
  }

  private fun stopX11Server() {
    if (x11Bound) {
      runCatching { unbindService(x11Connection) }
      x11Bound = false
    }
    x11Running = false
    updateNotification()
  }

  val sessions: List<TerminalSession>
    get() = mTerminalSessions

  val xSessions: List<XSession>
    get() = mXSessions

  fun createTermSession(parameter: ShellParameter): TerminalSession {
    val session = createOrFindSession(parameter)
    updateNotification()
    return session
  }

  fun removeTermSession(sessionToRemove: TerminalSession): Int {
    val indexOfRemoved = mTerminalSessions.indexOf(sessionToRemove)
    if (indexOfRemoved >= 0) {
      mTerminalSessions.removeAt(indexOfRemoved)
      updateNotification()
      stopSelfIfNoSessions()
    }
    return indexOfRemoved
  }

  fun createXSession(activity: AppCompatActivity, parameter: XParameter): XSession {
    val session = Terminals.createSession(activity, parameter)
    mXSessions.add(session)
    updateNotification()
    return session
  }

  fun removeXSession(sessionToRemove: XSession): Int {
    val indexOfRemoved = mXSessions.indexOf(sessionToRemove)
    if (indexOfRemoved >= 0) {
      mXSessions.removeAt(indexOfRemoved)
      updateNotification()
      stopSelfIfNoSessions()
    }
    return indexOfRemoved
  }

  /**
   * Shut down once the user has closed the last session. This only runs on an
   * actual removal (never at startup, where the list is empty before the first
   * session is created), so the foreground service — and its notification —
   * doesn't linger (or get re-spawned) with zero sessions.
   */
  private fun stopSelfIfNoSessions() {
    if (mTerminalSessions.isEmpty() && mXSessions.isEmpty()) {
      teardownAndStop()
    }
  }

  /**
   * Fully stop: finish the activity and drop its task (so the still-bound or
   * recents-resident activity can't re-create this service), remove the
   * foreground notification, then stop. onDestroy unbinds/stops the embedded
   * X11 server process.
   */
  private fun teardownAndStop() {
    NeoTermActivity.getInstance()?.finishAndRemoveTask()
    // Close the embedded X11 window too (its server is unbound in onDestroy).
    // We must NOT route this through startService(ACTION_X11_STOP): that would
    // re-create this just-stopped service and bring back a session-less
    // notification.
    runCatching { com.termux.x11.MainActivity.getInstance()?.finishAndRemoveTask() }
    stopForeground(true)
    // Explicitly cancel the notification: updateNotification() (called from
    // removeTermSession just before teardown) re-posts it via
    // NotificationManager.notify(), which detaches it from the foreground
    // lifecycle, so stopForeground(true) alone leaves the 0-session notification
    // stuck on screen after the service is already dead.
    runCatching {
      (getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager).cancel(NOTIFICATION_ID)
    }
    stopSelf()
  }

  private fun createOrFindSession(parameter: ShellParameter): TerminalSession {
    if (parameter.willCreateNewSession()) {
      NLog.d("createOrFindSession: creating new session")
      val session = Terminals.createSession(this, parameter)
      mTerminalSessions.add(session)
      return session
    }

    val sessionId = parameter.sessionId!!
    NLog.d("createOrFindSession: find session by id $sessionId")

    val session = mTerminalSessions.find { it.mHandle == sessionId.sessionId }
      ?: throw IllegalArgumentException("cannot find session by given id")

    session.write(parameter.initialCommand + "\n")
    return session
  }

  private fun updateNotification() {
    val service = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    service.notify(NOTIFICATION_ID, createNotification())
  }

  private fun createNotification(): Notification {
    val notifyIntent = Intent(this, NeoTermActivity::class.java)
    notifyIntent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    val pendingIntent = PendingIntent.getActivity(this, 0, notifyIntent, 0)

    val sessionCount = mTerminalSessions.size
    val xSessionCount = mXSessions.size
    var contentText = getString(R.string.service_status_text, sessionCount, xSessionCount)

    if (x11Running) contentText += getString(R.string.service_x11_running)

    val lockAcquired = mWakeLock != null
    if (lockAcquired) contentText += getString(R.string.service_lock_acquired)

    val builder = NotificationCompat.Builder(this, DEFAULT_CHANNEL_ID)
    builder.setContentTitle(getText(R.string.app_name))
    builder.setContentText(contentText)
    builder.setSmallIcon(R.drawable.ic_terminal_running)
    builder.setContentIntent(pendingIntent)
    builder.setOngoing(true)
    builder.setShowWhen(false)
    builder.color = 0xFF000000.toInt()

    builder.priority = if (lockAcquired) Notification.PRIORITY_HIGH else Notification.PRIORITY_LOW

    val exitIntent = Intent(this, NeoTermService::class.java).setAction(ACTION_SERVICE_STOP)
    builder.addAction(
      android.R.drawable.ic_delete,
      getString(R.string.exit),
      PendingIntent.getService(this, 0, exitIntent, 0)
    )

    val newWakeAction = if (lockAcquired) ACTION_RELEASE_LOCK else ACTION_ACQUIRE_LOCK
    val toggleWakeLockIntent = Intent(this, NeoTermService::class.java).setAction(newWakeAction)
    val actionTitle = getString(
      if (lockAcquired)
        R.string.service_release_lock
      else
        R.string.service_acquire_lock
    )
    val actionIcon = if (lockAcquired) android.R.drawable.ic_lock_idle_lock else android.R.drawable.ic_lock_lock
    builder.addAction(actionIcon, actionTitle, PendingIntent.getService(this, 0, toggleWakeLockIntent, 0))

    return builder.build()
  }

  private fun createNotificationChannel() {
    if (Build.VERSION.SDK_INT < Build.VERSION_CODES.O) return

    val channel = NotificationChannel(DEFAULT_CHANNEL_ID, "NeoTerm", NotificationManager.IMPORTANCE_LOW)
    channel.description = "NeoTerm notifications"
    val manager = getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    manager.createNotificationChannel(channel)
  }

  @SuppressLint("WakelockTimeout")
  private fun acquireLock(promptBatteryOpt: Boolean = true) {
    if (mWakeLock == null) {
      val pm = getSystemService(Context.POWER_SERVICE) as PowerManager
      mWakeLock = pm.newWakeLock(
        PowerManager.PARTIAL_WAKE_LOCK,
        EmulatorDebug.LOG_TAG + ":"
      )
      mWakeLock!!.acquire()

      val wm = applicationContext.getSystemService(Context.WIFI_SERVICE) as WifiManager
      mWifiLock = wm.createWifiLock(WifiManager.WIFI_MODE_FULL_HIGH_PERF, EmulatorDebug.LOG_TAG)
      mWifiLock!!.acquire()

      // A PARTIAL_WAKE_LOCK only reliably keeps the CPU running with the screen
      // off if the app is exempt from Doze/battery optimization, so prompt the
      // user to allow it — but only on an explicit acquire, not at startup.
      if (promptBatteryOpt) requestDisableBatteryOptimization(pm)

      updateNotification()
    }
  }

  @SuppressLint("BatteryLife")
  private fun requestDisableBatteryOptimization(pm: PowerManager) {
    try {
      if (!pm.isIgnoringBatteryOptimizations(packageName)) {
        val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS)
          .setData(Uri.parse("package:$packageName"))
          .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        startActivity(intent)
      }
    } catch (e: Exception) {
      // Some ROMs don't expose the dialog; fall back to the general settings.
      NLog.e("NeoTermService", "Battery optimization request failed: ${e.localizedMessage}")
      try {
        startActivity(
          Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        )
      } catch (ignored: Exception) {
      }
    }
  }

  private fun releaseLock() {
    if (mWakeLock != null) {
      mWakeLock!!.release()
      mWakeLock = null

      mWifiLock!!.release()
      mWifiLock = null

      updateNotification()
    }
  }

  companion object {
    val ACTION_SERVICE_STOP = "neoterm.action.service.stop"
    val ACTION_ACQUIRE_LOCK = "neoterm.action.service.lock.acquire"
    val ACTION_RELEASE_LOCK = "neoterm.action.service.lock.release"
    const val ACTION_X11_START = "neoterm.action.service.x11.start"
    const val ACTION_X11_STOP = "neoterm.action.service.x11.stop"
    private val NOTIFICATION_ID = 52019

    val DEFAULT_CHANNEL_ID = "neoterm_notification_channel"
  }
}
