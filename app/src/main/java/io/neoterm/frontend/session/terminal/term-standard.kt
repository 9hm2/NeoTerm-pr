package io.neoterm.frontend.session.terminal

import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.content.ClipData
import android.content.ClipboardManager
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.RingtoneManager
import android.os.VibrationEffect
import android.os.Vibrator
import android.util.Log
import android.view.InputDevice
import android.view.KeyEvent
import android.view.MotionEvent
import android.view.View
import android.view.inputmethod.InputMethodManager
import androidx.core.app.NotificationCompat
import io.neoterm.BuildConfig
import io.neoterm.R
import io.neoterm.backend.KeyHandler
import io.neoterm.backend.TerminalSession
import io.neoterm.component.ComponentManager
import io.neoterm.component.completion.*
import io.neoterm.component.config.NeoPreference
import io.neoterm.component.extrakey.ExtraKeyComponent
import io.neoterm.component.session.ShellTermSession
import io.neoterm.frontend.completion.CandidatePopupWindow
import io.neoterm.frontend.session.view.TerminalView
import io.neoterm.frontend.session.view.TerminalViewClient
import java.util.*

/**
 * @author kiva
 */
class TermViewClient(val context: Context) : TerminalViewClient {
  private var mVirtualControlKeyDown: Boolean = false
  private var mVirtualFnKeyDown: Boolean = false
  private var lastTitle: String = ""

  var termSessionData: TermSessionData? = null

  override fun onScale(scale: Float): Float {
    if (scale < 0.9f || scale > 1.1f) {
      val increase = scale > 1f
      changeFontSize(increase)
      return 1.0f
    }
    return scale
  }

  override fun onSingleTapUp(e: MotionEvent?) {
    val termView = termSessionData?.termView ?: return
    (context.getSystemService(Context.INPUT_METHOD_SERVICE) as InputMethodManager)
      .showSoftInput(termView, InputMethodManager.SHOW_IMPLICIT)
  }

  override fun shouldBackButtonBeMappedToEscape(): Boolean {
    val shellSession = termSessionData?.termSession as ShellTermSession? ?: return false
    return shellSession.shellProfile.enableBackKeyToEscape
  }

  override fun copyModeChanged(copyMode: Boolean) {
    // Keep the soft keyboard visible while selecting text (the user asked for
    // it to stay up in selection mode), so do nothing here.
  }

  override fun onKeyDown(keyCode: Int, e: KeyEvent?, session: TerminalSession?): Boolean {
    if (handleVirtualKeys(keyCode, e, true)) {
      return true
    }

    val termUI = termSessionData?.termUI

    when (keyCode) {
      KeyEvent.KEYCODE_ENTER -> {
        if (e?.action == KeyEvent.ACTION_DOWN && session?.isRunning == false) {
          termUI?.requireClose()
          return true
        }
        return false
      }
      KeyEvent.KEYCODE_BACK -> {
        if (e?.action == KeyEvent.ACTION_DOWN) {
          return termUI?.requireFinishAutoCompletion() ?: false
        }
        return false
      }
    }

    // TODO 自定义快捷键
    if (e != null && e.isCtrlPressed && e.isShiftPressed) {
      // Get the unmodified code point:
      val unicodeChar = e.getUnicodeChar(0).toChar()

      when (unicodeChar) {
        'v' -> termUI?.requirePaste()
        'n' -> termUI?.requireCreateNew()
        'z' -> termUI?.requireSwitchToPrevious()
        'x' -> termUI?.requireSwitchToNext()
        'f' -> termUI?.requireToggleFullScreen()
        '-' -> changeFontSize(false)
        '+' -> changeFontSize(true)
      }

      // 当要触发 NeoTerm 快捷键时，屏蔽所有终端处理key
      return true
    } else if (e != null && e.isAltPressed) {
      // Get the unmodified code point:
      val unicodeChar = e.getUnicodeChar(0).toChar()
      if (unicodeChar !in ('1'..'9')) {
        return false
      }

      // Use Alt + num to switch sessions
      val sessionIndex = unicodeChar.toInt() - '0'.toInt()
      termUI?.requireSwitchTo(sessionIndex)

      // 当要触发 NeoTerm 快捷键时，屏蔽所有终端处理key
      return true
    }
    return false
  }

  override fun onKeyUp(keyCode: Int, e: KeyEvent?): Boolean {
    return handleVirtualKeys(keyCode, e, false)
  }

  override fun readControlKey(): Boolean {
    val extraKeysView = termSessionData?.extraKeysView
    return (extraKeysView != null && extraKeysView.readControlButton()) || mVirtualControlKeyDown
  }

  override fun readAltKey(): Boolean {
    val extraKeysView = termSessionData?.extraKeysView
    return (extraKeysView != null && extraKeysView.readAltButton()) || mVirtualFnKeyDown
  }

  override fun onCodePoint(codePoint: Int, ctrlDown: Boolean, session: TerminalSession?): Boolean {
    if (mVirtualFnKeyDown) {
      var resultingKeyCode: Int = -1
      var resultingCodePoint: Int = -1
      var altDown = false
      val lowerCase = Character.toLowerCase(codePoint)
      when (lowerCase.toChar()) {
        // Arrow keys.
        'w' -> resultingKeyCode = KeyEvent.KEYCODE_DPAD_UP
        'a' -> resultingKeyCode = KeyEvent.KEYCODE_DPAD_LEFT
        's' -> resultingKeyCode = KeyEvent.KEYCODE_DPAD_DOWN
        'd' -> resultingKeyCode = KeyEvent.KEYCODE_DPAD_RIGHT

        // Page up and down.
        'p' -> resultingKeyCode = KeyEvent.KEYCODE_PAGE_UP
        'n' -> resultingKeyCode = KeyEvent.KEYCODE_PAGE_DOWN

        // Some special keys:
        't' -> resultingKeyCode = KeyEvent.KEYCODE_TAB
        'i' -> resultingKeyCode = KeyEvent.KEYCODE_INSERT
        'h' -> resultingCodePoint = '~'.toInt()

        // Special characters to input.
        'u' -> resultingCodePoint = '_'.toInt()
        'l' -> resultingCodePoint = '|'.toInt()

        // Function keys.
        '1', '2', '3', '4', '5', '6', '7', '8', '9' -> resultingKeyCode = codePoint - '1'.toInt() + KeyEvent.KEYCODE_F1
        '0' -> resultingKeyCode = KeyEvent.KEYCODE_F10

        // Other special keys.
        'e' -> resultingCodePoint = 27 /*Escape*/
        '.' -> resultingCodePoint = 28 /*^.*/

        'b' // alt+b, jumping backward in readline.
          , 'f' // alf+f, jumping forward in readline.
          , 'x' // alt+x, common in emacs.
        -> {
          resultingCodePoint = lowerCase
          altDown = true
        }

        // Volume control.
        'v' -> {
          resultingCodePoint = -1
          val audio = context.getSystemService(Context.AUDIO_SERVICE) as AudioManager
          audio.adjustSuggestedStreamVolume(
            AudioManager.ADJUST_SAME,
            AudioManager.USE_DEFAULT_STREAM_TYPE,
            AudioManager.FLAG_SHOW_UI
          )
        }
      }

      if (resultingKeyCode != -1) {
        if (session != null) {
          val term = session.emulator
          session.write(
            KeyHandler.getCode(
              resultingKeyCode,
              0,
              term.isCursorKeysApplicationMode,
              term.isKeypadApplicationMode
            )
          )
        }
      } else if (resultingCodePoint != -1) {
        session?.writeCodePoint(altDown, resultingCodePoint)
      }
      return true
    }
    return false
  }

  override fun onLongPress(event: MotionEvent?): Boolean {
    // Text selection is started by the TerminalView itself on long press; the
    // client needs no extra handling here.
    return false
  }

  override fun onSwipe(toLeft: Boolean) {
    // Page between tabs: swipe left -> next session, swipe right -> previous.
    val termUI = termSessionData?.termUI ?: return
    if (toLeft) termUI.requireSwitchToNext() else termUI.requireSwitchToPrevious()
  }

  private fun handleVirtualKeys(keyCode: Int, event: KeyEvent?, down: Boolean): Boolean {
    if (event == null) {
      return false
    }

    val shellSession = termSessionData?.termSession as ShellTermSession? ?: return false

    // Volume keys as special keys
    val volumeAsSpecialKeys = shellSession.shellProfile.enableSpecialVolumeKeys

    val inputDevice = event.device
    if (inputDevice != null && inputDevice.keyboardType == InputDevice.KEYBOARD_TYPE_ALPHABETIC) {
      return false
    } else if (keyCode == KeyEvent.KEYCODE_VOLUME_DOWN) {
      mVirtualControlKeyDown = down && volumeAsSpecialKeys
      return true
    } else if (keyCode == KeyEvent.KEYCODE_VOLUME_UP) {
      mVirtualFnKeyDown = down && volumeAsSpecialKeys
      return true
    }
    return false
  }

  fun updateExtraKeys(title: String?, force: Boolean = false) {
    val extraKeysView = termSessionData?.extraKeysView

    if (extraKeysView == null || title == null || title.isEmpty()) {
      return
    }

    if ((lastTitle != title || force)
      && updateExtraKeysVisibility()
    ) {
      removeExtraKeys()
      ComponentManager.getComponent<ExtraKeyComponent>().showShortcutKeys(title, extraKeysView)
      extraKeysView.updateButtons()
      lastTitle = title
    }
  }

  private fun updateExtraKeysVisibility(): Boolean {
    val extraKeysView = termSessionData?.extraKeysView ?: return false
    val shellSession = termSessionData?.termSession as ShellTermSession? ?: return false

    return if (shellSession.shellProfile.enableExtraKeys) {
      extraKeysView.visibility = View.VISIBLE
      true
    } else {
      extraKeysView.visibility = View.GONE
      false
    }
  }

  private fun removeExtraKeys() {
    val extraKeysView = termSessionData?.extraKeysView
    extraKeysView?.clearUserKeys()
  }

  private fun changeFontSize(increase: Boolean) {
    val termView = termSessionData?.termView
    if (termView != null) {
      val changedSize = (if (increase) 1 else -1) * 2
      val fontSize = NeoPreference.validateFontSize(termView.textSize + changedSize)
      termView.textSize = fontSize
      NeoPreference.store(NeoPreference.KEY_FONT_SIZE, fontSize)
    }
  }
}

/**
 * @author kiva
 */
class TermSessionCallback : TerminalSession.SessionChangedCallback {
  var termSessionData: TermSessionData? = null

  var bellController: BellController? = null

  override fun onTextChanged(changedSession: TerminalSession?) {
    termSessionData?.termView?.onScreenUpdated()
  }

  override fun onTitleChanged(changedSession: TerminalSession?) {
    if (changedSession?.title != null) {
      termSessionData?.termUI?.requireUpdateTitle(changedSession.title)
    }
  }

  override fun onSessionFinished(finishedSession: TerminalSession?) {
    // Exit status is set just before this callback fires.
    val exitCode = finishedSession?.exitStatus ?: 0
    termSessionData?.termUI?.requireOnSessionFinished(exitCode)
  }

  override fun onClipboardText(session: TerminalSession?, text: String?) {
    val termView = termSessionData?.termView
    if (termView != null) {
      val clipboard = termView.context.getSystemService(Context.CLIPBOARD_SERVICE) as ClipboardManager
      clipboard.primaryClip = ClipData.newPlainText("", text)
    }
  }

  override fun onBell(session: TerminalSession?) {
    val termView = termSessionData?.termView ?: return
    val shellSession = session as ShellTermSession

    if (bellController == null) {
      bellController = BellController()
    }

    bellController?.bellOrVibrate(termView.context, shellSession)
  }

  override fun onColorsChanged(session: TerminalSession?) {
    val termView = termSessionData?.termView
    if (session != null && termView != null) {
      termView.onScreenUpdated()
    }
  }
}

class BellController {
  companion object {
    private const val BELL_DELAY_MS = 100L
    private const val BELL_CHANNEL_ID = "neoterm_bell"
    private const val BELL_NOTIFICATION_ID = 52020
  }

  private var lastBellTime = 0L
  private var channelReady = false

  fun bellOrVibrate(context: Context, session: ShellTermSession) {
    val currentTime = System.currentTimeMillis()
    if (currentTime - lastBellTime < BELL_DELAY_MS) {
      return
    }
    lastBellTime = currentTime

    val app = context.applicationContext

    if (session.shellProfile.enableBell) {
      // The terminal bell is delivered as a notification using the system's
      // default notification sound, so it sounds consistent with the rest of
      // the system and also alerts the user when the app is in the background.
      postBellNotification(app)
    }

    if (session.shellProfile.enableVibrate) {
      val vibrator = app.getSystemService(Context.VIBRATOR_SERVICE) as? Vibrator
      if (vibrator != null && vibrator.hasVibrator()) {
        vibrator.vibrate(VibrationEffect.createOneShot(120, VibrationEffect.DEFAULT_AMPLITUDE))
      }
    }
  }

  private fun postBellNotification(context: Context) {
    val manager = context.getSystemService(Context.NOTIFICATION_SERVICE) as NotificationManager
    ensureChannel(context, manager)

    val launch = context.packageManager.getLaunchIntentForPackage(context.packageName)
      ?.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
    val contentIntent = if (launch != null) {
      PendingIntent.getActivity(
        context, 0, launch,
        PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE
      )
    } else null

    val builder = NotificationCompat.Builder(context, BELL_CHANNEL_ID)
      .setSmallIcon(R.drawable.ic_terminal_running)
      .setContentTitle(context.getString(R.string.app_name))
      .setContentText(context.getString(R.string.bell_notification_text))
      .setAutoCancel(true)
      .setCategory(NotificationCompat.CATEGORY_MESSAGE)
      .setPriority(NotificationCompat.PRIORITY_HIGH)
      .setTimeoutAfter(4000)
    if (contentIntent != null) builder.setContentIntent(contentIntent)

    manager.notify(BELL_NOTIFICATION_ID, builder.build())
  }

  private fun ensureChannel(context: Context, manager: NotificationManager) {
    if (channelReady) return
    val channel = NotificationChannel(
      BELL_CHANNEL_ID,
      context.getString(R.string.bell_channel_name),
      NotificationManager.IMPORTANCE_HIGH
    )
    val sound = RingtoneManager.getDefaultUri(RingtoneManager.TYPE_NOTIFICATION)
    if (sound != null) {
      val attrs = AudioAttributes.Builder()
        .setUsage(AudioAttributes.USAGE_NOTIFICATION_EVENT)
        .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
        .build()
      channel.setSound(sound, attrs)
    }
    // Vibration is governed by the separate "Vibrate" setting, not the channel.
    channel.enableVibration(false)
    channel.setShowBadge(false)
    manager.createNotificationChannel(channel)
    channelReady = true
  }
}

class TermCompleteListener(var terminalView: TerminalView?) : OnAutoCompleteListener, OnCandidateSelectedListener {
  private val inputStack = Stack<Char>()
  private var popupWindow: CandidatePopupWindow? = null
  private var lastCompletedIndex = 0

  override fun onKeyCode(keyCode: Int, keyMod: Int) {
    when (keyCode) {
      KeyEvent.KEYCODE_DEL -> {
        popChar()
        fixLastCompletedIndex()
        triggerCompletion()
      }

      KeyEvent.KEYCODE_ENTER -> {
        clearChars()
        popupWindow?.dismiss()
      }
    }
  }

  private fun fixLastCompletedIndex() {
    val currentText = getCurrentEditingText()
    lastCompletedIndex = minOf(lastCompletedIndex, currentText.length - 1)
  }

  override fun onCompletionRequired(newText: String?) {
    if (newText == null || newText.isEmpty()) {
      return
    }
    pushString(newText)
    triggerCompletion()
  }

  override fun onCleanUp() {
    popupWindow?.dismiss()
    popupWindow?.cleanup()
    popupWindow = null
    terminalView = null
  }

  override fun onFinishCompletion(): Boolean {
    val popWindow = popupWindow ?: return false

    if (popWindow.isShowing()) {
      popWindow.dismiss()
      return true
    }
    return false
  }

  override fun onCandidateSelected(candidate: CompletionCandidate) {
    val session = terminalView?.currentSession ?: return
    val textNeedCompletion = getCurrentEditingText().substring(lastCompletedIndex + 1)
    val newText = candidate.completeString

    val deleteLength = newText.indexOf(textNeedCompletion) + textNeedCompletion.length
    if (deleteLength > 0) {
      for (i in 0 until deleteLength) {
        session.write("\b")
        popChar()
      }
    }

    if (BuildConfig.DEBUG) {
      Log.e(
        "NeoTerm-AC",
        "currentEditing: $textNeedCompletion, " +
          "deleteLength: $deleteLength, completeString: $newText"
      )
    }

    pushString(newText)
    session.write(newText)
    // Trigger next completion
    lastCompletedIndex = inputStack.size
    triggerCompletion()
  }

  private fun triggerCompletion() {
    val text = getCurrentEditingText()
    if (text.isEmpty()) {
      return
    }

    val result = CompletionManager.tryCompleteFor(text)
    if (!result.hasResult()) {
      // A provider accepted the task
      // But no candidates are provided
      // Give it zero angrily!
      result.markScore(0)
      onFinishCompletion()
      return
    }
    showAutoCompleteCandidates(result)
  }

  private fun showAutoCompleteCandidates(result: CompletionResult) {
    val termView = terminalView
    var popWindow = popupWindow

    if (termView == null) {
      return
    }

    if (popWindow == null) {
      popWindow = CandidatePopupWindow(termView.context)
      popWindow.onCandidateSelectedListener = this
      this.popupWindow = popWindow
    }

    popWindow.candidates = result.candidates
    popWindow.show(termView)
  }

  private fun getCurrentEditingText(): String {
    val builder = StringBuilder()
    val size = inputStack.size
    var start = inputStack.lastIndexOf(' ')
    if (start < 0) {
      // Yes, it is -1, we will do `start + 1` below.
      start = -1
    }

    IntRange(start + 1, size - 1)
      .map { inputStack[it] }
      .takeWhile { !(it == 0.toChar() || it == ' ') }
      .forEach { builder.append(it) }
    return builder.toString()
  }

  private fun clearChars() {
    inputStack.clear()
    lastCompletedIndex = 0
  }

  private fun popChar() {
    if (inputStack.isNotEmpty()) {
      inputStack.pop()
    }
  }

  private fun pushString(string: String) {
    string.toCharArray().forEach { pushChar(it) }
  }

  private fun pushChar(char: Char) {
    inputStack.push(char)
  }
}
