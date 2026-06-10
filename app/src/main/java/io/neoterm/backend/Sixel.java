package io.neoterm.backend;

/**
 * Minimal Sixel decoder (DEC sixel graphics, as produced by img2sixel/chafa,
 * yazi/ranger image previews, mpv, etc.). Decodes the data part of a
 * {@code DCS <params> q <data> ST} sequence into an ARGB pixel array.
 *
 * <p>Supports: color registers ({@code #Pc;Pu;Px;Py;Pz}, RGB and HLS), color
 * selection ({@code #Pc}), repeat ({@code !Pn}), graphics CR ({@code $}) and LF
 * ({@code -}), raster attributes ({@code "}). Pixels that are never written stay
 * transparent so they blend over the terminal background.
 */
final class Sixel {

  /** Decoded image: ARGB_8888 pixels plus dimensions, or null if empty. */
  static final class Image {
    final int[] pixels;
    final int width;
    final int height;

    Image(int[] pixels, int width, int height) {
      this.pixels = pixels;
      this.width = width;
      this.height = height;
    }
  }

  /** Hard caps so a malformed/huge stream can't exhaust memory on a phone. */
  private static final int MAX_DIMENSION = 4096;
  private static final int MAX_PIXELS = 4096 * 4096;

  private Sixel() {
  }

  static Image decode(String data) {
    final int len = data.length();

    // ---- Pass 1: geometry (max width and height in pixels). ----
    int posX = 0;
    int bandTop = 0;
    int maxWidth = 0;
    int maxHeight = 0;
    for (int i = 0; i < len; ) {
      char c = data.charAt(i);
      if (c >= '?' && c <= '~') {
        int value = c - '?';
        i++;
        // posX advances by 1 here (repeat handled below where '!' is seen).
        posX++;
        if (posX > maxWidth) maxWidth = posX;
        if (value != 0) {
          int highest = 31 - Integer.numberOfLeadingZeros(value);
          if (bandTop + highest + 1 > maxHeight) maxHeight = bandTop + highest + 1;
        }
      } else if (c == '!') {
        // Repeat: "!Pn <sixelchar>".
        int[] r = readInt(data, i + 1);
        int count = Math.max(1, r[0]);
        int j = r[1];
        if (j < len) {
          char sc = data.charAt(j);
          if (sc >= '?' && sc <= '~') {
            int value = sc - '?';
            posX += count;
            if (posX > maxWidth) maxWidth = posX;
            if (value != 0) {
              int highest = 31 - Integer.numberOfLeadingZeros(value);
              if (bandTop + highest + 1 > maxHeight) maxHeight = bandTop + highest + 1;
            }
            j++;
          }
        }
        i = j;
      } else if (c == '$') {
        posX = 0;
        i++;
      } else if (c == '-') {
        posX = 0;
        bandTop += 6;
        i++;
      } else if (c == '#' || c == '"') {
        // Skip color/raster parameter list (digits and semicolons).
        i++;
        while (i < len) {
          char p = data.charAt(i);
          if ((p >= '0' && p <= '9') || p == ';') i++;
          else break;
        }
      } else {
        i++;
      }
    }

    if (maxWidth <= 0 || maxHeight <= 0) return null;
    if (maxWidth > MAX_DIMENSION) maxWidth = MAX_DIMENSION;
    if (maxHeight > MAX_DIMENSION) maxHeight = MAX_DIMENSION;
    if ((long) maxWidth * maxHeight > MAX_PIXELS) return null;

    final int width = maxWidth;
    final int height = maxHeight;
    final int[] pixels = new int[width * height];

    // ---- Pass 2: fill pixels. ----
    final int[] palette = defaultPalette();
    int color = palette[0];
    posX = 0;
    bandTop = 0;
    for (int i = 0; i < len; ) {
      char c = data.charAt(i);
      if (c >= '?' && c <= '~') {
        int value = c - '?';
        plot(pixels, width, height, posX, bandTop, value, color);
        posX++;
        i++;
      } else if (c == '!') {
        int[] r = readInt(data, i + 1);
        int count = Math.max(1, r[0]);
        int j = r[1];
        if (j < len) {
          char sc = data.charAt(j);
          if (sc >= '?' && sc <= '~') {
            int value = sc - '?';
            for (int k = 0; k < count; k++) {
              plot(pixels, width, height, posX, bandTop, value, color);
              posX++;
            }
            j++;
          }
        }
        i = j;
      } else if (c == '$') {
        posX = 0;
        i++;
      } else if (c == '-') {
        posX = 0;
        bandTop += 6;
        i++;
      } else if (c == '#') {
        int[] r = readInt(data, i + 1);
        int pc = r[0] & 0xff;
        int j = r[1];
        if (j < len && data.charAt(j) == ';') {
          // Color definition: #Pc;Pu;Px;Py;Pz
          int[] pu = readInt(data, j + 1);
          int[] px = readInt(data, pu[1] + 1);
          int[] py = readInt(data, px[1] + 1);
          int[] pz = readInt(data, py[1] + 1);
          palette[pc] = (pu[0] == 1)
            ? hlsToArgb(px[0], py[0], pz[0])
            : rgbToArgb(px[0], py[0], pz[0]);
          color = palette[pc];
          j = pz[1];
        } else {
          color = palette[pc];
        }
        i = j;
      } else if (c == '"') {
        // Raster attributes — skip, geometry already known.
        i++;
        while (i < len) {
          char p = data.charAt(i);
          if ((p >= '0' && p <= '9') || p == ';') i++;
          else break;
        }
      } else {
        i++;
      }
    }

    return new Image(pixels, width, height);
  }

  private static void plot(int[] pixels, int width, int height, int x, int bandTop, int value, int color) {
    if (x < 0 || x >= width) return;
    for (int bit = 0; bit < 6; bit++) {
      if ((value & (1 << bit)) != 0) {
        int y = bandTop + bit;
        if (y >= 0 && y < height) pixels[y * width + x] = color;
      }
    }
  }

  /** Read a non-negative integer starting at {@code start}; returns {value, nextIndex}. */
  private static int[] readInt(String s, int start) {
    int i = start;
    int v = 0;
    boolean any = false;
    while (i < s.length()) {
      char c = s.charAt(i);
      if (c >= '0' && c <= '9') {
        v = v * 10 + (c - '0');
        any = true;
        i++;
      } else {
        break;
      }
    }
    return new int[]{any ? v : 0, i};
  }

  private static int rgbToArgb(int r, int g, int b) {
    // Sixel RGB components are 0..100.
    return 0xff000000
      | (clamp255(r * 255 / 100) << 16)
      | (clamp255(g * 255 / 100) << 8)
      | clamp255(b * 255 / 100);
  }

  private static int hlsToArgb(int h, int l, int s) {
    // Sixel HLS: H 0..360 (0 = blue in DEC, but most encoders use RGB anyway),
    // L 0..100, S 0..100. Use a standard HSL conversion (good enough).
    double hue = ((h % 360) + 360) % 360;
    double light = l / 100.0;
    double sat = s / 100.0;
    double cc = (1 - Math.abs(2 * light - 1)) * sat;
    double x = cc * (1 - Math.abs((hue / 60.0) % 2 - 1));
    double m = light - cc / 2;
    double r = 0, g = 0, b = 0;
    if (hue < 60) { r = cc; g = x; }
    else if (hue < 120) { r = x; g = cc; }
    else if (hue < 180) { g = cc; b = x; }
    else if (hue < 240) { g = x; b = cc; }
    else if (hue < 300) { r = x; b = cc; }
    else { r = cc; b = x; }
    return 0xff000000
      | (clamp255((int) Math.round((r + m) * 255)) << 16)
      | (clamp255((int) Math.round((g + m) * 255)) << 8)
      | clamp255((int) Math.round((b + m) * 255));
  }

  private static int clamp255(int v) {
    return v < 0 ? 0 : (v > 255 ? 255 : v);
  }

  /** The 16-color VT340 default palette (used until the stream redefines colors). */
  private static int[] defaultPalette() {
    int[] p = new int[256];
    int[] base = {
      0x000000, 0x2020e0, 0xe02020, 0x20e020, 0xe020e0, 0x20e0e0, 0xe0e020, 0x808080,
      0x404040, 0x4040ff, 0xff4040, 0x40ff40, 0xff40ff, 0x40ffff, 0xffff40, 0xffffff
    };
    for (int i = 0; i < base.length; i++) p[i] = 0xff000000 | base[i];
    return p;
  }
}
