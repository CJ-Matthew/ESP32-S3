// ── Fireplace ─────────────────────────────────────────────────────────────────
// A single flickering flame rising from a bed of logs and glowing embers,
// centred low on the panel — not a full-screen pixel fire.
{
  const W = 64, H = 32;

  // Flame geometry
  const CX        = 32;   // flame centre column
  const BASE      = 28;   // flame root row (hidden just behind the front log)
  const FLAME_TOP = 10;   // highest row the flame reaches  → ~18 px tall
  const FLAME_W   = 5;    // half-width of the flame base    → ~11 px wide (medium)

  // Localised heat field — only the flame region is ever non-zero.
  const heat = new Float32Array(W * H);

  // Glowing embers scattered across the top of the log pile. Each has its own
  // flicker phase so they pulse independently. Filled in during setup().
  const embers = [];

  function fireColor(display, h) {
    // Hot core is white/yellow, cooling to orange then deep red at the tips.
    let r, g, b;
    if (h < 80) {                 // deep red → red
      r = h * 3.2; g = 0; b = 0;
    } else if (h < 180) {         // red → orange
      r = 255; g = (h - 80) * 1.6; b = 0;
    } else {                      // orange → yellow → white
      r = 255; g = 160 + (h - 180) * 1.1; b = (h - 180) * 1.5;
    }
    return display.color565(
      constrain(r | 0, 0, 255),
      constrain(g | 0, 0, 255),
      constrain(b | 0, 0, 255),
    );
  }

  function drawLogs(display) {
    const body = display.color565(66, 33, 12);   // dark bark
    const end  = display.color565(120, 66, 30);  // lit end-grain
    const ring = display.color565(90, 48, 20);   // growth ring

    // Front log (lower, wider)
    display.fillRect(17, 30, 30, 2, body);
    display.fillCircle(17, 30, 1, end);
    display.fillCircle(46, 30, 1, end);
    display.drawPixel(46, 30, ring);

    // Rear log (higher, shorter, offset left)
    display.fillRect(21, 28, 22, 2, body);
    display.fillCircle(21, 28, 1, end);
    display.fillCircle(42, 28, 1, end);
    display.drawPixel(21, 29, ring);
  }

  // Brick surround geometry: two side pillars + a top lintel frame the firebox.
  const LB = 9;    // left pillar spans x = 0 .. LB-1
  const RB = 55;   // right pillar spans x = RB .. 63
  const TB = 8;    // top lintel spans y = 0 .. TB-1  (mantel shelf sits at TB)

  // Colour of a single brick-wall pixel at absolute (x, y): running-bond brick
  // with darker mortar lines and a little per-brick shade variation.
  function brickPixel(display, x, y) {
    const bH = 4, bW = 8;
    const band   = Math.floor(y / bH);
    const offset = (band % 2) * (bW / 2);       // every other course shifts half a brick
    const inCol  = (x + offset) % bW;
    const inRow  = y % bH;
    if (inRow === bH - 1 || inCol === 0)        // mortar: bottom row + left edge
      return display.color565(60, 50, 46);
    const col   = Math.floor((x + offset) / bW);
    const shade = ((col * 29 + band * 53) % 7) - 3;   // stable −3..3 per brick
    return display.color565(
      constrain(150 + shade * 8, 90, 200),
      constrain(58  + shade * 3, 30, 95),
      constrain(42  + shade * 3, 20, 72),
    );
  }

  function drawSurround(display) {
    for (let y = 0; y < H; y++) {
      for (let x = 0; x < W; x++) {
        if (y < TB || x < LB || x >= RB)
          display.drawPixel(x, y, brickPixel(display, x, y));
      }
    }
    // Mantel shelf: a lighter stone ledge across the top, with a lit front edge.
    display.drawFastHLine(0, TB,     W, display.color565(120, 110, 100));
    display.drawFastHLine(0, TB - 1, W, display.color565(150, 140, 130));
  }

  registerFace('Fireplace', {
    setup(display) {
      heat.fill(0);
      embers.length = 0;
      // Embers sit along the tops of the two logs.
      const spots = [
        [23, 29], [26, 28], [29, 29], [31, 27], [33, 28],
        [35, 27], [37, 28], [40, 29], [28, 28], [43, 28],
        [20, 29], [45, 30],
      ];
      for (const [x, y] of spots) {
        embers.push({ x, y, phase: random(0, 628) / 100, rate: random(30, 90) / 10 });
      }
    },

    loop(display) {
      const t = millis() / 1000;
      display.fillScreen(0);

      // ── Seed the flame base ──
      // A slow side-to-side sway plus a fast flicker gives an organic candle-ish
      // flame. The seed is a parabolic (bell) profile so the base is rounded.
      const sway    = Math.sin(t * 1.7) * 1.4 + Math.sin(t * 3.1) * 0.7;
      const flicker = 0.78 + 0.22 * Math.sin(t * 11) * Math.sin(t * 6.3);
      for (let x = CX - FLAME_W; x <= CX + FLAME_W; x++) {
        const d    = (x - CX - sway) / FLAME_W;
        const bell = Math.max(0, 1 - d * d);
        const v    = bell * flicker * 255 + random(-18, 18);
        heat[BASE * W + x] = constrain(v, 0, 255);
      }

      // ── Rise, lean & cool ──
      // Each cell draws heat from the three cells below, sampled with a lean that
      // grows with height so the flame tip drifts with the sway. Cooling scales
      // with height and horizontal distance so it stays a localised teardrop.
      for (let y = FLAME_TOP; y < BASE; y++) {
        const lean = Math.round(sway * (BASE - y) / (BASE - FLAME_TOP));
        for (let x = CX - FLAME_W - 3; x <= CX + FLAME_W + 3; x++) {
          const bx     = constrain(x - lean, 0, W - 1);
          const below  = heat[(y + 1) * W + bx];
          const belowL = heat[(y + 1) * W + constrain(bx - 1, 0, W - 1)];
          const belowR = heat[(y + 1) * W + constrain(bx + 1, 0, W - 1)];
          let v = below * 0.58 + belowL * 0.22 + belowR * 0.22;
          v -= 1.5 + Math.abs(x - CX) * 1.3 + random(0, 5);   // taller + wider = cooler
          heat[y * W + x] = constrain(v, 0, 255);
        }
      }

      // ── Draw the flame (skip cold cells so the background stays black) ──
      for (let y = FLAME_TOP; y <= BASE; y++) {
        for (let x = CX - FLAME_W - 3; x <= CX + FLAME_W + 3; x++) {
          const h = heat[y * W + x];
          if (h > 14) display.drawPixel(x, y, fireColor(display, h));
        }
      }

      // ── Logs, then embers glowing on top of them ──
      drawLogs(display);
      for (const e of embers) {
        const glow = 0.45 + 0.55 * (0.5 + 0.5 * Math.sin(t * e.rate + e.phase));
        const r = 120 + 135 * glow;
        const g = 20  + 70  * glow;
        display.drawPixel(e.x, e.y, display.color565(r | 0, g | 0, 0));
      }

      // Brick surround + mantel, drawn last so it frames the firebox opening.
      drawSurround(display);
    }
  });
}
