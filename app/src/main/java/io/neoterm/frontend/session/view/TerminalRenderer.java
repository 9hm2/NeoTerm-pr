package io.neoterm.frontend.session.view;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.PorterDuff;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import io.neoterm.backend.*;

/**
 * Renderer of a {@link TerminalEmulator} into a {@link Canvas}.
 * <p/>
 * Saves font metrics, so needs to be recreated each time the typeface or font size changes.
 */
final class TerminalRenderer {

  final int mTextSize;
  final Typeface mTypeface;
  private final Paint mTextPaint = new Paint();

  /**
   * The width of a single mono spaced character obtained by {@link Paint#measureText(String)} on a single 'X'.
   */
  final float mFontWidth;
  /**
   * The {@link Paint#getFontSpacing()}. See http://www.fampennings.nl/maarten/android/08numgrid/font.png
   */
  final int mFontLineSpacing;
  /**
   * The {@link Paint#ascent()}. See http://www.fampennings.nl/maarten/android/08numgrid/font.png
   */
  private final int mFontAscent;
  /**
   * The {@link #mFontLineSpacing} + {@link #mFontAscent}.
   */
  final int mFontLineSpacingAndAscent;

  /**
   * AutoCompletion PopupWindow need them to show popup window
   */
  protected float savedLastDrawnLineX;
  protected float savedLastDrawnLineY;

  private final float[] asciiMeasures = new float[127];

  /**
   * Fallback typeface used to draw glyphs the user-selected font lacks, so they
   * don't render as tofu. Installed at startup with the bundled broad-coverage
   * symbol font (assets/symbol_fallback.ttf) via
   * {@link #setFallbackTypeface(Typeface)}. Until then it degrades to the system
   * MONOSPACE — whose fallback chain uses *subsetted* Noto symbol fonts that
   * themselves omit many glyphs (e.g. the media-control triangles U+23F4..FA used
   * by TUIs), which is exactly the tofu the bundled font fixes.
   */
  private static Typeface sFallbackTypeface = Typeface.MONOSPACE;

  /** Install the bundled symbol fallback font (see {@link #sFallbackTypeface}). */
  static void setFallbackTypeface(Typeface typeface) {
    if (typeface != null) sFallbackTypeface = typeface;
  }
  /** Per-code-point cache of whether the user font has the glyph (1) or needs
   *  the fallback (0); -1 = unknown. Keeps the per-frame check cheap. */
  private final android.util.SparseIntArray mGlyphCache = new android.util.SparseIntArray();

  // Reusable objects for drawing inline image (Sixel) cell tiles.
  private final Rect mImageSrc = new Rect();
  private final RectF mImageDst = new RectF();
  private final Paint mImagePaint = new Paint(Paint.FILTER_BITMAP_FLAG);

  public TerminalRenderer(int textSize, Typeface typeface) {
    mTextSize = textSize;
    mTypeface = typeface;

    mTextPaint.setTypeface(typeface);
    mTextPaint.setAntiAlias(true);
    mTextPaint.setTextSize(textSize);

    mFontLineSpacing = (int) Math.ceil(mTextPaint.getFontSpacing());
    mFontAscent = (int) Math.ceil(mTextPaint.ascent());
    mFontLineSpacingAndAscent = mFontLineSpacing + mFontAscent;
    mFontWidth = mTextPaint.measureText("X");

    StringBuilder sb = new StringBuilder(" ");
    for (int i = 0; i < asciiMeasures.length; i++) {
      sb.setCharAt(0, (char) i);
      asciiMeasures[i] = mTextPaint.measureText(sb, 0, 1);
    }
  }

  /**
   * Render the terminal to a canvas with at a specified row scroll, and an optional rectangular selection.
   */
  public final void render(TerminalEmulator mEmulator, Canvas canvas, int topRow,
                           int selectionY1, int selectionY2, int selectionX1, int selectionX2,
                           boolean cursorBlinkOn) {
    final boolean reverseVideo = mEmulator.isReverseVideo();
    final int endRow = topRow + mEmulator.mRows;
    final int columns = mEmulator.mColumns;
    final int cursorCol = mEmulator.getCursorCol();
    final int cursorRow = mEmulator.getCursorRow();
    // The cursor is shown when the emulator says so AND the view's blink phase is currently "on".
    final boolean cursorVisible = mEmulator.isShowingCursor() && cursorBlinkOn;
    final TerminalBuffer screen = mEmulator.getScreen();
    final int[] palette = mEmulator.mColors.mCurrentColors;
    final int cursorShape = mEmulator.getCursorStyle();

    if (reverseVideo)
      canvas.drawColor(palette[TextStyle.COLOR_INDEX_FOREGROUND], PorterDuff.Mode.SRC);

    // Columns that are part of a clickable URL get underlined so the user can
    // see what is tappable. Computed once for the whole visible region (wrap
    // aware), indexed by (row - topRow).
    final boolean[][] urlMask = computeUrlMask(mEmulator, screen, topRow, endRow, columns);

    float heightOffset = mFontLineSpacingAndAscent;
    for (int row = topRow; row < endRow; row++) {
      heightOffset += mFontLineSpacing;

      final int cursorX = (row == cursorRow && cursorVisible) ? cursorCol : -1;
      int selx1 = -1, selx2 = -1;
      if (row >= selectionY1 && row <= selectionY2) {
        if (row == selectionY1) selx1 = selectionX1;
        selx2 = (row == selectionY2) ? selectionX2 : mEmulator.mColumns;
      }

      final boolean[] rowUrlMask = urlMask[row - topRow];

      TerminalRow lineObject = screen.allocateFullLineIfNecessary(screen.externalToInternalRow(row));
      final char[] line = lineObject.mText;
      final int charsUsedInLine = lineObject.getSpaceUsed();

      long lastRunStyle = 0;
      boolean lastRunInsideCursor = false;
      boolean lastRunUrl = false;
      int lastRunStartColumn = -1;
      int lastRunStartIndex = 0;
      boolean lastRunFontWidthMismatch = false;
      boolean lastRunMissingGlyph = false;
      int currentCharIndex = 0;
      float measuredWidthForRun = 0.f;

      for (int column = 0; column < columns; ) {
        final char charAtIndex = line[currentCharIndex];
        final boolean charIsHighsurrogate = Character.isHighSurrogate(charAtIndex);
        final int charsForCodePoint = charIsHighsurrogate ? 2 : 1;
        final int codePoint = charIsHighsurrogate ? Character.toCodePoint(charAtIndex, line[currentCharIndex + 1]) : charAtIndex;
        int codePointWcWidth = WcWidth.width(codePoint);
        // VS16 (U+FE0F) right after a width-1 base makes it emoji-presentation,
        // width 2 — match the emulator's cursor model (see emitCodePoint).
        if (codePointWcWidth == 1 && followedByVs16(line, currentCharIndex + charsForCodePoint, charsUsedInLine)) {
          codePointWcWidth = 2;
        }
        final boolean insideCursor = (column >= selx1 && column <= selx2) || (cursorX == column || (codePointWcWidth == 2 && cursorX == column + 1));
        final boolean insideUrl = rowUrlMask != null && column < rowUrlMask.length && rowUrlMask[column];
        final long style = lineObject.getStyle(column);

        // Check if the measured text width for this code point is not the same as that expected by wcwidth().
        // This could happen for some fonts which are not truly monospace, or for more exotic characters such as
        // smileys which android font renders as wide.
        // If this is detected, we draw this code point scaled to match what wcwidth() expects.
        // A custom (user) font has no system fallback chain, so glyphs it lacks
        // would render as tofu/blank. Detect those and draw the run with a
        // fallback typeface (which does fall back to the system fonts).
        final boolean missingGlyph = codePointWcWidth > 0 && !fontHasGlyph(codePoint);
        // For a present glyph use the real measured width (so non-monospace and
        // wide glyphs are scaled to their cell); for a missing one the user font's
        // measure is meaningless (often the .notdef/tofu advance, sometimes 0), so
        // use the expected cell width — the real width comes from the fallback font.
        final float measuredCodePointWidth = missingGlyph ? (codePointWcWidth * mFontWidth)
          : (codePoint < asciiMeasures.length) ? asciiMeasures[codePoint]
          : mTextPaint.measureText(line, currentCharIndex, charsForCodePoint);
        final boolean fontWidthMismatch = Math.abs(measuredCodePointWidth / mFontWidth - codePointWcWidth) > 0.01;

        if (style != lastRunStyle || insideCursor != lastRunInsideCursor || insideUrl != lastRunUrl || fontWidthMismatch || lastRunFontWidthMismatch || missingGlyph != lastRunMissingGlyph) {
          if (column == 0) {
            // Skip first column as there is nothing to draw, just record the current style.
          } else {
            final int columnWidthSinceLastRun = column - lastRunStartColumn;
            final int charsSinceLastRun = currentCharIndex - lastRunStartIndex;
            int cursorColor = lastRunInsideCursor ? mEmulator.mColors.mCurrentColors[TextStyle.COLOR_INDEX_CURSOR] : 0;
            drawTextRun(canvas, line, palette, heightOffset, lastRunStartColumn, columnWidthSinceLastRun,
              lastRunStartIndex, charsSinceLastRun, measuredWidthForRun,
              cursorColor, cursorShape, lastRunStyle, reverseVideo, lastRunUrl, lastRunMissingGlyph);
          }
          measuredWidthForRun = 0.f;
          lastRunStyle = style;
          lastRunInsideCursor = insideCursor;
          lastRunUrl = insideUrl;
          lastRunStartColumn = column;
          lastRunStartIndex = currentCharIndex;
          lastRunFontWidthMismatch = fontWidthMismatch;
          lastRunMissingGlyph = missingGlyph;
        }
        measuredWidthForRun += measuredCodePointWidth;
        column += codePointWcWidth;
        currentCharIndex += charsForCodePoint;
        while (currentCharIndex < charsUsedInLine && WcWidth.width(line, currentCharIndex) <= 0) {
          // Eat combining chars so that they are treated as part of the last non-combining code point,
          // instead of e.g. being considered inside the cursorColor in the next run.
          currentCharIndex += Character.isHighSurrogate(line[currentCharIndex]) ? 2 : 1;
        }
      }

      final int columnWidthSinceLastRun = columns - lastRunStartColumn;
      final int charsSinceLastRun = currentCharIndex - lastRunStartIndex;
      int cursorColor = lastRunInsideCursor ? mEmulator.mColors.mCurrentColors[TextStyle.COLOR_INDEX_CURSOR] : 0;
      drawTextRun(canvas, line, palette, heightOffset, lastRunStartColumn, columnWidthSinceLastRun, lastRunStartIndex, charsSinceLastRun,
        measuredWidthForRun, cursorColor, cursorShape, lastRunStyle, reverseVideo, lastRunUrl, lastRunMissingGlyph);

      // Inline image (Sixel) cells: drawTextRun skips them, so draw their bitmap
      // tiles here over the (blank) cells. Only when images exist on screen.
      if (mEmulator.hasBitmaps()) {
        drawImageCells(canvas, mEmulator, lineObject, columns, heightOffset);
      }
    }
  }

  /**
   * Whether the combining sequence starting at {@code index} (zero-width code
   * points attached to the preceding cell) contains a VS16 (U+FE0F). Scans only
   * the run of zero-width code points, mirroring how the render loop eats them.
   */
  /**
   * Whether the user-selected font can render {@code codePoint}. ASCII is always
   * present; other code points are probed once via {@link Paint#hasGlyph} and
   * cached. When false, the run is drawn with {@link #sFallbackTypeface} so the
   * glyph falls back to the bundled symbol font instead of rendering as tofu.
   */
  private boolean fontHasGlyph(int codePoint) {
    if (codePoint < 0x80) return true;
    int cached = mGlyphCache.get(codePoint, -1);
    if (cached != -1) return cached == 1;
    boolean has;
    try {
      has = mTextPaint.hasGlyph(new String(Character.toChars(codePoint)));
    } catch (Exception e) {
      has = true; // On any error, assume present (draw with the user font).
    }
    mGlyphCache.put(codePoint, has ? 1 : 0);
    return has;
  }

  private static boolean followedByVs16(char[] line, int index, int charsUsed) {
    int i = index;
    while (i < charsUsed && WcWidth.width(line, i) <= 0) {
      boolean high = Character.isHighSurrogate(line[i]);
      int cp = high ? Character.toCodePoint(line[i], line[i + 1]) : line[i];
      if (cp == 0xFE0F) return true;
      i += high ? 2 : 1;
    }
    return false;
  }

  /** Draw the bitmap tile of every inline-image cell in a row. */
  private void drawImageCells(Canvas canvas, TerminalEmulator emulator, TerminalRow lineObject, int columns, float baselineY) {
    final float top = baselineY - mFontLineSpacing;
    for (int column = 0; column < columns; column++) {
      final long style = lineObject.getStyle(column);
      if (!TextStyle.isImage(style)) continue;
      final TerminalBitmap holder = emulator.getBitmap(TextStyle.decodeImageId(style));
      if (holder == null || holder.bitmap == null || holder.cellCols <= 0 || holder.cellRows <= 0) continue;
      final int tileCol = TextStyle.decodeImageCol(style);
      final int tileRow = TextStyle.decodeImageRow(style);
      final int bw = holder.bitmap.getWidth();
      final int bh = holder.bitmap.getHeight();
      final int sl = (int) ((long) tileCol * bw / holder.cellCols);
      final int sr = (int) ((long) (tileCol + 1) * bw / holder.cellCols);
      final int st = (int) ((long) tileRow * bh / holder.cellRows);
      final int sb = (int) ((long) (tileRow + 1) * bh / holder.cellRows);
      if (sr <= sl || sb <= st) continue;
      mImageSrc.set(sl, st, sr, sb);
      final float left = column * mFontWidth;
      mImageDst.set(left, top, left + mFontWidth, top + mFontLineSpacing);
      canvas.drawBitmap(holder.bitmap, mImageSrc, mImageDst, mImagePaint);
    }
  }

  /**
   * Build, for every visible row, a boolean mask flagging the columns that are
   * part of a clickable URL. Rows joined by line-wrap are treated as one logical
   * line so URLs spanning the screen width are highlighted continuously. The
   * detection mirrors {@link TerminalUrls} so the underline matches the span the
   * view opens on tap.
   */
  private boolean[][] computeUrlMask(TerminalEmulator emulator, TerminalBuffer screen,
                                     int topRow, int endRow, int columns) {
    final int rows = endRow - topRow;
    final boolean[][] mask = new boolean[rows][];
    final int minRow = -screen.getActiveTranscriptRows();
    final int maxRow = emulator.mRows - 1;

    int row = topRow;
    while (row < endRow) {
      int startRow = row;
      for (int n = 0; n < TerminalUrls.MAX_WRAP_ROWS && startRow > minRow
        && TerminalUrls.isContinued(screen, startRow - 1, columns); n++) startRow--;
      int logicalEnd = row;
      for (int n = 0; n < TerminalUrls.MAX_WRAP_ROWS && logicalEnd < maxRow
        && TerminalUrls.isContinued(screen, logicalEnd, columns); n++) logicalEnd++;

      StringBuilder builder = new StringBuilder((logicalEnd - startRow + 1) * columns);
      for (int r = startRow; r <= logicalEnd; r++) {
        appendRowColumns(builder, screen.allocateFullLineIfNecessary(screen.externalToInternalRow(r)), columns);
      }

      boolean[] flat = new boolean[builder.length()];
      java.util.regex.Matcher matcher = TerminalUrls.PATTERN.matcher(builder);
      while (matcher.find()) {
        int end = TerminalUrls.trimmedEnd(builder, matcher.start(), matcher.end());
        for (int i = matcher.start(); i < end; i++) flat[i] = true;
      }

      int visibleFrom = Math.max(startRow, topRow);
      int visibleTo = Math.min(logicalEnd, endRow - 1);
      for (int r = visibleFrom; r <= visibleTo; r++) {
        TerminalRow lineObj = screen.allocateFullLineIfNecessary(screen.externalToInternalRow(r));
        boolean[] rowMask = new boolean[columns];
        int base = (r - startRow) * columns;
        boolean any = false;
        for (int c = 0; c < columns; c++) {
          // A cell is "tappable" (underlined) if it's in a regex-matched URL or
          // carries an OSC 8 hyperlink.
          boolean on = (base + c < flat.length && flat[base + c]) || lineObj.getHyperlink(c) != null;
          if (on) {
            rowMask[c] = true;
            any = true;
          }
        }
        mask[r - topRow] = any ? rowMask : null;
      }

      row = logicalEnd + 1;
    }
    return mask;
  }

  /**
   * Append exactly {@code columns} characters representing one terminal row, so
   * that the string index of each character equals its column. Wide and astral
   * code points (never part of a URL) are emitted as placeholders that keep the
   * column alignment intact.
   */
  private void appendRowColumns(StringBuilder builder, TerminalRow lineObject, int columns) {
    final char[] line = lineObject.mText;
    final int charsUsed = lineObject.getSpaceUsed();
    int charIndex = 0;
    int column = 0;
    while (column < columns) {
      if (charIndex >= charsUsed) {
        builder.append(' ');
        column++;
        continue;
      }
      final char c = line[charIndex];
      final boolean highSurrogate = Character.isHighSurrogate(c);
      final int codePoint = highSurrogate ? Character.toCodePoint(c, line[charIndex + 1]) : c;
      final int width = WcWidth.width(codePoint);
      charIndex += highSurrogate ? 2 : 1;
      while (charIndex < charsUsed && WcWidth.width(line, charIndex) <= 0) {
        charIndex += Character.isHighSurrogate(line[charIndex]) ? 2 : 1;
      }
      if (width <= 0) {
        continue;
      }
      builder.append(highSurrogate ? ' ' : c);
      column++;
      for (int k = 1; k < width && column < columns; k++) {
        builder.append(' ');
        column++;
      }
    }
  }

  private void drawTextRun(Canvas canvas, char[] text, int[] palette, float y, int startColumn, int runWidthColumns,
                           int startCharIndex, int runWidthChars, float mes, int cursor, int cursorStyle,
                           long textStyle, boolean reverseVideo, boolean forceUnderline, boolean fallbackFont) {
    // Inline image cells are drawn separately (drawImageCells); skip them here so
    // their packed image id isn't misread as colours.
    if (TextStyle.isImage(textStyle)) return;
    int foreColor = TextStyle.decodeForeColor(textStyle);
    final int effect = TextStyle.decodeEffect(textStyle);
    int backColor = TextStyle.decodeBackColor(textStyle);
    final boolean bold = (effect & (TextStyle.CHARACTER_ATTRIBUTE_BOLD | TextStyle.CHARACTER_ATTRIBUTE_BLINK)) != 0;
    // URLs are underlined so the user can see they are tappable.
    final boolean underline = forceUnderline || (effect & TextStyle.CHARACTER_ATTRIBUTE_UNDERLINE) != 0;
    final boolean italic = (effect & TextStyle.CHARACTER_ATTRIBUTE_ITALIC) != 0;
    final boolean strikeThrough = (effect & TextStyle.CHARACTER_ATTRIBUTE_STRIKETHROUGH) != 0;
    final boolean dim = (effect & TextStyle.CHARACTER_ATTRIBUTE_DIM) != 0;

    if ((foreColor & 0xff000000) != 0xff000000) {
      // Let bold have bright colors if applicable (one of the first 8):
      if (bold && foreColor >= 0 && foreColor < 8) foreColor += 8;
      foreColor = palette[foreColor];
    }

    if ((backColor & 0xff000000) != 0xff000000) {
      backColor = palette[backColor];
    }

    // Reverse video here if _one and only one_ of the reverse flags are set:
    final boolean reverseVideoHere = reverseVideo ^ (effect & (TextStyle.CHARACTER_ATTRIBUTE_INVERSE)) != 0;
    if (reverseVideoHere) {
      int tmp = foreColor;
      foreColor = backColor;
      backColor = tmp;
    }

    // For runs drawn with the fallback font, switch the typeface now and measure
    // with it so the scale-to-fit below uses the fallback glyphs' real advances.
    // (Measuring against the user font, whose glyph the run lacks, would mis-scale
    // it.) The typeface is restored at the end of the method.
    if (fallbackFont) {
      mTextPaint.setTypeface(sFallbackTypeface);
      float fallbackMeasured = mTextPaint.measureText(text, startCharIndex, runWidthChars);
      if (fallbackMeasured > 0f) mes = fallbackMeasured;
    }

    float left = startColumn * mFontWidth;
    float right = left + runWidthColumns * mFontWidth;

    mes = mes / mFontWidth;
    boolean savedMatrix = false;
    if (Math.abs(mes - runWidthColumns) > 0.01) {
      canvas.save();
      canvas.scale(runWidthColumns / mes, 1.f);
      left *= mes / runWidthColumns;
      right *= mes / runWidthColumns;
      savedMatrix = true;
    }

    if (backColor != palette[TextStyle.COLOR_INDEX_BACKGROUND]) {
      // Only draw non-default backgroundColor.
      mTextPaint.setColor(backColor);
      canvas.drawRect(left, y - mFontLineSpacingAndAscent + mFontAscent, right, y, mTextPaint);
    }

    if (cursor != 0) {
      mTextPaint.setColor(cursor);
      float cursorHeight = mFontLineSpacingAndAscent - mFontAscent;
      if (cursorStyle == TerminalEmulator.CURSOR_STYLE_UNDERLINE) cursorHeight /= 4.;
      else if (cursorStyle == TerminalEmulator.CURSOR_STYLE_BAR) right -= ((right - left) * 3) / 4.;
      canvas.drawRect(left, y - cursorHeight, right, y, mTextPaint);
      savedLastDrawnLineX = left;
      savedLastDrawnLineY = y;
    }

    if ((effect & TextStyle.CHARACTER_ATTRIBUTE_INVISIBLE) == 0) {
      // A block cursor fills the whole cell with the cursor colour and the glyph is painted on
      // top of it. Draw that glyph in the cell's background colour (reverse video under the
      // cursor) so it stays readable -- otherwise a glyph whose colour happens to match the
      // cursor colour would vanish under the block. Bar/underline cursors don't cover the glyph.
      final boolean blockCursorOverGlyph = cursor != 0 && cursorStyle == TerminalEmulator.CURSOR_STYLE_BLOCK;
      if (blockCursorOverGlyph) foreColor = backColor;

      if (dim && !blockCursorOverGlyph) {
        int red = (0xFF & (foreColor >> 16));
        int green = (0xFF & (foreColor >> 8));
        int blue = (0xFF & foreColor);
        // Dim color handling used by libvte which in turn took it from xterm
        // (https://bug735245.bugzilla-attachments.gnome.org/attachment.cgi?id=284267):
        red = red * 2 / 3;
        green = green * 2 / 3;
        blue = blue * 2 / 3;
        foreColor = 0xFF000000 + (red << 16) + (green << 8) + blue;
      }

      mTextPaint.setFakeBoldText(bold);
      mTextPaint.setUnderlineText(underline);
      mTextPaint.setTextSkewX(italic ? -0.35f : 0.f);
      mTextPaint.setStrikeThruText(strikeThrough);
      mTextPaint.setColor(foreColor);

      // The text alignment is the default Paint.Align.LEFT. For fallback runs the
      // typeface was switched (and the run measured) above; it is restored below so
      // it is reset even for invisible runs.
      canvas.drawText(text, startCharIndex, runWidthChars, left, y - mFontLineSpacingAndAscent, mTextPaint);
    }

    if (fallbackFont) mTextPaint.setTypeface(mTypeface);
    if (savedMatrix) canvas.restore();
  }

  float getCursorX() {
    return savedLastDrawnLineX;
  }

  float getCursorY() {
    return savedLastDrawnLineY;
  }
}
