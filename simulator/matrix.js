// MatrixDisplay — browser equivalent of the MrCodetastic HUB75 DMA library.
// API mirrors dma_display->* so you can copy drawing calls from your .ino
// with only s/dma_display->/display./ substitution.

// Adafruit GFX 5×7 bitmap font (glcdfont.c), column-major, LSB = top row.
// 5 bytes per character, ASCII 0x20–0x7E (95 characters, 475 bytes).
// Each byte is one column; bit 0 = topmost pixel, bit 6 = bottom pixel.
const _FONT = new Uint8Array([
  0x00,0x00,0x00,0x00,0x00, // 0x20 ' '
  0x00,0x00,0x5F,0x00,0x00, // 0x21 !
  0x00,0x07,0x00,0x07,0x00, // 0x22 "
  0x14,0x7F,0x14,0x7F,0x14, // 0x23 #
  0x24,0x2A,0x7F,0x2A,0x12, // 0x24 $
  0x23,0x13,0x08,0x64,0x62, // 0x25 %
  0x36,0x49,0x55,0x22,0x50, // 0x26 &
  0x00,0x05,0x03,0x00,0x00, // 0x27 '
  0x00,0x1C,0x22,0x41,0x00, // 0x28 (
  0x00,0x41,0x22,0x1C,0x00, // 0x29 )
  0x14,0x08,0x3E,0x08,0x14, // 0x2A *
  0x08,0x08,0x3E,0x08,0x08, // 0x2B +
  0x00,0x50,0x30,0x00,0x00, // 0x2C ,
  0x08,0x08,0x08,0x08,0x08, // 0x2D -
  0x00,0x60,0x60,0x00,0x00, // 0x2E .
  0x20,0x10,0x08,0x04,0x02, // 0x2F /
  0x3E,0x51,0x49,0x45,0x3E, // 0x30 0
  0x00,0x42,0x7F,0x40,0x00, // 0x31 1
  0x42,0x61,0x51,0x49,0x46, // 0x32 2
  0x21,0x41,0x45,0x4B,0x31, // 0x33 3
  0x18,0x14,0x12,0x7F,0x10, // 0x34 4
  0x27,0x45,0x45,0x45,0x39, // 0x35 5
  0x3C,0x4A,0x49,0x49,0x30, // 0x36 6
  0x01,0x71,0x09,0x05,0x03, // 0x37 7
  0x36,0x49,0x49,0x49,0x36, // 0x38 8
  0x06,0x49,0x49,0x29,0x1E, // 0x39 9
  0x00,0x36,0x36,0x00,0x00, // 0x3A :
  0x00,0x56,0x36,0x00,0x00, // 0x3B ;
  0x08,0x14,0x22,0x41,0x00, // 0x3C <
  0x14,0x14,0x14,0x14,0x14, // 0x3D =
  0x00,0x41,0x22,0x14,0x08, // 0x3E >
  0x02,0x01,0x51,0x09,0x06, // 0x3F ?
  0x32,0x49,0x79,0x41,0x3E, // 0x40 @
  0x7E,0x11,0x11,0x11,0x7E, // 0x41 A
  0x7F,0x49,0x49,0x49,0x36, // 0x42 B
  0x3E,0x41,0x41,0x41,0x22, // 0x43 C
  0x7F,0x41,0x41,0x22,0x1C, // 0x44 D
  0x7F,0x49,0x49,0x49,0x41, // 0x45 E
  0x7F,0x09,0x09,0x09,0x01, // 0x46 F
  0x3E,0x41,0x49,0x49,0x7A, // 0x47 G
  0x7F,0x08,0x08,0x08,0x7F, // 0x48 H
  0x00,0x41,0x7F,0x41,0x00, // 0x49 I
  0x20,0x40,0x41,0x3F,0x01, // 0x4A J
  0x7F,0x08,0x14,0x22,0x41, // 0x4B K
  0x7F,0x40,0x40,0x40,0x40, // 0x4C L
  0x7F,0x02,0x0C,0x02,0x7F, // 0x4D M
  0x7F,0x04,0x08,0x10,0x7F, // 0x4E N
  0x3E,0x41,0x41,0x41,0x3E, // 0x4F O
  0x7F,0x09,0x09,0x09,0x06, // 0x50 P
  0x3E,0x41,0x51,0x21,0x5E, // 0x51 Q
  0x7F,0x09,0x19,0x29,0x46, // 0x52 R
  0x46,0x49,0x49,0x49,0x31, // 0x53 S
  0x01,0x01,0x7F,0x01,0x01, // 0x54 T
  0x3F,0x40,0x40,0x40,0x3F, // 0x55 U
  0x1F,0x20,0x40,0x20,0x1F, // 0x56 V
  0x3F,0x40,0x38,0x40,0x3F, // 0x57 W
  0x63,0x14,0x08,0x14,0x63, // 0x58 X
  0x07,0x08,0x70,0x08,0x07, // 0x59 Y
  0x61,0x51,0x49,0x45,0x43, // 0x5A Z
  0x00,0x7F,0x41,0x41,0x00, // 0x5B [
  0x02,0x04,0x08,0x10,0x20, // 0x5C backslash
  0x00,0x41,0x41,0x7F,0x00, // 0x5D ]
  0x04,0x02,0x01,0x02,0x04, // 0x5E ^
  0x40,0x40,0x40,0x40,0x40, // 0x5F _
  0x00,0x01,0x02,0x04,0x00, // 0x60 `
  0x20,0x54,0x54,0x54,0x78, // 0x61 a
  0x7F,0x48,0x44,0x44,0x38, // 0x62 b
  0x38,0x44,0x44,0x44,0x20, // 0x63 c
  0x38,0x44,0x44,0x48,0x7F, // 0x64 d
  0x38,0x54,0x54,0x54,0x18, // 0x65 e
  0x08,0x7E,0x09,0x01,0x02, // 0x66 f
  0x0C,0x52,0x52,0x52,0x3E, // 0x67 g
  0x7F,0x08,0x04,0x04,0x78, // 0x68 h
  0x00,0x44,0x7D,0x40,0x00, // 0x69 i
  0x20,0x40,0x44,0x3D,0x00, // 0x6A j
  0x7F,0x10,0x28,0x44,0x00, // 0x6B k
  0x00,0x41,0x7F,0x40,0x00, // 0x6C l
  0x7C,0x04,0x18,0x04,0x78, // 0x6D m
  0x7C,0x08,0x04,0x04,0x78, // 0x6E n
  0x38,0x44,0x44,0x44,0x38, // 0x6F o
  0x7C,0x14,0x14,0x14,0x08, // 0x70 p
  0x08,0x14,0x14,0x18,0x7C, // 0x71 q
  0x7C,0x08,0x04,0x04,0x08, // 0x72 r
  0x48,0x54,0x54,0x54,0x20, // 0x73 s
  0x04,0x3F,0x44,0x40,0x20, // 0x74 t
  0x3C,0x40,0x40,0x20,0x7C, // 0x75 u
  0x1C,0x20,0x40,0x20,0x1C, // 0x76 v
  0x3C,0x40,0x30,0x40,0x3C, // 0x77 w
  0x44,0x28,0x10,0x28,0x44, // 0x78 x
  0x0C,0x50,0x50,0x50,0x3C, // 0x79 y
  0x44,0x64,0x54,0x4C,0x44, // 0x7A z
  0x00,0x08,0x36,0x41,0x00, // 0x7B {
  0x00,0x00,0x7F,0x00,0x00, // 0x7C |
  0x00,0x41,0x36,0x08,0x00, // 0x7D }
  0x10,0x08,0x08,0x10,0x08, // 0x7E ~
]);

class MatrixDisplay {
  constructor(canvasId, pixelSize = 12) {
    this.W = 64;
    this.H = 32;
    this.pixelSize = pixelSize;
    this.gap = 1;

    this._canvas = document.getElementById(canvasId);
    const cell = pixelSize + this.gap;
    this._canvas.width  = this.W * cell + this.gap;
    this._canvas.height = this.H * cell + this.gap;
    this._ctx = this._canvas.getContext('2d');

    // Flat RGB buffer — each element is [r, g, b]
    this._buf = Array.from({ length: this.W * this.H }, () => [0, 0, 0]);

    this._cx = 0;
    this._cy = 0;
    this._textColor = [255, 255, 255];
    this._textSize  = 1;

    // Gamma correction LUT.
    // LED panels use roughly linear PWM: value 128 = 50% brightness.
    // Monitors apply ~2.2 gamma: CSS 128 = only ~22% brightness.
    // This LUT pre-corrects values so the sim matches the real panel's brightness.
    this._lut = new Uint8Array(256);
    for (let i = 0; i < 256; i++) {
      this._lut[i] = Math.round(Math.pow(i / 255, 1 / 2.2) * 255);
    }
  }

  // ── Color ──────────────────────────────────────────────────────────────────

  color565(r, g, b) {
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
  }

  _c565(c) {
    return [((c >> 11) & 0x1F) << 3, ((c >> 5) & 0x3F) << 2, (c & 0x1F) << 3];
  }

  // ── Pixel primitives ───────────────────────────────────────────────────────

  _put(x, y, rgb) {
    if (x < 0 || x >= this.W || y < 0 || y >= this.H) return;
    this._buf[y * this.W + x] = rgb;
  }

  drawPixel(x, y, color) {
    this._put(x, y, this._c565(color));
  }

  fillScreen(color) {
    const rgb = this._c565(color);
    for (let i = 0; i < this._buf.length; i++) this._buf[i] = [...rgb];
  }

  clearScreen() {
    this.fillScreen(0);
  }

  // ── Lines ──────────────────────────────────────────────────────────────────

  drawLine(x0, y0, x1, y1, color) {
    const rgb = this._c565(color);
    let dx = Math.abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    let dy = -Math.abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    let err = dx + dy;
    for (;;) {
      this._put(x0, y0, [...rgb]);
      if (x0 === x1 && y0 === y1) break;
      const e2 = 2 * err;
      if (e2 >= dy) { err += dy; x0 += sx; }
      if (e2 <= dx) { err += dx; y0 += sy; }
    }
  }

  drawFastVLine(x, y, h, color) { this.drawLine(x, y, x, y + h - 1, color); }
  drawFastHLine(x, y, w, color) { this.drawLine(x, y, x + w - 1, y, color); }

  // ── Rectangles ─────────────────────────────────────────────────────────────

  drawRect(x, y, w, h, color) {
    this.drawFastHLine(x,         y,         w, color);
    this.drawFastHLine(x,         y + h - 1, w, color);
    this.drawFastVLine(x,         y,         h, color);
    this.drawFastVLine(x + w - 1, y,         h, color);
  }

  fillRect(x, y, w, h, color) {
    const rgb = this._c565(color);
    for (let row = y; row < y + h; row++)
      for (let col = x; col < x + w; col++)
        this._put(col, row, [...rgb]);
  }

  // ── Circles ────────────────────────────────────────────────────────────────

  drawCircle(x0, y0, r, color) {
    const rgb = this._c565(color);
    const _p = (x, y) => this._put(x, y, [...rgb]);
    let x = r, y = 0, err = 0;
    while (x >= y) {
      _p(x0+x,y0+y); _p(x0+y,y0+x); _p(x0-y,y0+x); _p(x0-x,y0+y);
      _p(x0-x,y0-y); _p(x0-y,y0-x); _p(x0+y,y0-x); _p(x0+x,y0-y);
      y++;
      err += 2*y + 1;
      if (err > 0) { x--; err -= 2*x + 1; }
    }
  }

  fillCircle(x0, y0, r, color) {
    for (let dy = -r; dy <= r; dy++) {
      const dx = Math.floor(Math.sqrt(r * r - dy * dy));
      this.drawFastHLine(x0 - dx, y0 + dy, dx * 2 + 1, color);
    }
  }

  // ── Triangles ──────────────────────────────────────────────────────────────

  drawTriangle(x0, y0, x1, y1, x2, y2, color) {
    this.drawLine(x0, y0, x1, y1, color);
    this.drawLine(x1, y1, x2, y2, color);
    this.drawLine(x2, y2, x0, y0, color);
  }

  // ── Text ───────────────────────────────────────────────────────────────────
  // Bitmap font renderer using the Adafruit GFX 5×7 glcdfont.
  // Each character is 5 columns wide; at textSize s, each pixel becomes an s×s block.
  // Advance per character = 6*s (5 cols + 1 gap), matching Adafruit GFX exactly.

  setCursor(x, y)      { this._cx = x; this._cy = y; }
  setTextColor(color)  { this._textColor = this._c565(color); }
  setTextSize(size)    { this._textSize = Math.max(1, size); }

  print(str) {
    const s = this._textSize;
    const [r, g, b] = this._textColor;
    for (let i = 0; i < str.length; i++) {
      const code = str.charCodeAt(i);
      if (code >= 0x20 && code <= 0x7E) {
        const base = (code - 0x20) * 5;
        for (let col = 0; col < 5; col++) {
          const bits = _FONT[base + col];
          for (let row = 0; row < 7; row++) {
            if (bits & (1 << row)) {
              for (let dy = 0; dy < s; dy++)
                for (let dx = 0; dx < s; dx++)
                  this._put(this._cx + col * s + dx, this._cy + row * s + dy, [r, g, b]);
            }
          }
        }
      }
      this._cx += 6 * s;
    }
  }

  println(str = '') {
    this.print(str);
    this._cx  = 0;
    this._cy += 8 * this._textSize;
  }

  // Like print() but with independent x/y pixel scaling.
  // xMul is an integer block scale; yMul may be fractional — the 7-px font is mapped
  // to a target height of round(7 * yMul) px using centre-weighted nearest-neighbour
  // sampling, so a non-integer scale drops/adds its odd row in the middle (e.g.
  // yMul = 13/7 → 13 px tall = 6 + 1 + 6) rather than lopsidedly at the bottom.
  // Integer yMul is identical to plain block doubling.
  // printScaled('HH:MM:SS', 1, 2) → 48 px wide × 14 px tall: fits 64 px wide in one line.
  printScaled(str, xMul = 1, yMul = 1) {
    const [r, g, b] = this._textColor;
    const outH = Math.round(7 * yMul);
    for (let i = 0; i < str.length; i++) {
      const code = str.charCodeAt(i);
      if (code >= 0x20 && code <= 0x7E) {
        const base = (code - 0x20) * 5;
        for (let col = 0; col < 5; col++) {
          const bits = _FONT[base + col];
          for (let oy = 0; oy < outH; oy++) {
            const row = Math.max(0, Math.min(6, Math.round((oy + 0.5) * 7 / outH - 0.5)));
            if (bits & (1 << row)) {
              for (let dx = 0; dx < xMul; dx++)
                this._put(this._cx + col * xMul + dx, this._cy + oy, [r, g, b]);
            }
          }
        }
      }
      this._cx += 5 * xMul + 1; // 5 col pixels + 1 px gap
    }
  }

  // ── Render to canvas ───────────────────────────────────────────────────────

  render() {
    const { _ctx: ctx, W, H, pixelSize: ps, gap, _lut } = this;
    const cell = ps + gap;
    const radius = ps * 0.3; // LEDs have a slight circular lens, not sharp square edges

    ctx.fillStyle = '#080808';
    ctx.fillRect(0, 0, W * cell + gap, H * cell + gap);

    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        const [r, g, b] = this._buf[y * W + x];
        const px = x * cell + gap;
        const py = y * cell + gap;

        if (r === 0 && g === 0 && b === 0) {
          // Dark "off" LED — slightly visible so you can see the grid
          ctx.fillStyle = '#141414';
          ctx.beginPath();
          ctx.roundRect(px, py, ps, ps, radius);
          ctx.fill();
        } else {
          // Gamma-corrected color
          const cr = _lut[r], cg = _lut[g], cb = _lut[b];

          // Bloom: faint halo bleeds into the surrounding gap pixels
          ctx.fillStyle = `rgba(${cr},${cg},${cb},0.22)`;
          ctx.fillRect(px - 1, py - 1, ps + 2, ps + 2);

          // LED face
          ctx.fillStyle = `rgb(${cr},${cg},${cb})`;
          ctx.beginPath();
          ctx.roundRect(px, py, ps, ps, radius);
          ctx.fill();
        }
      }
    }
  }
}
