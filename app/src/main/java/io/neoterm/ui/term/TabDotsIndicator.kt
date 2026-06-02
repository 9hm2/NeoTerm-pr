package io.neoterm.ui.term

import android.content.Context
import android.graphics.Canvas
import android.graphics.Paint
import android.util.AttributeSet
import android.view.View
import androidx.core.graphics.ColorUtils
import kotlin.math.max
import kotlin.math.min

/**
 * A thin page-indicator strip drawn under the toolbar. Shows one dot per open
 * tab and highlights the active one, so it is always visible which tab we are
 * on (and matches the swipe-to-page gesture between tabs).
 *
 * @author kiva
 */
class TabDotsIndicator @JvmOverloads constructor(
  context: Context,
  attrs: AttributeSet? = null,
  defStyleAttr: Int = 0
) : View(context, attrs, defStyleAttr) {

  private val density = resources.displayMetrics.density
  private val dotRadius = 3f * density
  private val activeDotRadius = 4f * density
  private val preferredSpacing = 14f * density

  private val paint = Paint(Paint.ANTI_ALIAS_FLAG)

  private var count = 0
  private var selected = -1

  private var activeColor = 0xFFFFFFFF.toInt()
  private var inactiveColor = 0x66FFFFFF

  /** Updates the dot count and the selected index, hiding the strip for 0/1 tabs. */
  fun setTabs(count: Int, selected: Int) {
    this.count = count
    this.selected = selected
    // A single tab needs no indicator.
    visibility = if (count > 1) VISIBLE else GONE
    invalidate()
  }

  /**
   * Derive the dot colors from the terminal foreground color so they stay
   * legible on whatever background the toolbar currently uses.
   */
  fun setBaseColor(foreground: Int) {
    activeColor = foreground
    inactiveColor = ColorUtils.setAlphaComponent(foreground, 0x66)
    invalidate()
  }

  override fun onMeasure(widthMeasureSpec: Int, heightMeasureSpec: Int) {
    val height = (activeDotRadius * 2 + 8f * density).toInt()
    setMeasuredDimension(
      getDefaultSize(suggestedMinimumWidth, widthMeasureSpec),
      resolveSize(height, heightMeasureSpec)
    )
  }

  override fun onDraw(canvas: Canvas) {
    if (count <= 1) return

    val available = (width - paddingLeft - paddingRight).toFloat()
    // Shrink the spacing if there are too many tabs to fit at the preferred gap.
    val spacing = if (count > 1) {
      min(preferredSpacing, available / count)
    } else {
      preferredSpacing
    }
    val totalWidth = spacing * (count - 1)
    var cx = paddingLeft + (available - totalWidth) / 2f
    val cy = height / 2f

    for (i in 0 until count) {
      val isActive = i == selected
      paint.color = if (isActive) activeColor else inactiveColor
      val r = if (isActive) activeDotRadius else dotRadius
      canvas.drawCircle(cx, cy, max(1f, min(r, spacing / 2f)), paint)
      cx += spacing
    }
  }
}
