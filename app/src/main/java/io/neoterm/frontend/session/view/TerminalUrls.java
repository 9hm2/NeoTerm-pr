package io.neoterm.frontend.session.view;

import java.util.regex.Pattern;

import io.neoterm.backend.TerminalBuffer;

/**
 * Shared URL detection used both for drawing the underline (TerminalRenderer)
 * and for opening links on tap (TerminalView), so the highlighted span and the
 * tappable span always match exactly.
 */
final class TerminalUrls {

  /** Matches http(s)/ftp and bare www. URLs in terminal text. */
  static final Pattern PATTERN = Pattern.compile(
    "(?:(?:https?|ftp)://|www\\.)[\\w\\-._~:/?#\\[\\]@!$&'()*+,;=%]+",
    Pattern.CASE_INSENSITIVE);

  /** Punctuation that is not part of the URL when it appears at the very end. */
  private static final String TRAILING_PUNCTUATION = ".,;:!?)]}'\"";

  /** Cap how far a logical-line join may extend, so very long buffers can't make
   *  per-frame URL detection walk unbounded. */
  static final int MAX_WRAP_ROWS = 40;

  private TerminalUrls() {
  }

  /**
   * Whether {@code row} continues onto the next physical row for the purpose of
   * joining a wrapped URL: either the terminal soft-wrapped it ({@code
   * getLineWrap}), or the row is full (its last cell isn't blank), which is how a
   * program that hard-wraps a long URL to the terminal width looks.
   */
  static boolean isContinued(TerminalBuffer screen, int row, int columns) {
    if (screen.getLineWrap(row)) return true;
    String last = screen.getSelectedText(columns - 1, row, columns - 1, row);
    return last != null && last.length() > 0 && !" ".equals(last);
  }

  /**
   * Given a regex match [start, end) over {@code text}, return the end index
   * with trailing punctuation trimmed (e.g. the period ending a sentence).
   */
  static int trimmedEnd(CharSequence text, int start, int end) {
    int trimmed = end;
    while (trimmed > start && TRAILING_PUNCTUATION.indexOf(text.charAt(trimmed - 1)) >= 0) {
      trimmed--;
    }
    return trimmed;
  }
}
