// MatrixDisplay — browser equivalent of the MrCodetastic HUB75 DMA library.
// API mirrors dma_display->* so you can copy drawing calls from your .ino
// with only s/dma_display->/display./ substitution.

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
  // Rendered via an offscreen canvas. Character width is ~6px at textSize 1,
  // matching Adafruit GFX spacing. Font shape differs from the hardware glcdfont.c.

  setCursor(x, y)      { this._cx = x; this._cy = y; }
  setTextColor(color)  { this._textColor = this._c565(color); }
  setTextSize(size)    { this._textSize = Math.max(1, size); }

  print(text) {
    const s = this._textSize;
    const offW = text.length * 6 * s + 2;
    const offH = 8 * s + 2;

    const off = document.createElement('canvas');
    off.width  = offW;
    off.height = offH;
    const ctx2 = off.getContext('2d');
    ctx2.fillStyle = '#000';
    ctx2.fillRect(0, 0, offW, offH);
    ctx2.fillStyle = '#fff';
    ctx2.font = `bold ${Math.round(6.5 * s)}px monospace`;
    ctx2.fillText(text, 0, Math.round(6.5 * s));

    const data = ctx2.getImageData(0, 0, offW, offH).data;
    const [r, g, b] = this._textColor;
    for (let py = 0; py < offH; py++)
      for (let px = 0; px < offW; px++)
        if (data[(py * offW + px) * 4] > 100)
          this._put(this._cx + px, this._cy + py, [r, g, b]);

    this._cx += offW - 2;
  }

  println(text = '') {
    this.print(text);
    this._cx  = 0;
    this._cy += 8 * this._textSize;
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
