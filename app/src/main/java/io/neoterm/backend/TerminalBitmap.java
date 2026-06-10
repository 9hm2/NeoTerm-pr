package io.neoterm.backend;

import android.graphics.Bitmap;

/**
 * A decoded inline image (e.g. from a Sixel sequence) together with the number
 * of terminal cells it occupies. The image is laid into the screen buffer as a
 * grid of {@link #cellCols} x {@link #cellRows} cells whose styles reference this
 * bitmap (see {@link TextStyle#encodeImage}); the renderer draws each cell as the
 * corresponding sub-tile, so the image scrolls and clears with the text.
 */
public final class TerminalBitmap {

  public final Bitmap bitmap;
  public final int cellCols;
  public final int cellRows;

  public TerminalBitmap(Bitmap bitmap, int cellCols, int cellRows) {
    this.bitmap = bitmap;
    this.cellCols = cellCols;
    this.cellRows = cellRows;
  }
}
