package io.neoterm.frontend.session.view;

import android.annotation.SuppressLint;
import android.annotation.TargetApi;
import android.content.ClipData;
import android.content.ClipboardManager;
import android.content.Context;
import android.content.Intent;
import android.graphics.Canvas;
import android.graphics.Rect;
import android.graphics.Typeface;
import android.graphics.drawable.BitmapDrawable;
import android.net.Uri;
import android.os.Build;
import android.text.Editable;
import android.text.InputType;
import android.text.TextUtils;
import android.util.AttributeSet;
import android.util.Log;
import android.view.*;
import android.view.accessibility.AccessibilityManager;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.Scroller;
import io.neoterm.R;
import io.neoterm.backend.*;
import io.neoterm.component.completion.OnAutoCompleteListener;


/**
 * View displaying and interacting with a {@link TerminalSession}.
 */
public final class TerminalView extends View {

  /**
   * Log view key and IME events.
   */
  private static final boolean LOG_KEY_EVENTS = false;

  /**
   * The background/foreground colors last advertised to the on-screen keyboard
   * (see {@link KeyboardThemeBridge}). Used to restart IME input only when the
   * terminal's colors actually change, so the keyboard tracks the live terminal.
   */
  private int mLastImeBg = 0, mLastImeFg = 0;

  /**
   * The currently displayed terminal session, whose emulator is {@link #mEmulator}.
   */
  TerminalSession mTermSession;

  /**
   * Our terminal emulator whose session is {@link #mTermSession}.
   */
  TerminalEmulator mEmulator;

  TerminalRenderer mRenderer;

  /**
   * Drives repaints off the display's frame clock (vsync, ~60 Hz or the panel's native rate)
   * rather than only when output arrives. After each emulator update the view keeps refreshing
   * for a short tail window, so the screen always converges to the latest buffer state within a
   * frame even if an individual repaint was coalesced away or suppressed (e.g. during DEC 2026
   * synchronized output) -- a stale frame can't get stuck. Idles when no updates arrive so it
   * doesn't burn power on a quiet terminal.
   */
  private boolean mViewAttached = false;
  private boolean mRefreshScheduled = false;
  private long mLastScreenUpdateUptime = 0;
  /** Keep ticking this long after the last emulator update, then idle until the next one. */
  private static final long REFRESH_TAIL_MILLIS = 250;
  private final Choreographer.FrameCallback mRefreshFrameCallback = new Choreographer.FrameCallback() {
    @Override
    public void doFrame(long frameTimeNanos) {
      if (!mViewAttached) {
        mRefreshScheduled = false;
        return;
      }
      // Animation callbacks run before this frame's draw pass, so this invalidate is served on
      // the same vsync (no extra latency).
      invalidate();
      if (android.os.SystemClock.uptimeMillis() - mLastScreenUpdateUptime < REFRESH_TAIL_MILLIS) {
        Choreographer.getInstance().postFrameCallback(this);
      } else {
        mRefreshScheduled = false;
      }
    }
  };

  /** Mark the screen dirty and ensure the vsync refresh loop is running. */
  private void scheduleRefresh() {
    mLastScreenUpdateUptime = android.os.SystemClock.uptimeMillis();
    if (mRefreshScheduled || !mViewAttached) return;
    mRefreshScheduled = true;
    Choreographer.getInstance().postFrameCallback(mRefreshFrameCallback);
  }

  @Override
  protected void onAttachedToWindow() {
    super.onAttachedToWindow();
    mViewAttached = true;
    restartCursorBlink();
  }

  @Override
  protected void onDetachedFromWindow() {
    mViewAttached = false;
    mRefreshScheduled = false;
    Choreographer.getInstance().removeFrameCallback(mRefreshFrameCallback);
    removeCallbacks(mCursorBlinkRunnable);
    super.onDetachedFromWindow();
  }

  @Override
  protected void onFocusChanged(boolean gainFocus, int direction, android.graphics.Rect previouslyFocusedRect) {
    super.onFocusChanged(gainFocus, direction, previouslyFocusedRect);
    // Cursor only blinks while focused; show it solid otherwise (like a typical terminal).
    mCursorBlinkOn = true;
    restartCursorBlink();
    invalidate();
  }

  /** Time the cursor stays on / off while blinking. */
  private static final long CURSOR_BLINK_MS = 500;
  /** Current blink phase: when false the cursor is hidden for this half-cycle. */
  private boolean mCursorBlinkOn = true;
  private final Runnable mCursorBlinkRunnable = new Runnable() {
    @Override
    public void run() {
      if (!mViewAttached || mEmulator == null) return;
      // Blink only when focused, the cursor is shown, and DECSCUSR asked for a blinking style.
      boolean blink = isFocused() && mEmulator.isShowingCursor() && mEmulator.isCursorBlinkingEnabled();
      if (blink) {
        mCursorBlinkOn = !mCursorBlinkOn;
        invalidate();
      } else if (!mCursorBlinkOn) {
        mCursorBlinkOn = true;
        invalidate();
      }
      postDelayed(this, CURSOR_BLINK_MS);
    }
  };

  /** Reset the cursor to solid and restart the blink timer (e.g. on focus or output activity). */
  private void restartCursorBlink() {
    removeCallbacks(mCursorBlinkRunnable);
    mCursorBlinkOn = true;
    if (mViewAttached) postDelayed(mCursorBlinkRunnable, CURSOR_BLINK_MS);
  }

  TerminalViewClient mClient;

  /**
   * Vertical pan, in pixels, applied so the soft keyboard does not force a PTY resize. When the
   * keyboard covers the bottom of the view we keep the terminal's row/column count fixed (see
   * {@link #updateSize}) and instead shift the rendered content up by this amount, so the cursor
   * and bottom rows stay visible above the keyboard. This avoids reflowing the screen on every
   * keyboard toggle, which strands the bottom UI of main-buffer apps (e.g. a CLI status box).
   */
  private int mKeyboardPanPx = 0;

  /**
   * The top row of text to display. Ranges from -activeTranscriptRows to 0.
   */
  int mTopRow;

  boolean mIsSelectingText = false, mIsDraggingLeftSelection, mInitialTextSelection;
  /** Per-gesture latch for the pager-vs-terminal touch decision: once a drag is
   *  classified (horizontal -> let the ViewPager2 page; vertical -> terminal
   *  scroll) we stick with it. Reset on ACTION_DOWN. */
  private boolean mGestureDecided = false;
  private boolean mGestureHorizontal = false;
  /** True while the current touch gesture is dragging a selection handle. */
  private boolean mIsDraggingHandle = false;
  int mSelX1 = -1, mSelX2 = -1, mSelY1 = -1, mSelY2 = -1;
  float mSelectionDownX, mSelectionDownY;

  /**
   * Auto-scroll while dragging a selection handle near (or past) the top/bottom
   * edge. The number of rows scrolled per tick (signed: negative = up into the
   * scrollback) grows with how far past the edge the finger is, so the further
   * you drag the faster the terminal scrolls and the selection extends.
   */
  private int mSelectionAutoScrollRows = 0;
  private static final long SELECTION_AUTO_SCROLL_INTERVAL_MS = 16;
  private static final int SELECTION_AUTO_SCROLL_MAX_ROWS = 12;

  private final Runnable mSelectionAutoScrollRunnable = new Runnable() {
    @Override
    public void run() {
      if (!mIsSelectingText || mSelectionAutoScrollRows == 0 || mEmulator == null) {
        return;
      }
      int before = mTopRow;
      int minTopRow = -mEmulator.getScreen().getActiveTranscriptRows();
      mTopRow = Math.min(0, Math.max(minTopRow, mTopRow + mSelectionAutoScrollRows));
      int scrolled = mTopRow - before;
      if (scrolled == 0) {
        // Reached the top/bottom of the transcript: stop until the finger moves.
        mSelectionAutoScrollRows = 0;
        return;
      }
      // Keep the dragged handle pinned to the edge so the selection extends
      // into the freshly revealed rows.
      if (mIsDraggingLeftSelection) {
        mSelY1 += scrolled;
      } else {
        mSelY2 += scrolled;
      }
      clampAndSwapSelection();
      if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && mActionMode != null) {
        mActionMode.invalidateContentRect();
      }
      invalidate();
      postDelayed(this, SELECTION_AUTO_SCROLL_INTERVAL_MS);
    }
  };

  private ActionMode mActionMode;
  private BitmapDrawable mLeftSelectionHandle, mRightSelectionHandle;
  /** Tracks finger velocity for inertial fling when scrolling during selection. */
  private VelocityTracker mSelectionVelocityTracker;

  float mScaleFactor = 1.f;
  /* final */ GestureAndScaleRecognizer mGestureRecognizer;

  // Horizontal tab paging is now owned by the enclosing ViewPager2 (so adjacent
  // tabs are live during the swipe). The TerminalView no longer translates
  // itself sideways; instead it tells the pager to keep its hands off vertical
  // scrolls / text selection / pinch via requestDisallowInterceptTouchEvent (see
  // onTouchEvent), and lets clearly-horizontal drags fall through to the pager.

  /**
   * Keep track of where mouse touch event started which we report as mouse scroll.
   */
  private int mMouseScrollStartX = -1, mMouseScrollStartY = -1;
  /**
   * Keep track of the time when a touch event leading to sending mouse scroll events started.
   */
  private long mMouseStartDownTime = -1;

  /* final */ Scroller mScroller;

  /**
   * What was left in from scrolling movement.
   */
  float mScrollRemainder;

  /**
   * If non-zero, this is the last unicode code point received if that was a combining character.
   */
  int mCombiningAccent;
  int mTextSize;

  /**
   * If true, IME will be word based instead of char based.
   */
  private boolean mEnableWordBasedIme = false;

  private boolean mAccessibilityEnabled;

  public TerminalView(Context context) {
    super(context);
    commonInit(context);
  }

  public TerminalView(Context context, AttributeSet attributeSet) { // NO_UCD (unused code)
    super(context, attributeSet);
    commonInit(context);
  }

  /** Loaded once: the bundled broad-coverage symbol font used to draw glyphs the
   *  user font (and the system's subsetted fallback) lack. {@code null} until the
   *  first load attempt; the load is attempted only once. */
  private static Typeface sFallbackTypeface;
  private static boolean sFallbackLoaded = false;

  /**
   * Load assets/symbol_fallback.ttf (a subset of GNU Unifont covering the symbol,
   * technical, box-drawing, arrow and dingbat blocks) once and hand it to
   * {@link TerminalRenderer} as the tofu fallback. Android's own MONOSPACE
   * fallback chain uses subsetted Noto symbol fonts that omit many of these
   * glyphs, so without this a TUI's symbols (e.g. Claude's "⏵⏵" auto-accept
   * marker) render as empty boxes.
   */
  private static void ensureFallbackTypeface(Context context) {
    if (sFallbackLoaded) return;
    sFallbackLoaded = true;
    try {
      sFallbackTypeface = Typeface.createFromAsset(context.getApplicationContext().getAssets(), "symbol_fallback.ttf");
      TerminalRenderer.setFallbackTypeface(sFallbackTypeface);
    } catch (Exception e) {
      Log.w("NeoTerm", "Could not load symbol fallback font, using system MONOSPACE", e);
    }
  }

  private void commonInit(Context context) {
    ensureFallbackTypeface(context);
    // Don't draw the platform's default focus highlight: when the terminal gains focus in
    // non-touch mode (e.g. right after an Enter key press, which switches the view tree out of
    // touch mode), Android would otherwise overlay a translucent highlight across the whole view,
    // making it look "lit up"/selected. The terminal draws its own cursor, so the system highlight
    // is unwanted. (minSdk 26 -> setDefaultFocusHighlightEnabled is always available.)
    setDefaultFocusHighlightEnabled(false);
    mGestureRecognizer = new GestureAndScaleRecognizer(context, new GestureAndScaleRecognizer.Listener() {

      private boolean scrolledWithFinger;

      // For treating double tap as MOUSE_LEFT_BUTTON_MOVED event
      // e.g in vim, we can change window size with fingers moving.
      private float doubleTapX, doubleTapY;
      private boolean draggedAfterDoubleTap;

      @Override
      public boolean onUp(MotionEvent e) {
        mScrollRemainder = 0.0f;
        // 只有在没有选中文字的时候可以发送鼠标事件： !isSelectingText
        if (mEmulator != null && mEmulator.isMouseTrackingActive() && !mIsSelectingText && !scrolledWithFinger) {
          // Quick event processing when mouse tracking is active - do not wait for check of double tapping
          // for zooming.
          sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON, true);
          sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON, false);
          return true;
        }
        scrolledWithFinger = false;
        return false;
      }

      @Override
      public boolean onSingleTapUp(MotionEvent e) {
        if (mEmulator == null) return true;
        if (mIsSelectingText) {
          toggleSelectingText(null);
          return true;
        }
        // Tapping a URL / path / git-hash offers a quick action instead of
        // raising the keyboard.
        if (!mEmulator.isMouseTrackingActive() && !e.isFromSource(InputDevice.SOURCE_MOUSE)
          && handleTokenTap(e)) {
          return true;
        }
        requestFocus();
        if (!mEmulator.isMouseTrackingActive()) {
          if (!e.isFromSource(InputDevice.SOURCE_MOUSE)) {
            mClient.onSingleTapUp(e);
            return true;
          }
        }
        return false;
      }

      @Override
      public boolean onScroll(MotionEvent e, float distanceX, float distanceY) {
        // While selecting, scrolling is driven manually from onTouchEvent (the
        // gesture detector suppresses onScroll right after the long-press that
        // started the selection), so skip it here.
        if (mEmulator == null || mIsSelectingText) return true;

        if (mEmulator.isMouseTrackingActive() && e.isFromSource(InputDevice.SOURCE_MOUSE)) {
          // If moving with mouse pointer while pressing button, report that instead of scroll.
          // This means that we never report moving with button press-events for touch input,
          // since we cannot just start sending these events without a starting press event,
          // which we do not do for touch input, only mouse in onTouchEvent().
          sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON_MOVED, true);
          return true;
        }

        // Classify the drag once: a clearly-horizontal drag is handed to the
        // enclosing ViewPager2 (page tabs); anything else (vertical) stays with
        // the terminal. We already disallowed parent intercept on ACTION_DOWN, so
        // long-press/selection/pinch are safe; here we only RELEASE to the pager
        // for a horizontal drag.
        if (!mGestureDecided) {
          mGestureDecided = true;
          mGestureHorizontal = Math.abs(distanceX) > Math.abs(distanceY);
          if (mGestureHorizontal) allowParentIntercept();
        }
        if (mGestureHorizontal) {
          // The pager owns this gesture now (it will intercept the next move).
          return true;
        }

        disallowParentIntercept();
        scrolledWithFinger = true;
        scrollByPixels(e, distanceY);
        return true;
      }

      @Override
      public boolean onScale(float focusX, float focusY, float scale) {
        if (mEmulator == null || mIsSelectingText) return true;
        // A pinch is a two-finger terminal gesture (font zoom): keep the pager
        // from hijacking it as a horizontal page swipe.
        disallowParentIntercept();
        mScaleFactor *= scale;
        // 这里一般是改变文字大小
        mScaleFactor = mClient.onScale(mScaleFactor);
        return true;
      }

      @Override
      public boolean onFling(final MotionEvent e2, float velocityX, float velocityY) {
        // While selecting, flinging is driven manually from onTouchEvent.
        if (mEmulator == null || mIsSelectingText) return true;

        startFling(e2, velocityY);
        return true;
      }

      @Override
      public boolean onDown(float x, float y) {
        return false;
      }

      @Override
      public boolean onDoubleTap(MotionEvent e) {
        // Old behavior: Do not treat is as a single confirmed tap - it may be followed by zoom.

        // For treating double tap as MOUSE_LEFT_BUTTON_MOVED event
        // e.g in vim, we can change window size with fingers moving.
        // Now double tap and drag has been treated as a MOUSE_LEFT_BUTTON_MOVED event.
        return true;
      }

      // For treating double tap as MOUSE_LEFT_BUTTON_MOVED event
      // e.g in vim, we can change window size with fingers moving.
      @Override
      public boolean onDoubleTapEvent(MotionEvent e) {
        if (mEmulator.isMouseTrackingActive() && !e.isFromSource(InputDevice.SOURCE_MOUSE)) {
          switch (e.getAction()) {
            case MotionEvent.ACTION_DOWN:
              doubleTapX = e.getX();
              doubleTapY = e.getY();
              draggedAfterDoubleTap = false;
              sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON, true);
              break;
            case MotionEvent.ACTION_UP:
              if (!draggedAfterDoubleTap) {
                sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON, false);
                sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON, true);
              }
              sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON, false);
              break;
            case MotionEvent.ACTION_MOVE:
              if (Math.abs(e.getX() - doubleTapX) >= mRenderer.mFontWidth
                || Math.abs(e.getY() - doubleTapY) >= mRenderer.mFontLineSpacing) {
                doubleTapX = e.getX();
                doubleTapY = e.getY();
                draggedAfterDoubleTap = true;
                sendMouseEventCode(e, TerminalEmulator.MOUSE_LEFT_BUTTON_MOVED, true);
              }
              break;
          }
        }
        return true;
      }

      @Override
      public void onLongPress(MotionEvent e) {
        if (mGestureRecognizer.isInProgress()) return;
        if (mClient.onLongPress(e)) return;
        if (!mIsSelectingText) {
          performHapticFeedback(HapticFeedbackConstants.LONG_PRESS);
          toggleSelectingText(e);
        }
      }
    });
    mScroller = new Scroller(context);
    AccessibilityManager am = (AccessibilityManager) context.getSystemService(Context.ACCESSIBILITY_SERVICE);
    mAccessibilityEnabled = am != null && am.isEnabled();
  }

  /**
   * @param client Listener for all kinds of key events, both hardware and IME (which makes it different from that
   *               available with {@link View#setOnKeyListener(OnKeyListener)}.
   */
  public void setTerminalViewClient(TerminalViewClient client) {
    this.mClient = client;
  }

  @Override
  public void setOnKeyListener(OnKeyListener l) {
    if (l instanceof TerminalViewClient) {
      setTerminalViewClient(((TerminalViewClient) l));
    }
  }

  /**
   * Attach a {@link TerminalSession} to this view.
   *
   * @param session The {@link TerminalSession} this view will be displaying.
   */
  public boolean attachSession(TerminalSession session) {
    if (session == mTermSession) return false;
    mTopRow = 0;

    mTermSession = session;
    mEmulator = null;
    mCombiningAccent = 0;

    updateSize();

    // Wait with enabling the scrollbar until we have a terminal to get scroll position from.
    setVerticalScrollBarEnabled(true);

    return true;
  }

  /**
   * Called when the terminal's colors change (OSC escape, color scheme applied,
   * reset, …). If the background or foreground actually changed from what we
   * last handed the on-screen keyboard, restart IME input so the keyboard
   * re-reads the new colors and keeps matching the live terminal. May be called
   * off the UI thread, so the restart is posted.
   */
  public void onTerminalColorsChanged() {
    if (mEmulator == null) return;
    int[] colors = mEmulator.mColors.mCurrentColors;
    final int bg = colors[TextStyle.COLOR_INDEX_BACKGROUND];
    final int fg = colors[TextStyle.COLOR_INDEX_FOREGROUND];
    if (bg == mLastImeBg && fg == mLastImeFg) return;
    post(() -> {
      InputMethodManager imm =
        (InputMethodManager) getContext().getSystemService(Context.INPUT_METHOD_SERVICE);
      if (imm != null) imm.restartInput(this);
    });
  }

  @Override
  public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
    // Using InputType.NULL is the most correct input type and avoids issues with other hacks.
    //
    // Previous keyboard issues:
    // https://github.com/termux/termux-packages/issues/25
    // https://github.com/termux/termux-app/issues/87.
    // https://github.com/termux/termux-app/issues/126.
    // https://github.com/termux/termux-app/issues/137 (japanese chars and TYPE_NULL).
    if (mEnableWordBasedIme) {
      // Workaround for Google Pinying cannot input Chinese
      outAttrs.inputType = InputType.TYPE_CLASS_TEXT;
    } else {
      outAttrs.inputType = InputType.TYPE_NULL;
    }

    // Note that IME_ACTION_NONE cannot be used as that makes it impossible to input newlines using the on-screen
    // keyboard on Android TV (see https://github.com/termux/termux-app/issues/221).
    outAttrs.imeOptions = EditorInfo.IME_FLAG_NO_FULLSCREEN;

    // Advertise the terminal colors to a co-operating on-screen keyboard
    // (pcKeyboard) so it can theme itself to match the terminal. The keyboard
    // is themed from the very first input session even before the emulator has
    // attached, because the bridge falls back to the user's configured color
    // scheme. See KeyboardThemeBridge.
    int[] advertised = KeyboardThemeBridge.applyTo(outAttrs, mEmulator);
    if (advertised != null) {
      mLastImeBg = advertised[0];
      mLastImeFg = advertised[1];
    }

    return new BaseInputConnection(this, true) {
      @Override
      public boolean finishComposingText() {
        if (LOG_KEY_EVENTS) Log.i(EmulatorDebug.LOG_TAG, "IME: finishComposingText()");
        super.finishComposingText();

        sendTextToTerminal(getEditable());
        getEditable().clear();
        return true;
      }

      @Override
      public boolean commitText(CharSequence text, int newCursorPosition) {
        if (LOG_KEY_EVENTS) {
          Log.i(EmulatorDebug.LOG_TAG, "IME: commitText(\"" + text + "\", " + newCursorPosition + ")");
        }
        super.commitText(text, newCursorPosition);

        if (mEmulator == null) return true;

        Editable content = getEditable();
        sendTextToTerminal(content);
        if (onAutoCompleteListener != null) {
          onAutoCompleteListener.onCompletionRequired(content.toString());
        }
        content.clear();
        return true;
      }

      @Override
      public boolean deleteSurroundingText(int leftLength, int rightLength) {
        if (LOG_KEY_EVENTS) {
          Log.i(EmulatorDebug.LOG_TAG, "IME: deleteSurroundingText(" + leftLength + ", " + rightLength + ")");
        }
        // The stock Samsung keyboard with 'Auto check spelling' enabled sends leftLength > 1.
        KeyEvent deleteKey = new KeyEvent(KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_DEL);
        for (int i = 0; i < leftLength; i++) sendKeyEvent(deleteKey);
        return super.deleteSurroundingText(leftLength, rightLength);
      }

      void sendTextToTerminal(CharSequence text) {
        final int textLengthInChars = text.length();
        for (int i = 0; i < textLengthInChars; i++) {
          char firstChar = text.charAt(i);
          int codePoint;
          if (Character.isHighSurrogate(firstChar)) {
            if (++i < textLengthInChars) {
              codePoint = Character.toCodePoint(firstChar, text.charAt(i));
            } else {
              // At end of string, with no low surrogate following the high:
              codePoint = TerminalEmulator.UNICODE_REPLACEMENT_CHAR;
            }
          } else {
            codePoint = firstChar;
          }

          boolean ctrlHeld = false;
          if (codePoint <= 31 && codePoint != 27) {
            if (codePoint == '\n') {
              // The AOSP keyboard and descendants seems to send \n as text when the enter key is pressed,
              // instead of a key event like most other keyboard apps. A terminal expects \r for the enter
              // key (although when icrnl is enabled this doesn't make a difference - run 'stty -icrnl' to
              // check the behaviour).
              codePoint = '\r';
            }

            // E.g. penti keyboard for ctrl input.
            ctrlHeld = true;
            switch (codePoint) {
              case 31:
                codePoint = '_';
                break;
              case 30:
                codePoint = '^';
                break;
              case 29:
                codePoint = ']';
                break;
              case 28:
                codePoint = '\\';
                break;
              default:
                codePoint += 96;
                break;
            }
          }

          inputCodePoint(codePoint, ctrlHeld, false);
        }
      }

    };
  }

  @Override
  protected int computeVerticalScrollRange() {
    return mEmulator == null ? 1 : mEmulator.getScreen().getActiveRows();
  }

  @Override
  protected int computeVerticalScrollExtent() {
    return mEmulator == null ? 1 : mEmulator.mRows;
  }

  @Override
  protected int computeVerticalScrollOffset() {
    return mEmulator == null ? 1 : mEmulator.getScreen().getActiveRows() + mTopRow - mEmulator.mRows;
  }

  public void onScreenUpdated() {
    if (mEmulator == null) return;
    boolean skipScrolling = false;
    boolean isScreenHeld = false;

    // currentScroll 记录了当前滚动到的位置
    // expectedScroll 记录了假设一直跟随输出滚动在最底部时的滚动位置
    // 如果二者不一样，即 mTop != 0，则说明用户在脚本输出的时候滚动了屏幕
    // 很有可能时用户需要观察上面脚本的输出结果
    // 那么这个时候我们就不跟随输出滚动屏幕
    // int currentScroll = computeVerticalScrollOffset();
    // int expectedScroll = mEmulator.getScreen().getActiveRows() - mEmulator.mRows;

    if (mTopRow != 0) {
      isScreenHeld = true;
    }

    if (mIsSelectingText || isScreenHeld) {
      // Do not scroll when selecting text.
      int rowsInHistory = mEmulator.getScreen().getActiveTranscriptRows();
      int rowShift = mEmulator.getScrollCounter();
      if (-mTopRow + rowShift > rowsInHistory) {
        // .. unless we're hitting the end of history transcript, in which
        // case we abort text selection and scroll to end.

        // 只当是因为选择文字而停止滚动时才取消选择文字
        if (mIsSelectingText) {
          toggleSelectingText(null);
        }
      } else {
        skipScrolling = true;
        mTopRow -= rowShift;
        mSelY1 -= rowShift;
        mSelY2 -= rowShift;
      }

      // 不滚动屏幕，但要让滚动条显示来告诉用户脚本在输出
      if (isScreenHeld) {
        awakenScrollBars();
      }
    }

    if (!skipScrolling && mTopRow != 0) {
      // Scroll down if not already there.
      if (mTopRow < -3) {
        // Awaken scroll bars only if scrolling a noticeable amount
        // - we do not want visible scroll bars during normal typing
        // of one row at a time.
        awakenScrollBars();
      }
      mTopRow = 0;
    }

    mEmulator.clearScrollCounter();
    scheduleRefresh();
    // Keep the cursor solid while output is flowing; it resumes blinking once things go idle.
    restartCursorBlink();

    // Basic accessibility service
    String contentText = mEmulator.getScreen()
      .getSelectedText(0, mTopRow, mEmulator.mColumns, mTopRow + mEmulator.mRows);
    if (mAccessibilityEnabled) {
      setContentDescription(contentText);
    }
  }

  public int getTextSize() {
    return mTextSize;
  }

  /**
   * Sets the text size, which in turn sets the number of rows and columns.
   *
   * @param textSize the new font size, in density-independent pixels.
   */
  public void setTextSize(int textSize) {
    this.mTextSize = textSize;
    mRenderer = new TerminalRenderer(textSize, mRenderer == null ? Typeface.MONOSPACE : mRenderer.mTypeface);
    updateSize();
  }

  public void setTypeface(Typeface newTypeface) {
    mRenderer = new TerminalRenderer(mRenderer.mTextSize, newTypeface);
    updateSize();
    invalidate();
  }

  @Override
  public boolean onCheckIsTextEditor() {
    return true;
  }

  @Override
  public boolean isOpaque() {
    return true;
  }

  /**
   * Send a single mouse event code to the terminal.
   */
  void sendMouseEventCode(MotionEvent e, int button, boolean pressed) {
    int x = (int) (e.getX() / mRenderer.mFontWidth) + 1;
    int y = (int) ((e.getY() - mRenderer.mFontLineSpacingAndAscent) / mRenderer.mFontLineSpacing) + 1;
    if (pressed && (button == TerminalEmulator.MOUSE_WHEELDOWN_BUTTON || button == TerminalEmulator.MOUSE_WHEELUP_BUTTON)) {
      if (mMouseStartDownTime == e.getDownTime()) {
        x = mMouseScrollStartX;
        y = mMouseScrollStartY;
      } else {
        mMouseStartDownTime = e.getDownTime();
        mMouseScrollStartX = x;
        mMouseScrollStartY = y;
      }
    }
    mEmulator.sendMouseEvent(button, x, y, pressed);
  }

  /**
   * Perform a scroll, either from dragging the screen or by scrolling a mouse wheel.
   */
  /**
   * Scroll the terminal by a pixel delta, line-quantized with a sub-line
   * remainder (the exact dynamics used by the normal {@code onScroll}). Used by
   * both the gesture recognizer and the in-selection finger scroll so they feel
   * identical.
   */
  void scrollByPixels(MotionEvent event, float distanceY) {
    distanceY += mScrollRemainder;
    int deltaRows = (int) (distanceY / mRenderer.mFontLineSpacing);
    mScrollRemainder = distanceY - deltaRows * mRenderer.mFontLineSpacing;
    doScroll(event, deltaRows);
    // Keep the selection toolbar anchored to the (content-anchored) selection.
    if (mIsSelectingText && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && mActionMode != null) {
      mActionMode.invalidateContentRect();
    }
  }

  /**
   * Start an inertial fling scroll (the same momentum used by the normal
   * {@code onFling}), so releasing a flick keeps scrolling — including while a
   * text selection is active.
   */
  void startFling(final MotionEvent e2, float velocityY) {
    // Do not start scrolling until last fling has been taken care of:
    if (!mScroller.isFinished()) return;

    final boolean mouseTrackingAtStartOfFling = mEmulator.isMouseTrackingActive();
    float SCALE = 0.25f;
    if (mouseTrackingAtStartOfFling) {
      mScroller.fling(0, 0, 0, -(int) (velocityY * SCALE), 0, 0, -mEmulator.mRows / 2, mEmulator.mRows / 2);
    } else {
      mScroller.fling(0, mTopRow, 0, -(int) (velocityY * SCALE), 0, 0, -mEmulator.getScreen().getActiveTranscriptRows(), 0);
    }

    post(new Runnable() {
      private int mLastY = 0;

      @Override
      public void run() {
        if (mouseTrackingAtStartOfFling != mEmulator.isMouseTrackingActive()) {
          mScroller.abortAnimation();
          return;
        }
        if (mScroller.isFinished()) return;
        boolean more = mScroller.computeScrollOffset();
        int newY = mScroller.getCurrY();
        int diff = mouseTrackingAtStartOfFling ? (newY - mLastY) : (newY - mTopRow);
        doScroll(e2, diff);
        if (mIsSelectingText && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && mActionMode != null) {
          mActionMode.invalidateContentRect();
        }
        mLastY = newY;
        if (more) post(this);
      }
    });
  }

  void doScroll(MotionEvent event, int rowsDown) {
    boolean up = rowsDown < 0;
    int amount = Math.abs(rowsDown);
    for (int i = 0; i < amount; i++) {
      if (mEmulator.isMouseTrackingActive()) {
        sendMouseEventCode(event, up ? TerminalEmulator.MOUSE_WHEELUP_BUTTON : TerminalEmulator.MOUSE_WHEELDOWN_BUTTON, true);
      } else if (mEmulator.isAlternateBufferActive()) {
        // Send up and down key events for scrolling, which is what some terminals do to make scroll work in
        // e.g. less, which shifts to the alt screen without mouse handling.
        handleKeyCode(up ? KeyEvent.KEYCODE_DPAD_UP : KeyEvent.KEYCODE_DPAD_DOWN, 0);
      } else {
        mTopRow = Math.min(0, Math.max(-(mEmulator.getScreen().getActiveTranscriptRows()), mTopRow + (up ? -1 : 1)));
        if (!awakenScrollBars()) invalidate();
      }
    }
  }

  /**
   * Overriding {@link View#onGenericMotionEvent(MotionEvent)}.
   */
  @Override
  public boolean onGenericMotionEvent(MotionEvent event) {
    if (mEmulator != null && event.isFromSource(InputDevice.SOURCE_MOUSE) && event.getAction() == MotionEvent.ACTION_SCROLL) {
      // Handle mouse wheel scrolling.
      boolean up = event.getAxisValue(MotionEvent.AXIS_VSCROLL) > 0.0f;
      doScroll(event, up ? -3 : 3);
      return true;
    }
    return false;
  }

  @SuppressLint("ClickableViewAccessibility")
  @Override
  @TargetApi(23)
  public boolean onTouchEvent(MotionEvent ev) {
    if (mEmulator == null) return true;
    // Map touch coordinates into the (keyboard-)panned content space, so taps/selection land on
    // the same rows the renderer drew. A single offset here covers every downstream getY() use
    // (gesture detector, selection, hit-testing).
    if (mKeyboardPanPx != 0) ev.offsetLocation(0, mKeyboardPanPx);
    final int action = ev.getAction();

    // Claim the gesture up front (touch only) so a long-press/selection/vertical
    // scroll/pinch can't be stolen by the enclosing ViewPager2 on a tiny finger
    // wobble. The first onScroll releases it back to the pager only for a
    // clearly-horizontal drag (mGestureHorizontal).
    if (action == MotionEvent.ACTION_DOWN && !ev.isFromSource(InputDevice.SOURCE_MOUSE)) {
      mGestureDecided = false;
      mGestureHorizontal = false;
      disallowParentIntercept();
    }

    if (mIsSelectingText) {
      // The whole gesture belongs to text selection / its scroll: keep the pager
      // from intercepting any part of it as a page swipe.
      disallowParentIntercept();
      switch (action) {
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_CANCEL:
          mInitialTextSelection = false;
          // If the gesture was a (non-handle) scroll, give it inertia just like
          // a normal scroll so the dynamics match.
          if (!mIsDraggingHandle && action == MotionEvent.ACTION_UP && mSelectionVelocityTracker != null) {
            mSelectionVelocityTracker.computeCurrentVelocity(1000);
            float velocityY = mSelectionVelocityTracker.getYVelocity();
            if (Math.abs(velocityY) > dpToPx(50)) {
              startFling(ev, velocityY);
            }
          }
          if (mSelectionVelocityTracker != null) {
            mSelectionVelocityTracker.recycle();
            mSelectionVelocityTracker = null;
          }
          mIsDraggingHandle = false;
          stopSelectionAutoScroll();
          break;
        case MotionEvent.ACTION_DOWN:
          // Only grab a handle if the touch actually lands on (near) one.
          // Otherwise the gesture is a scroll, so we don't move the selection.
          int hit = handleHitTest(ev.getX(), ev.getY());
          mIsDraggingHandle = hit != 0;
          if (hit == 1) {
            mIsDraggingLeftSelection = true;
          } else if (hit == 2) {
            mIsDraggingLeftSelection = false;
          }
          mSelectionDownX = ev.getX();
          mSelectionDownY = ev.getY();
          stopSelectionAutoScroll();
          if (mSelectionVelocityTracker != null) mSelectionVelocityTracker.recycle();
          mSelectionVelocityTracker = VelocityTracker.obtain();
          mSelectionVelocityTracker.addMovement(ev);
          break;
        case MotionEvent.ACTION_MOVE:
          if (mInitialTextSelection) break;

          if (!mIsDraggingHandle) {
            // Not on a handle: scroll the terminal through the exact same path
            // (pixel-quantized + remainder, and inertia on release) as a normal
            // non-selecting scroll, so the dynamics match. The selection stays
            // anchored to its text (its rows are absolute, so changing mTopRow
            // moves it with the content).
            if (mSelectionVelocityTracker != null) mSelectionVelocityTracker.addMovement(ev);
            float distanceY = mSelectionDownY - ev.getY();
            mSelectionDownY = ev.getY();
            scrollByPixels(ev, distanceY);
            break;
          }

          float deltaX = ev.getX() - mSelectionDownX;
          float deltaY = ev.getY() - mSelectionDownY;
          int deltaCols = (int) Math.ceil(deltaX / mRenderer.mFontWidth);
          int deltaRows = (int) Math.ceil(deltaY / mRenderer.mFontLineSpacing);
          mSelectionDownX += deltaCols * mRenderer.mFontWidth;
          mSelectionDownY += deltaRows * mRenderer.mFontLineSpacing;
          if (mIsDraggingLeftSelection) {
            mSelX1 += deltaCols;
            mSelY1 += deltaRows;
          } else {
            mSelX2 += deltaCols;
            mSelY2 += deltaRows;
          }

          clampAndSwapSelection();

          if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.M)
            mActionMode.invalidateContentRect();
          invalidate();

          // Drag near/past the top or bottom edge to auto-scroll the terminal
          // while selecting; speed grows with the distance past the edge.
          updateSelectionAutoScroll(ev.getY());
          break;
        default:
          break;
      }
      mGestureRecognizer.onTouchEvent(ev);
      return true;
    } else if (ev.isFromSource(InputDevice.SOURCE_MOUSE)) {
      if (ev.isButtonPressed(MotionEvent.BUTTON_SECONDARY)) {
        if (action == MotionEvent.ACTION_DOWN) showContextMenu();
        return true;
      } else if (ev.isButtonPressed(MotionEvent.BUTTON_TERTIARY)) {
        pasteFromClipboard();
      } else if (mEmulator.isMouseTrackingActive()) { // BUTTON_PRIMARY.
        switch (ev.getAction()) {
          case MotionEvent.ACTION_DOWN:
          case MotionEvent.ACTION_UP:
            sendMouseEventCode(ev, TerminalEmulator.MOUSE_LEFT_BUTTON, ev.getAction() == MotionEvent.ACTION_DOWN);
            break;
          case MotionEvent.ACTION_MOVE:
            sendMouseEventCode(ev, TerminalEmulator.MOUSE_LEFT_BUTTON_MOVED, true);
            break;
        }
        return true;
      }
    }

    mGestureRecognizer.onTouchEvent(ev);
    return true;
  }

  /**
   * Ask the enclosing scrolling container (the ViewPager2's internal
   * RecyclerView) not to intercept the rest of this gesture, so a vertical
   * terminal scroll / text selection / pinch is not stolen and turned into a
   * horizontal page swipe. A clearly-horizontal drag never calls this, so the
   * pager is still free to page between live tabs.
   */
  private void disallowParentIntercept() {
    ViewParent parent = getParent();
    if (parent != null) parent.requestDisallowInterceptTouchEvent(true);
  }

  /** Hand the gesture back to the enclosing ViewPager2 (used once a drag is
   *  classified as a horizontal page swipe). */
  private void allowParentIntercept() {
    ViewParent parent = getParent();
    if (parent != null) parent.requestDisallowInterceptTouchEvent(false);
  }

  /**
   * Hit-test the two selection handles against a touch point (in view pixels).
   * The handle's drawn bitmap rect is expanded by a finger-sized grab slop so
   * the handles are easy to grab, but a touch away from both returns 0 so the
   * gesture is treated as a scroll instead of moving the selection.
   *
   * @return 1 for the left handle, 2 for the right handle, 0 for neither.
   */
  private int handleHitTest(float x, float y) {
    if (mLeftSelectionHandle == null || mRightSelectionHandle == null) return 0;

    final int w = mLeftSelectionHandle.getIntrinsicWidth();
    final int h = mLeftSelectionHandle.getIntrinsicHeight();
    final int margin = w / 4; // See the png; matches onDraw().
    // Grab slop: enlarge the touch target to at least ~24dp around the handle.
    final int slop = (int) Math.max(w / 2.0f, dpToPx(24));

    // Left handle hangs below-left of the selection start.
    int lRight = Math.round(mSelX1 * mRenderer.mFontWidth) + margin;
    int lLeft = lRight - w;
    int lTop = (mSelY1 + 1 - mTopRow) * mRenderer.mFontLineSpacing + mRenderer.mFontLineSpacingAndAscent;
    int lBottom = lTop + h;
    boolean inLeft = x >= (lLeft - slop) && x <= (lRight + slop)
      && y >= (lTop - slop) && y <= (lBottom + slop);

    // Right handle hangs below-right of the selection end.
    int rLeft = Math.round((mSelX2 + 1) * mRenderer.mFontWidth) - margin;
    int rRight = rLeft + w;
    int rTop = (mSelY2 + 1 - mTopRow) * mRenderer.mFontLineSpacing + mRenderer.mFontLineSpacingAndAscent;
    int rBottom = rTop + h;
    boolean inRight = x >= (rLeft - slop) && x <= (rRight + slop)
      && y >= (rTop - slop) && y <= (rBottom + slop);

    if (inLeft && inRight) {
      // Both in range (selection is tiny): pick the nearer handle center.
      double dl = Math.hypot(x - (lLeft + lRight) / 2.0, y - (lTop + lBottom) / 2.0);
      double dr = Math.hypot(x - (rLeft + rRight) / 2.0, y - (rTop + rBottom) / 2.0);
      return dl <= dr ? 1 : 2;
    }
    if (inLeft) return 1;
    if (inRight) return 2;
    return 0;
  }

  private float dpToPx(float dp) {
    return dp * getResources().getDisplayMetrics().density;
  }

  /**
   * Clamp the selection columns to the screen and, if the two handles crossed
   * over, swap them (and which one is being dragged).
   */
  private void clampAndSwapSelection() {
    mSelX1 = Math.min(mEmulator.mColumns, Math.max(0, mSelX1));
    mSelX2 = Math.min(mEmulator.mColumns, Math.max(0, mSelX2));

    if (mSelY1 == mSelY2 && mSelX1 > mSelX2 || mSelY1 > mSelY2) {
      mIsDraggingLeftSelection = !mIsDraggingLeftSelection;
      int tmpX1 = mSelX1, tmpY1 = mSelY1;
      mSelX1 = mSelX2;
      mSelY1 = mSelY2;
      mSelX2 = tmpX1;
      mSelY2 = tmpY1;
    }
  }

  /**
   * Start/adjust/stop auto-scrolling based on how close to (or far past) the
   * top/bottom edge the dragging finger is. The rows scrolled per tick grow
   * with the distance past the edge, so the selection accelerates the further
   * you drag.
   */
  private void updateSelectionAutoScroll(float y) {
    if (!mIsSelectingText || mEmulator == null) {
      stopSelectionAutoScroll();
      return;
    }

    int edge = (int) (mRenderer.mFontLineSpacing * 1.5f);
    int rows = 0;
    if (y < edge) {
      // Near/above the top edge: scroll up into the scrollback.
      float over = edge - y;
      rows = -(1 + (int) (over / mRenderer.mFontLineSpacing));
    } else if (y > getHeight() - edge) {
      // Near/below the bottom edge: scroll down toward the prompt.
      float over = y - (getHeight() - edge);
      rows = 1 + (int) (over / mRenderer.mFontLineSpacing);
    }

    if (rows < -SELECTION_AUTO_SCROLL_MAX_ROWS) rows = -SELECTION_AUTO_SCROLL_MAX_ROWS;
    if (rows > SELECTION_AUTO_SCROLL_MAX_ROWS) rows = SELECTION_AUTO_SCROLL_MAX_ROWS;

    boolean wasRunning = mSelectionAutoScrollRows != 0;
    mSelectionAutoScrollRows = rows;
    if (rows != 0 && !wasRunning) {
      postDelayed(mSelectionAutoScrollRunnable, SELECTION_AUTO_SCROLL_INTERVAL_MS);
    }
  }

  private void stopSelectionAutoScroll() {
    mSelectionAutoScrollRows = 0;
    removeCallbacks(mSelectionAutoScrollRunnable);
  }

  /**
   * If the single tap landed on a URL in the terminal text, open it in the
   * browser. Rows joined by line-wrap are treated as one logical line so URLs
   * that wrap across the screen width are still detected.
   *
   * @return true if a URL was found and opened.
   */
  private boolean handleTokenTap(MotionEvent e) {
    if (mEmulator == null) return false;
    TerminalBuffer screen = mEmulator.getScreen();
    int columns = mEmulator.mColumns;
    int col = (int) (e.getX() / mRenderer.mFontWidth);
    int tapRow = (int) (e.getY() / mRenderer.mFontLineSpacing) + mTopRow;

    int minRow = -screen.getActiveTranscriptRows();
    int maxRow = mEmulator.mRows - 1;
    if (tapRow < minRow || tapRow > maxRow) return false;

    // An OSC 8 hyperlink on the tapped cell wins: the real target lives in the
    // escape sequence (e.g. a short "Sign in" label hiding a long URL), so the
    // plain-text regex below can't find it.
    String hyperlink = screen.getHyperlinkAt(tapRow, Math.min(col, columns - 1));
    if (hyperlink != null) {
      showTokenActions(new TerminalTokens.Token(TerminalTokens.Type.URL, hyperlink));
      return true;
    }

    int startRow = tapRow;
    for (int n = 0; n < TerminalUrls.MAX_WRAP_ROWS && startRow > minRow
      && TerminalUrls.isContinued(screen, startRow - 1, columns); n++) startRow--;
    int endRow = tapRow;
    for (int n = 0; n < TerminalUrls.MAX_WRAP_ROWS && endRow < maxRow
      && TerminalUrls.isContinued(screen, endRow, columns); n++) endRow++;

    StringBuilder builder = new StringBuilder();
    int tapIndex = -1;
    for (int row = startRow; row <= endRow; row++) {
      String rowText = screen.getSelectedText(0, row, columns - 1, row);
      if (rowText == null) rowText = "";
      if (row == tapRow) tapIndex = builder.length() + Math.min(col, rowText.length());
      builder.append(rowText);
    }
    if (tapIndex < 0) return false;

    TerminalTokens.Token token = TerminalTokens.find(builder, tapIndex);
    if (token == null) return false;
    showTokenActions(token);
    return true;
  }

  /** Quick-action menu for a tapped URL / path / git-hash. */
  private void showTokenActions(final TerminalTokens.Token token) {
    final String value = token.value;
    final CharSequence[] items;
    final Runnable[] actions;
    switch (token.type) {
      case PATH: {
        final String q = shellQuote(value);
        items = new CharSequence[]{
          getContext().getString(R.string.token_edit),
          getContext().getString(R.string.token_list),
          getContext().getString(R.string.token_cd),
          getContext().getString(R.string.token_copy)
        };
        actions = new Runnable[]{
          () -> runInSession("${EDITOR:-nano} " + q),
          () -> runInSession("ls -la " + q),
          () -> runInSession("cd " + q + " 2>/dev/null || cd \"$(dirname " + q + ")\""),
          () -> mTermSession.clipboardText(value)
        };
        break;
      }
      case GIT_HASH: {
        items = new CharSequence[]{
          getContext().getString(R.string.token_git_show),
          getContext().getString(R.string.token_copy)
        };
        actions = new Runnable[]{
          () -> runInSession("git show " + value),
          () -> mTermSession.clipboardText(value)
        };
        break;
      }
      case URL:
      default: {
        items = new CharSequence[]{
          getContext().getString(R.string.token_open),
          getContext().getString(R.string.token_copy)
        };
        actions = new Runnable[]{
          () -> openUrl(value),
          () -> mTermSession.clipboardText(value)
        };
        break;
      }
    }
    new androidx.appcompat.app.AlertDialog.Builder(getContext())
      .setTitle(value)
      .setItems(items, (d, which) -> actions[which].run())
      .show();
  }

  /** Type a command into the active session and run it (append newline). */
  private void runInSession(String command) {
    if (mTermSession != null) mTermSession.write(command + "\n");
  }

  /** Single-quote a string for safe use as one shell argument. */
  private static String shellQuote(String s) {
    return "'" + s.replace("'", "'\\''") + "'";
  }

  private void openUrl(String url) {
    try {
      Intent intent = new Intent(Intent.ACTION_VIEW, Uri.parse(url));
      intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
      getContext().startActivity(intent);
    } catch (Exception e) {
      Log.w(EmulatorDebug.LOG_TAG, "Could not open URL: " + url, e);
    }
  }

  public void pasteFromClipboard() {
    ClipboardManager clipboard = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
    if (clipboard == null) {
      return;
    }
    ClipData clipData = clipboard.getPrimaryClip();
    if (clipData != null) {
      CharSequence paste = clipData.getItemAt(0).coerceToText(getContext());
      if (!TextUtils.isEmpty(paste)) mEmulator.paste(paste.toString());
    }
  }

  @Override
  public boolean onKeyPreIme(int keyCode, KeyEvent event) {
    if (LOG_KEY_EVENTS)
      Log.i(EmulatorDebug.LOG_TAG, "onKeyPreIme(keyCode=" + keyCode + ", event=" + event + ")");
    if (keyCode == KeyEvent.KEYCODE_BACK) {
      if (mIsSelectingText) {
        toggleSelectingText(null);
        return true;
      } else if (mClient.shouldBackButtonBeMappedToEscape()) {
        // Intercept back button to treat it as escape:
        switch (event.getAction()) {
          case KeyEvent.ACTION_DOWN:
            return onKeyDown(keyCode, event);
          case KeyEvent.ACTION_UP:
            return onKeyUp(keyCode, event);
        }
      }
    }
    return super.onKeyPreIme(keyCode, event);
  }

  @Override
  public boolean onKeyDown(int keyCode, KeyEvent event) {
    if (LOG_KEY_EVENTS)
      Log.i(EmulatorDebug.LOG_TAG, "onKeyDown(keyCode=" + keyCode + ", isSystem()=" + event.isSystem() + ", event=" + event + ")");
    if (mEmulator == null) return true;

    if (mClient.onKeyDown(keyCode, event, mTermSession)) {
      invalidate();
      return true;
    } else if (event.isSystem() && (!mClient.shouldBackButtonBeMappedToEscape() || keyCode != KeyEvent.KEYCODE_BACK)) {
      return super.onKeyDown(keyCode, event);
    } else if (event.getAction() == KeyEvent.ACTION_MULTIPLE && keyCode == KeyEvent.KEYCODE_UNKNOWN) {
      mTermSession.write(event.getCharacters());
      return true;
    }

    final int metaState = event.getMetaState();
    final boolean controlDownFromEvent = event.isCtrlPressed();
    final boolean leftAltDownFromEvent = (metaState & KeyEvent.META_ALT_LEFT_ON) != 0;
    final boolean rightAltDownFromEvent = (metaState & KeyEvent.META_ALT_RIGHT_ON) != 0;

    int keyMod = 0;
    if (controlDownFromEvent) keyMod |= KeyHandler.KEYMOD_CTRL;
    if (event.isAltPressed()) keyMod |= KeyHandler.KEYMOD_ALT;
    if (event.isShiftPressed()) keyMod |= KeyHandler.KEYMOD_SHIFT;
    if (!event.isFunctionPressed() && handleKeyCode(keyCode, keyMod)) {
      if (LOG_KEY_EVENTS) Log.i(EmulatorDebug.LOG_TAG, "handleKeyCode() took key event");
      return true;
    }

    // Clear Ctrl since we handle that ourselves:
    int bitsToClear = KeyEvent.META_CTRL_MASK;
    if (rightAltDownFromEvent) {
      // Let right Alt/Alt Gr be used to compose characters.
    } else {
      // Use left alt to send to terminal (e.g. Left Alt+B to jump back a word), so remove:
      bitsToClear |= KeyEvent.META_ALT_ON | KeyEvent.META_ALT_LEFT_ON;
    }
    int effectiveMetaState = event.getMetaState() & ~bitsToClear;

    int result = event.getUnicodeChar(effectiveMetaState);
    if (LOG_KEY_EVENTS)
      Log.i(EmulatorDebug.LOG_TAG, "KeyEvent#getUnicodeChar(" + effectiveMetaState + ") returned: " + result);
    if (result == 0) {
      return false;
    }

    int oldCombiningAccent = mCombiningAccent;
    if ((result & KeyCharacterMap.COMBINING_ACCENT) != 0) {
      // If entered combining accent previously, write it out:
      if (mCombiningAccent != 0)
        inputCodePoint(mCombiningAccent, controlDownFromEvent, leftAltDownFromEvent);
      mCombiningAccent = result & KeyCharacterMap.COMBINING_ACCENT_MASK;
    } else {
      if (mCombiningAccent != 0) {
        int combinedChar = KeyCharacterMap.getDeadChar(mCombiningAccent, result);
        if (combinedChar > 0) result = combinedChar;
        mCombiningAccent = 0;
      }
      inputCodePoint(result, controlDownFromEvent, leftAltDownFromEvent);
    }

    if (mCombiningAccent != oldCombiningAccent) invalidate();

    if (onAutoCompleteListener != null) {
      if (event.isPrintingKey()) {
        char printingChar = (char) event.getUnicodeChar(metaState);
        if (printingChar != '\b') {
          // ASCII chars
          onAutoCompleteListener.onCompletionRequired(new String(new char[]{printingChar}));
        }
      }
    }

    return true;
  }

  void inputCodePoint(int codePoint, boolean controlDownFromEvent, boolean leftAltDownFromEvent) {
    if (LOG_KEY_EVENTS) {
      Log.i(EmulatorDebug.LOG_TAG, "inputCodePoint(codePoint=" + codePoint + ", controlDownFromEvent=" + controlDownFromEvent + ", leftAltDownFromEvent="
        + leftAltDownFromEvent + ")");
    }

    if (mTermSession == null) return;

    final boolean controlDown = controlDownFromEvent || mClient.readControlKey();
    final boolean altDown = leftAltDownFromEvent || mClient.readAltKey();

    if (mClient.onCodePoint(codePoint, controlDown, mTermSession)) return;

    if (controlDown) {
      if (codePoint >= 'a' && codePoint <= 'z') {
        codePoint = codePoint - 'a' + 1;
      } else if (codePoint >= 'A' && codePoint <= 'Z') {
        codePoint = codePoint - 'A' + 1;
      } else if (codePoint == ' ' || codePoint == '2') {
        codePoint = 0;
      } else if (codePoint == '[' || codePoint == '3') {
        codePoint = 27; // ^[ (Esc)
      } else if (codePoint == '\\' || codePoint == '4') {
        codePoint = 28;
      } else if (codePoint == ']' || codePoint == '5') {
        codePoint = 29;
      } else if (codePoint == '^' || codePoint == '6') {
        codePoint = 30; // control-^
      } else if (codePoint == '_' || codePoint == '7' || codePoint == '/') {
        // "Ctrl-/ sends 0x1f which is equivalent of Ctrl-_ since the days of VT102"
        // - http://apple.stackexchange.com/questions/24261/how-do-i-send-c-that-is-control-slash-to-the-terminal
        codePoint = 31;
      } else if (codePoint == '8') {
        codePoint = 127; // DEL
      }
    }

    if (codePoint > -1) {
      // Work around bluetooth keyboards sending funny unicode characters instead
      // of the more normal ones from ASCII that terminal programs expect - the
      // desire to input the original characters should be low.
      switch (codePoint) {
        case 0x02DC: // SMALL TILDE.
          codePoint = 0x007E; // TILDE (~).
          break;
        case 0x02CB: // MODIFIER LETTER GRAVE ACCENT.
          codePoint = 0x0060; // GRAVE ACCENT (`).
          break;
        case 0x02C6: // MODIFIER LETTER CIRCUMFLEX ACCENT.
          codePoint = 0x005E; // CIRCUMFLEX ACCENT (^).
          break;
      }

      // If left alt, send escape before the code point to make e.g. Alt+B and Alt+F work in readline:
      mTermSession.writeCodePoint(altDown, codePoint);
      scrollToBottomIfNeeded();
    }
  }

  /**
   * Input the specified keyCode if applicable and return if the input was consumed.
   */
  public boolean handleKeyCode(int keyCode, int keyMod) {
    TerminalEmulator term = mTermSession.getEmulator();
    String code = KeyHandler.getCode(keyCode, keyMod, term.isCursorKeysApplicationMode(), term.isKeypadApplicationMode());
    if (code == null) return false;
    mTermSession.write(code);
    scrollToBottomIfNeeded();
    if (onAutoCompleteListener != null) {
      onAutoCompleteListener.onKeyCode(keyCode, keyMod);
    }
    return true;
  }

  /**
   * Called when a key is released in the view.
   *
   * @param keyCode The keycode of the key which was released.
   * @param event   A {@link KeyEvent} describing the event.
   * @return Whether the event was handled.
   */
  @Override
  public boolean onKeyUp(int keyCode, KeyEvent event) {
    if (LOG_KEY_EVENTS)
      Log.i(EmulatorDebug.LOG_TAG, "onKeyUp(keyCode=" + keyCode + ", event=" + event + ")");
    if (mEmulator == null) return true;

    if (mClient.onKeyUp(keyCode, event)) {
      invalidate();
      return true;
    } else if (event.isSystem()) {
      // Let system key events through.
      return super.onKeyUp(keyCode, event);
    }

    return true;
  }

  /**
   * 每次处理用户按下对终端输出有影响的键时被调用
   * 如果当前屏幕不处于最底部，则自动滚动到最底部
   */
  void scrollToBottomIfNeeded() {
    if (mTopRow != 0) {
      mTopRow = 0;
      mEmulator.clearScrollCounter();
      invalidate();
    }
  }

  /**
   * This is called during layout when the size of this view has changed. If you were just added to the view
   * hierarchy, you're called with the old values of 0.
   */
  @Override
  protected void onSizeChanged(int w, int h, int oldw, int oldh) {
    updateSize();
  }

  /**
   * Set the keyboard pan (px). Called by the host activity when the soft keyboard overlaps the
   * view. We deliberately do NOT resize here: the value is stored and the next layout pass
   * ({@link #onSizeChanged} -> {@link #updateSize}) recomputes rows against the un-panned height,
   * keeping the row count constant across the keyboard toggle. Only a repaint is needed to apply
   * the new content offset.
   */
  public void setKeyboardPan(int panPx) {
    if (panPx < 0) panPx = 0;
    if (panPx == mKeyboardPanPx) return;
    mKeyboardPanPx = panPx;
    invalidate();
  }

  /**
   * Check if the terminal size in rows and columns should be updated.
   */
  public void updateSize() {
    int viewWidth = getWidth();
    // Add back the keyboard pan so the row count is computed against the full (un-covered) height:
    // the keyboard pans the content instead of resizing the terminal.
    int viewHeight = getHeight() + mKeyboardPanPx;
    if (viewWidth == 0 || viewHeight == 0 || mTermSession == null) return;

    // Set to 80 and 24 if you want to enable vttest.
    int newColumns = Math.max(4, (int) (viewWidth / mRenderer.mFontWidth));
    int newRows = Math.max(4, (viewHeight - mRenderer.mFontLineSpacingAndAscent) / mRenderer.mFontLineSpacing);

    if (mEmulator == null || (newColumns != mEmulator.mColumns || newRows != mEmulator.mRows)) {
      mTermSession.updateSize(newColumns, newRows);
      mEmulator = mTermSession.getEmulator();

      mTopRow = 0;
      scrollTo(0, 0);
      invalidate();
    }

    // Keep the emulator's cell pixel size current so Sixel images map to the
    // right number of cells (also covers font-size/pinch-zoom changes).
    if (mEmulator != null) {
      mEmulator.setCellSize((int) mRenderer.mFontWidth, mRenderer.mFontLineSpacing);
    }
  }

  @Override
  protected void onDraw(Canvas canvas) {
    if (mEmulator == null) {
      canvas.drawColor(0XFF000000);
    } else {
      // Pan the content up when the keyboard covers the bottom (see mKeyboardPanPx). Wrapped in
      // save/restore so the offset doesn't leak into foreground drawing (e.g. scrollbars).
      int saveCount = -1;
      if (mKeyboardPanPx != 0) {
        saveCount = canvas.save();
        canvas.translate(0, -mKeyboardPanPx);
      }
      mRenderer.render(mEmulator, canvas, mTopRow, mSelY1, mSelY2, mSelX1, mSelX2, mCursorBlinkOn);

      if (mIsSelectingText) {
        final int gripHandleWidth = mLeftSelectionHandle.getIntrinsicWidth();
        final int gripHandleMargin = gripHandleWidth / 4; // See the png.

        int right = Math.round((mSelX1) * mRenderer.mFontWidth) + gripHandleMargin;
        int top = (mSelY1 + 1 - mTopRow) * mRenderer.mFontLineSpacing + mRenderer.mFontLineSpacingAndAscent;
        mLeftSelectionHandle.setBounds(right - gripHandleWidth, top, right, top + mLeftSelectionHandle.getIntrinsicHeight());
        mLeftSelectionHandle.draw(canvas);

        int left = Math.round((mSelX2 + 1) * mRenderer.mFontWidth) - gripHandleMargin;
        top = (mSelY2 + 1 - mTopRow) * mRenderer.mFontLineSpacing + mRenderer.mFontLineSpacingAndAscent;
        mRightSelectionHandle.setBounds(left, top, left + gripHandleWidth, top + mRightSelectionHandle.getIntrinsicHeight());
        mRightSelectionHandle.draw(canvas);
      }

      if (saveCount != -1) canvas.restoreToCount(saveCount);
    }
  }

  /**
   * Toggle text selection mode in the view.
   */
  @TargetApi(23)
  public void toggleSelectingText(MotionEvent ev) {
    mIsSelectingText = !mIsSelectingText;
    mClient.copyModeChanged(mIsSelectingText);

    if (mIsSelectingText) {
      if (mLeftSelectionHandle == null) {
        mLeftSelectionHandle = (BitmapDrawable) getContext().getDrawable(R.drawable.text_select_handle_left_material);
        mRightSelectionHandle = (BitmapDrawable) getContext().getDrawable(R.drawable.text_select_handle_right_material);
      }

      int cx = (int) (ev.getX() / mRenderer.mFontWidth);
      final boolean eventFromMouse = ev.isFromSource(InputDevice.SOURCE_MOUSE);
      // Offset for finger:
      final int SELECT_TEXT_OFFSET_Y = eventFromMouse ? 0 : -40;
      int cy = (int) ((ev.getY() + SELECT_TEXT_OFFSET_Y) / mRenderer.mFontLineSpacing) + mTopRow;

      mSelX1 = mSelX2 = cx;
      mSelY1 = mSelY2 = cy;

      TerminalBuffer screen = mEmulator.getScreen();
      if (!" ".equals(screen.getSelectedText(mSelX1, mSelY1, mSelX1, mSelY1))) {
        // Selecting something other than whitespace. Expand to word.
        while (mSelX1 > 0 && !"".equals(screen.getSelectedText(mSelX1 - 1, mSelY1, mSelX1 - 1, mSelY1))) {
          mSelX1--;
        }
        while (mSelX2 < mEmulator.mColumns - 1 && !"".equals(screen.getSelectedText(mSelX2 + 1, mSelY1, mSelX2 + 1, mSelY1))) {
          mSelX2++;
        }
      }

      mInitialTextSelection = true;
      mIsDraggingLeftSelection = true;
      mSelectionDownX = ev.getX();
      mSelectionDownY = ev.getY();

      final ActionMode.Callback callback = new ActionMode.Callback() {
        @Override
        public boolean onCreateActionMode(ActionMode mode, Menu menu) {
          int show = MenuItem.SHOW_AS_ACTION_ALWAYS | MenuItem.SHOW_AS_ACTION_WITH_TEXT;

          ClipboardManager clipboard = (ClipboardManager) getContext().getSystemService(Context.CLIPBOARD_SERVICE);
          menu.add(Menu.NONE, 1, Menu.NONE, R.string.copy_text).setShowAsAction(show);
          menu.add(Menu.NONE, 2, Menu.NONE, R.string.paste_text).setEnabled(clipboard.hasPrimaryClip()).setShowAsAction(show);
          menu.add(Menu.NONE, 3, Menu.NONE, R.string.cc_button).setShowAsAction(show);

          return true;
        }

        @Override
        public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
          return false;
        }

        @Override
        public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
          if (!mIsSelectingText) {
            // Fix issue where the dialog is pressed while being dismissed.
            return true;
          }

          switch (item.getItemId()) {
            case 1:
              String selectedText = mEmulator.getSelectedText(mSelX1, mSelY1, mSelX2, mSelY2).trim();
              mTermSession.clipboardText(selectedText);
              break;
            case 2:
              pasteFromClipboard();
              break;
            case 3:
              mClient.onCustomCommands(mTermSession);
              break;
          }
          toggleSelectingText(null);
          return true;
        }

        @Override
        public void onDestroyActionMode(ActionMode mode) {
        }
      };

      mActionMode = startActionMode(new ActionMode.Callback2() {
        @Override
        public boolean onCreateActionMode(ActionMode mode, Menu menu) {
          return callback.onCreateActionMode(mode, menu);
        }

        @Override
        public boolean onPrepareActionMode(ActionMode mode, Menu menu) {
          return false;
        }

        @Override
        public boolean onActionItemClicked(ActionMode mode, MenuItem item) {
          return callback.onActionItemClicked(mode, item);
        }

        @Override
        public void onDestroyActionMode(ActionMode mode) {
          // Ignore.
        }

        @Override
        public void onGetContentRect(ActionMode mode, View view, Rect outRect) {
          int x1 = Math.round(mSelX1 * mRenderer.mFontWidth);
          int x2 = Math.round(mSelX2 * mRenderer.mFontWidth);
          int y1 = Math.round((mSelY1 - mTopRow) * mRenderer.mFontLineSpacing);
          int y2 = Math.round((mSelY2 + 1 - mTopRow) * mRenderer.mFontLineSpacing);
          outRect.set(Math.min(x1, x2), y1, Math.max(x1, x2), y2);
        }
      }, ActionMode.TYPE_FLOATING);
      invalidate();
    } else {
      stopSelectionAutoScroll();
      mIsDraggingHandle = false;
      mActionMode.finish();
      mSelX1 = mSelY1 = mSelX2 = mSelY2 = -1;
      invalidate();
    }
  }

  public TerminalSession getCurrentSession() {
    return mTermSession;
  }


  private OnAutoCompleteListener onAutoCompleteListener;

  public OnAutoCompleteListener getOnAutoCompleteListener() {
    return onAutoCompleteListener;
  }

  public void setOnAutoCompleteListener(OnAutoCompleteListener onAutoCompleteListener) {
    this.onAutoCompleteListener = onAutoCompleteListener;
  }

  public int getCursorAbsoluteX() {
    return (int) mRenderer.getCursorX();
  }

  public int getCursorAbsoluteY() {
    int[] locations = new int[2];
    getLocationOnScreen(locations);
    return (int) (mRenderer.getCursorY() + locations[1]);
  }

  public void setEnableWordBasedIme(boolean mEnableWordBasedIme) {
    this.mEnableWordBasedIme = mEnableWordBasedIme;
  }
}
