// ── Your sketch ──────────────────────────────────────────────────────────────
// Mirror your Arduino drawing calls here.
//   • display replaces dma_display
//   • display.color565(r, g, b) / display.drawPixel / fillRect / etc. all work
//   • millis(), random(), sin(), cos(), constrain(), map() are available
//   • delay() is a no-op — use millis() for timing:
//       if (millis() - last > 1000) { ...; last = millis(); }
//
// setup(display) — runs once
// loop(display)  — called at the target FPS set in the UI

// ── Fire effect ──────────────────────────────────────────────────────────────
// Classic cellular-automaton fire. Identical algorithm works on ESP32.

const W = 64, H = 32;
const heat = new Uint8Array(W * H);  // on ESP32 this would be: byte heat[W * H]

function setup(display) {
  heat.fill(0);
}

function loop(display) {
  // Seed the bottom row with random heat
  for (let x = 0; x < W; x++) {
    heat[(H - 1) * W + x] = random(200, 255);
  }

  // Propagate upward: each cell = average of cell below + two neighbours below, cooled
  for (let y = 0; y < H - 1; y++) {
    for (let x = 0; x < W; x++) {
      const below  = heat[(y + 1) * W + x];
      const belowL = heat[(y + 1) * W + constrain(x - 1, 0, W - 1)];
      const belowR = heat[(y + 1) * W + constrain(x + 1, 0, W - 1)];
      const avg = ((below + belowL + belowR) / 3) | 0;
      heat[y * W + x] = constrain(avg - random(0, 8), 0, 255);
    }
  }

  // Map heat values to fire colours and draw
  for (let y = 0; y < H; y++) {
    for (let x = 0; x < W; x++) {
      const t = heat[y * W + x];
      // Black → deep red → orange → yellow → white
      let r, g, b;
      if (t < 85) {
        r = (t * 3) | 0; g = 0; b = 0;
      } else if (t < 170) {
        r = 255; g = ((t - 85) * 3) | 0; b = 0;
      } else {
        r = 255; g = 255; b = ((t - 170) * 3) | 0;
      }
      display.drawPixel(x, y, display.color565(r, g, b));
    }
  }
}
