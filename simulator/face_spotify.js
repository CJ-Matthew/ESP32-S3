// ── Spotify "Now Playing" Face ────────────────────────────────────────────────
// Layout (64×32):
//   Left  x=1-30  y=1-30   30×30 album cover  (1 px margin off every edge)
//   Right x=34-63 (30 px wide), all text in a compact 3×5 font:
//     title   y=2    white, marquee-scrolls ONLY when it overflows the window
//     artist  y=10   muted, marquee-scrolls likewise
//     time    y=18   elapsed "m:ss" (green) + 3-bar equaliser flush right
//     bar     y=28   progress bar, Spotify-green fill + brighter playhead
//
// ── How this maps to the ESP32 ────────────────────────────────────────────────
// The album cover is NOT decoded on the ESP32. The backend downloads the Spotify
// JPEG, resizes it to 30×30 with Pillow, and sends raw RGB565 bytes; the firmware
// just blits those 900 uint16s — exactly what `blitCover()` does. Here we generate
// a mock cover per track into the same Uint16Array format, so the path is identical.
// Track metadata arrives as small JSON from GET /spotify/state every ~3-5 s; the
// elapsed time is interpolated from millis() between polls (see resolveState) so the
// bar sweeps smoothly. In the simulator a mock playlist drives it (→/← to skip).
{
  const GREEN   = [ 29, 185,  84];   // Spotify green
  const WHITE   = [235, 235, 235];
  const MUTED   = [120, 150, 130];
  const TRACK_C = [ 45,  55,  50];   // unfilled progress track

  const ART = 30;                    // album cover size (px), inset 1 px off edges
  const RX = 34, RW = 30;            // right column: x start, width
  const SCROLL_SPEED = 14;           // marquee px/sec
  const SCROLL_GAP   = 8;            // px between marquee repeats

  // ── Compact 3×5 font ─────────────────────────────────────────────────────
  // Uppercase glyphs only (lowercase maps to these — small-caps). Each glyph is
  // 5 rows × 3 cols; advance is 4 px (3 + 1 gap). Unknown chars render blank.
  const G = {
    ' ': ['...','...','...','...','...'],
    '0': ['###','#.#','#.#','#.#','###'], '1': ['.#.','##.','.#.','.#.','###'],
    '2': ['##.','..#','.#.','#..','###'], '3': ['##.','..#','.#.','..#','##.'],
    '4': ['#.#','#.#','###','..#','..#'], '5': ['###','#..','##.','..#','##.'],
    '6': ['.##','#..','###','#.#','###'], '7': ['###','..#','.#.','.#.','.#.'],
    '8': ['###','#.#','###','#.#','###'], '9': ['###','#.#','###','..#','##.'],
    'A': ['.#.','#.#','###','#.#','#.#'], 'B': ['##.','#.#','##.','#.#','##.'],
    'C': ['.##','#..','#..','#..','.##'], 'D': ['##.','#.#','#.#','#.#','##.'],
    'E': ['###','#..','##.','#..','###'], 'F': ['###','#..','##.','#..','#..'],
    'G': ['.##','#..','#.#','#.#','.##'], 'H': ['#.#','#.#','###','#.#','#.#'],
    'I': ['###','.#.','.#.','.#.','###'], 'J': ['..#','..#','..#','#.#','.#.'],
    'K': ['#.#','#.#','##.','#.#','#.#'], 'L': ['#..','#..','#..','#..','###'],
    'M': ['#.#','###','###','#.#','#.#'], 'N': ['#.#','##.','###','.##','#.#'],
    'O': ['.#.','#.#','#.#','#.#','.#.'], 'P': ['##.','#.#','##.','#..','#..'],
    'Q': ['.#.','#.#','#.#','###','.##'], 'R': ['##.','#.#','##.','#.#','#.#'],
    'S': ['.##','#..','.#.','..#','##.'], 'T': ['###','.#.','.#.','.#.','.#.'],
    'U': ['#.#','#.#','#.#','#.#','###'], 'V': ['#.#','#.#','#.#','#.#','.#.'],
    'W': ['#.#','#.#','###','###','#.#'], 'X': ['#.#','#.#','.#.','#.#','#.#'],
    'Y': ['#.#','#.#','.#.','.#.','.#.'], 'Z': ['###','..#','.#.','#..','###'],
    '.': ['...','...','...','...','.#.'], ',': ['...','...','...','.#.','#..'],
    "'": ['.#.','.#.','...','...','...'], '-': ['...','...','###','...','...'],
    ':': ['...','.#.','...','.#.','...'], '!': ['.#.','.#.','.#.','...','.#.'],
    '?': ['##.','..#','.#.','...','.#.'], '&': ['.#.','#.#','.#.','#.#','.##'],
    '/': ['..#','..#','.#.','#..','#..'], '(': ['.#.','#..','#..','#..','.#.'],
    ')': ['.#.','..#','..#','..#','.#.'], '+': ['...','.#.','###','.#.','...'],
  };
  const GW = 4;                       // per-glyph advance (3 + 1 gap)

  // Draw text in the 3×5 font, clipped to [clipL, clipR] so scrolling text never
  // bleeds over the cover. (No cover-over-bleed hack needed — we clip exactly.)
  function drawTiny(d, str, x, y, color, clipL, clipR) {
    const c = d.color565(...color);
    let cx = x;
    for (const ch of str) {
      const g = G[ch.toUpperCase()] || G[' '];
      for (let row = 0; row < 5; row++)
        for (let col = 0; col < 3; col++)
          if (g[row][col] === '#') {
            const px = cx + col;
            if (px >= clipL && px <= clipR) d.drawPixel(px, y + row, c);
          }
      cx += GW;
    }
  }
  const textW = str => str.length * GW;   // pixel width of a string

  function marquee(d, str, y, color) {
    if (textW(str) <= RW) { drawTiny(d, str, RX, y, color, RX, 63); return; }  // fits → static
    const period = textW(str) + SCROLL_GAP;
    const off = (millis() * SCROLL_SPEED / 1000) % period;
    const base = RX - off;
    drawTiny(d, str, Math.round(base),          y, color, RX, 63);   // two copies →
    drawTiny(d, str, Math.round(base + period), y, color, RX, 63);   //   seamless wrap
  }

  // ── Colour helpers ──────────────────────────────────────────────────────────
  function rgb565(r, g, b) { return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3); }
  function hsl(h, s, l) {                     // h 0-360, s/l 0-1 → [r,g,b] 0-255
    h = ((h % 360) + 360) % 360 / 360;
    const a = s * Math.min(l, 1 - l);
    const f = n => {
      const k = (n + h * 12) % 12;
      return Math.round(255 * (l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1))));
    };
    return [f(0), f(8), f(4)];
  }

  // ── Mock album covers ─────────────────────────────────────────────────────
  // Each returns an ART×ART Uint16Array of RGB565 — the exact buffer the backend
  // will ship. Five archetypes so tracks look visibly different.
  function makeCover(idx, baseHue) {
    const buf = new Uint16Array(ART * ART);
    const put = (x, y, c) => { if (x >= 0 && x < ART && y >= 0 && y < ART) buf[y * ART + x] = c; };
    const mid = (ART - 1) / 2;
    const kind = idx % 5;

    if (kind === 0) {                         // diagonal gradient + accent slash
      for (let y = 0; y < ART; y++)
        for (let x = 0; x < ART; x++) put(x, y, rgb565(...hsl(baseHue + (x + y) * 3.2, 0.62, 0.48)));
      for (let x = 0; x < ART; x++) {
        const y = Math.round(x * 0.7) + 4;
        put(x, y, rgb565(...hsl(baseHue + 180, 0.9, 0.7)));
        put(x, y + 1, rgb565(...hsl(baseHue + 180, 0.9, 0.55)));
      }
    } else if (kind === 1) {                  // vinyl — concentric rings on dark
      const c1 = hsl(baseHue, 0.7, 0.5), c2 = hsl(baseHue + 30, 0.6, 0.35);
      for (let y = 0; y < ART; y++)
        for (let x = 0; x < ART; x++) {
          const dst = Math.hypot(x - mid, y - mid);
          put(x, y, dst > mid ? rgb565(14, 14, 18) : rgb565(...(Math.floor(dst / 2) % 2 ? c1 : c2)));
        }
      for (let y = 13; y <= 16; y++) for (let x = 13; x <= 16; x++) put(x, y, rgb565(...hsl(baseHue + 180, 0.9, 0.6)));
    } else if (kind === 2) {                  // sunset — vertical gradient + sun
      for (let y = 0; y < ART; y++) {
        const t = y / (ART - 1), c = rgb565(...hsl(baseHue + t * 60, 0.75, 0.6 - t * 0.35));
        for (let x = 0; x < ART; x++) put(x, y, c);
      }
      for (let y = 0; y < ART; y++) for (let x = 0; x < ART; x++)
        if (Math.hypot(x - 15, y - 11) < 6) put(x, y, rgb565(...hsl(baseHue - 10, 0.9, 0.72)));
    } else if (kind === 3) {                  // colour blocks
      const pal = [hsl(baseHue, 0.7, 0.5), hsl(baseHue + 120, 0.7, 0.5),
                   hsl(baseHue + 210, 0.7, 0.5), hsl(baseHue + 60, 0.7, 0.55)];
      for (let y = 0; y < ART; y++) for (let x = 0; x < ART; x++)
        put(x, y, rgb565(...pal[(Math.floor(x / 8) + Math.floor(y / 8) * 2) % pal.length]));
    } else {                                  // waves
      for (let y = 0; y < ART; y++) for (let x = 0; x < ART; x++) {
        const band = Math.floor((y + Math.sin(x / 4 + y / 6) * 3) / 4);
        put(x, y, rgb565(...hsl(baseHue + band * 22, 0.6, 0.5)));
      }
    }
    return buf;
  }
  function blitCover(d, buf) {                // 1 px inset — never touches the edges
    for (let y = 0; y < ART; y++)
      for (let x = 0; x < ART; x++) d.drawPixel(1 + x, 1 + y, buf[y * ART + x]);
  }

  // ── Mock playlist (stand-in for GET /spotify/state) ────────────────────────
  const MOCK = [
    { title: 'Blinding Lights',   artist: 'The Weeknd',   dur: 200000, hue:   0 },
    { title: 'Nights',            artist: 'Frank Ocean',  dur: 307000, hue: 265 },
    { title: 'Bohemian Rhapsody', artist: 'Queen',        dur: 354000, hue:  40 },
    { title: 'As It Was',         artist: 'Harry Styles', dur: 167000, hue: 190 },
    { title: 'Levitating',        artist: 'Dua Lipa',     dur: 203000, hue: 320 },
  ];
  MOCK.forEach((t, i) => { t.cover = makeCover(i, t.hue); });

  let mockIdx = 0, mockTrackStart = null, mockMode = 'playing', mockPausedElapsed = 0, keysBound = false;
  function nextTrack(dir) { mockIdx = (mockIdx + dir + MOCK.length) % MOCK.length; mockTrackStart = millis(); mockMode = 'playing'; }
  function cycleMode() {                       // 's' cycles playing → paused → stopped
    if (mockMode === 'playing') { mockPausedElapsed = millis() - mockTrackStart; mockMode = 'paused'; }
    else if (mockMode === 'paused') { mockMode = 'stopped'; }
    else { mockMode = 'playing'; mockTrackStart = millis() - mockPausedElapsed; }
  }
  function mockState() {
    if (mockTrackStart === null) mockTrackStart = millis() - 12000;
    if (mockMode === 'stopped') return { active: false, isPlaying: false };   // nothing playing
    const t = MOCK[mockIdx];
    let elapsed = mockMode === 'paused' ? mockPausedElapsed : millis() - mockTrackStart;
    if (mockMode === 'playing' && elapsed >= t.dur) { nextTrack(1); return mockState(); }
    return { active: true, title: t.title, artist: t.artist, durationMs: t.dur,
             progressMs: elapsed, isPlaying: mockMode === 'playing', cover: t.cover };
  }
  // Live path (wired later): window.spotifyState uses the same shape + updatedAtMillis.
  //   active:false → nothing playing (idle screen); isPlaying:false + active:true → paused.
  function resolveState() {
    const live = window.spotifyState;
    if (live) {
      if (live.active === false || !live.title) return { active: false, isPlaying: false };
      const drift = live.isPlaying ? (millis() - (live.updatedAtMillis || millis())) : 0;
      return { ...live, active: true, progressMs: Math.min(live.durationMs, live.progressMs + drift) };
    }
    return mockState();
  }

  function mss(ms) { const s = Math.floor(ms / 1000); return Math.floor(s / 60) + ':' + String(s % 60).padStart(2, '0'); }

  function drawEqualiser(d, x, baseY, playing) {
    const c = d.color565(...GREEN);
    for (let i = 0; i < 3; i++) {
      const h = playing ? 1 + Math.round((Math.sin(millis() / (140 + i * 55) + i * 1.7) * 0.5 + 0.5) * 5) : 1;
      d.drawFastVLine(x + i * 2, baseY - h + 1, h, c);
    }
  }
  const mul = (rgb, f) => rgb.map(v => Math.round(v * f));   // dim a colour

  function drawPause(d, x, y) {                 // two-bar pause glyph (shown when paused)
    const c = d.color565(...MUTED);
    d.fillRect(x, y, 1, 5, c);
    d.fillRect(x + 2, y, 1, 5, c);
  }

  // Nothing playing → a calm centred music note + label (Spotify closed / idle).
  function drawIdle(d) {
    const g = d.color565(...mul(GREEN, 0.85));
    d.fillCircle(29, 15, 2, g);                 // note head
    d.drawFastVLine(31, 7, 9, g);               // stem
    d.drawFastHLine(31, 7, 4, g);               // flag
    d.drawPixel(34, 8, g);
    drawTiny(d, 'NOT PLAYING', 10, 22, mul(MUTED, 1), 0, 63);
  }

  // ── Face ────────────────────────────────────────────────────────────────────
  registerFace('Spotify', {
    setup(d) {
      if (!keysBound) {                        // simulator-only shortcuts
        window.addEventListener('keydown', (e) => {
          if (e.key === 'ArrowRight') nextTrack(1);      // skip fwd
          if (e.key === 'ArrowLeft')  nextTrack(-1);     // skip back
          if (e.key === 's' || e.key === 'S') cycleMode(); // playing → paused → stopped
        });
        keysBound = true;
      }
    },

    loop(d) {
      const st = resolveState();
      d.clearScreen();

      if (!st.active) { drawIdle(d); return; }   // nothing playing → idle screen

      const frac = st.durationMs ? Math.min(1, st.progressMs / st.durationMs) : 0;
      const dimF = st.isPlaying ? 1 : 0.5;       // paused → dim the whole face

      blitCover(d, st.cover);                    // cover first; text is clipped, so order is free
      marquee(d, st.title,  2, mul(WHITE, dimF));   // separate lines, scroll only if overflowing
      marquee(d, st.artist, 8, mul(MUTED, dimF));

      drawTiny(d, mss(st.progressMs), RX, 22, mul(st.isPlaying ? GREEN : MUTED, 1), RX, 63);  // timer, 1px gap above bar
      if (st.isPlaying) drawEqualiser(d, 57, 26, true);
      else              drawPause(d, 58, 22);

      d.drawFastHLine(RX, 29, RW, d.color565(...TRACK_C));                              // progress (1 px)
      const fillW = Math.round(RW * frac);
      if (fillW > 0) d.drawFastHLine(RX, 29, fillW, d.color565(...mul(st.isPlaying ? GREEN : MUTED, 1)));
      d.drawFastVLine(RX + Math.min(RW - 1, fillW), 28, 2, d.color565(120, 255, 160));  // playhead
    },
  });
}
