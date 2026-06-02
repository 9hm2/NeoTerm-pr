package io.neoterm.utils

import android.app.Activity
import android.graphics.Color
import android.graphics.drawable.ColorDrawable
import android.text.SpannableString
import android.text.Spanned
import android.text.style.ForegroundColorSpan
import android.view.View
import android.view.ViewGroup
import android.widget.ListView
import android.widget.TextView
import androidx.appcompat.app.ActionBar
import androidx.core.content.ContextCompat
import androidx.core.graphics.ColorUtils
import io.neoterm.R
import io.neoterm.backend.TerminalColors
import io.neoterm.component.ComponentManager
import io.neoterm.component.colorscheme.ColorSchemeComponent

/**
 * Themes non-terminal screens (settings, about, …) to match the terminal: the
 * window/bars get the terminal background color and the text gets the terminal
 * foreground color. The colors are read fresh from the active color scheme, so
 * they apply right after the first install without having to change the scheme.
 *
 * @author kiva
 */
object TerminalColorTheme {

  fun background(activity: Activity): Int = colorOf(activity, background = true)
  fun foreground(activity: Activity): Int = colorOf(activity, background = false)

  private fun colorOf(activity: Activity, background: Boolean): Int {
    return try {
      val scheme = ComponentManager.getComponent<ColorSchemeComponent>().getCurrentColorScheme()
      val hex = if (background) scheme.backgroundColor else scheme.foregroundColor
      TerminalColors.parse(hex ?: if (background) "#14181c" else "#ffffff")
    } catch (e: Exception) {
      if (background) ContextCompat.getColor(activity, R.color.terminal_background) else Color.WHITE
    }
  }

  /**
   * Apply the terminal background to the window + system bars, color the action
   * bar and its title, and recolor every text view (including the preference
   * list, reapplied on layout so recycled rows stay themed).
   */
  fun apply(activity: Activity, actionBar: ActionBar?, content: View?, listView: ListView?) {
    val bg = background(activity)
    val fg = foreground(activity)

    activity.window.setBackgroundDrawable(ColorDrawable(bg))
    activity.window.statusBarColor = bg
    activity.window.navigationBarColor = bg
    applyBarIconContrast(activity, bg)

    if (actionBar != null) {
      actionBar.setBackgroundDrawable(ColorDrawable(bg))
      val title = actionBar.title
      if (title != null) {
        actionBar.title = tinted(title, fg)
      }
    }

    content?.let { recolorText(it, fg) }

    listView?.let { lv ->
      lv.setBackgroundColor(bg)
      lv.cacheColorHint = bg
      recolorText(lv, fg)
    }
  }

  /** Recolor a [ListView]'s rows on every layout so scrolled-in rows stay themed. */
  fun keepListThemed(listView: ListView, foreground: Int) {
    listView.viewTreeObserver.addOnGlobalLayoutListener {
      recolorText(listView, foreground)
    }
  }

  private fun applyBarIconContrast(activity: Activity, bg: Int) {
    val lightBackground = ColorUtils.calculateLuminance(bg) > 0.5
    val lightFlags = View.SYSTEM_UI_FLAG_LIGHT_STATUS_BAR or View.SYSTEM_UI_FLAG_LIGHT_NAVIGATION_BAR
    var flags = activity.window.decorView.systemUiVisibility
    flags = if (lightBackground) flags or lightFlags else flags and lightFlags.inv()
    activity.window.decorView.systemUiVisibility = flags
  }

  private fun tinted(text: CharSequence, color: Int): CharSequence {
    val spannable = SpannableString(text)
    spannable.setSpan(ForegroundColorSpan(color), 0, spannable.length, Spanned.SPAN_INCLUSIVE_INCLUSIVE)
    return spannable
  }

  fun recolorText(view: View, foreground: Int) {
    when (view) {
      is TextView -> view.setTextColor(foreground)
      is ViewGroup -> for (i in 0 until view.childCount) recolorText(view.getChildAt(i), foreground)
    }
  }
}
