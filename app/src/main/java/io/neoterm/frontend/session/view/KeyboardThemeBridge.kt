package io.neoterm.frontend.session.view

import android.os.Bundle
import android.util.Log
import android.view.inputmethod.EditorInfo
import io.neoterm.backend.TerminalColors
import io.neoterm.backend.TerminalEmulator
import io.neoterm.backend.TextStyle
import io.neoterm.component.ComponentManager
import io.neoterm.component.colorscheme.ColorSchemeComponent

/**
 * Hands the terminal's colors to a co-operating on-screen keyboard (pcKeyboard)
 * through [EditorInfo.extras]. The keyboard derives its whole theme — including
 * its accent — from the background and foreground so it blends into the terminal
 * with minimal deviation. The extras are namespaced under the keyboard's package
 * and are simply ignored by any other IME.
 */
object KeyboardThemeBridge {

  private const val TAG = "KbTheme"

  /** EditorInfo.extras key carrying the terminal background color (ARGB int). */
  private const val EXTRA_BACKGROUND = "com.pckeyboard.ime.theme.BACKGROUND"

  /** EditorInfo.extras key carrying the terminal foreground color (ARGB int). */
  private const val EXTRA_FOREGROUND = "com.pckeyboard.ime.theme.FOREGROUND"

  /**
   * Writes the current background/foreground colors into [outAttrs] so the
   * keyboard is themed from the very first input session.
   */
  @JvmStatic
  fun applyTo(outAttrs: EditorInfo, emulator: TerminalEmulator?) {
    val colors = resolveColors(emulator) ?: run {
      Log.d(TAG, "applyTo: no colors resolved (emulator=${emulator != null})")
      return
    }
    val extras = outAttrs.extras ?: Bundle().also { outAttrs.extras = it }
    extras.putInt(EXTRA_BACKGROUND, colors[0])
    extras.putInt(EXTRA_FOREGROUND, colors[1])
    Log.d(TAG, "applyTo: wrote bg=#${Integer.toHexString(colors[0])} " +
      "fg=#${Integer.toHexString(colors[1])} (emulator=${emulator != null})")
  }

  /** Returns `[background, foreground]` as ARGB ints, or null if unavailable. */
  private fun resolveColors(emulator: TerminalEmulator?): IntArray? {
    // Once a session is attached the emulator holds the live colors (which also
    // reflect any runtime OSC changes), so prefer those.
    if (emulator != null) {
      val c = emulator.mColors.mCurrentColors
      return intArrayOf(c[TextStyle.COLOR_INDEX_BACKGROUND], c[TextStyle.COLOR_INDEX_FOREGROUND])
    }
    // Before the session attaches — e.g. the keyboard is shown for the very
    // first input session — read the user's *configured* color scheme directly.
    // Without this the keyboard would otherwise get the built-in default until
    // the input connection is recreated (e.g. after a recents round-trip).
    configuredColors()?.let { return it }
    // Last resort: the static default scheme.
    val d = TerminalColors.COLOR_SCHEME.mDefaultColors
    return intArrayOf(d[TextStyle.COLOR_INDEX_BACKGROUND], d[TextStyle.COLOR_INDEX_FOREGROUND])
  }

  private fun configuredColors(): IntArray? {
    return try {
      val scheme = ComponentManager.getComponent<ColorSchemeComponent>().getCurrentColorScheme()
      Log.d(TAG, "configuredColors: scheme=${scheme.colorName} bg=${scheme.backgroundColor} fg=${scheme.foregroundColor}")
      val bg = scheme.backgroundColor?.let { TerminalColors.parse(it) } ?: return null
      val fg = scheme.foregroundColor?.let { TerminalColors.parse(it) } ?: return null
      intArrayOf(bg, fg)
    } catch (e: Exception) {
      Log.d(TAG, "configuredColors: failed: $e")
      null
    }
  }
}
