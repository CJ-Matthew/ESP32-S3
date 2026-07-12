// ── Starfield + Moon Phase ────────────────────────────────────────────────────
// A slow night sky for the bedside: parallax stars twinkling in depth, the real
// moon rendered at tonight's ACTUAL phase (computed from the date — no network),
// an occasional shooting star, and weather that reaches the sky — clouds drift
// across when it's cloudy, a light drizzle falls when it's rainy. A dim HH:MM in
// the corner keeps it glanceable at night.
{
  const W = 64, H = 32;

  // ── Moon geometry ──
  const MOON_CX = 50, MOON_CY = 9, MOON_R = 5;   // top-right, ~11px disc

  // Southern hemisphere (Sydney): the waxing moon is lit on the LEFT, not the
  // right — flip the terminator so the phase matches what's actually out the window.
  const SOUTHERN = true;

  // The synodic month (new moon → new moon) and a known reference new moon in UTC.
  // age = (now − epoch) mod SYNODIC gives where we are in the cycle.
  const SYNODIC        = 29.530588853;                  // days
  const NEW_MOON_EPOCH = Date.UTC(2000, 0, 6, 18, 14);  // 2000-01-06 18:14 UTC

  // Illumination phase 0..1 for a given time: 0 = new, 0.5 = full, → 1 back to new.
  function moonPhase(nowMs) {
    let p = ((nowMs - NEW_MOON_EPOCH) / 86400000) % SYNODIC / SYNODIC;
    return p < 0 ? p + 1 : p;
  }

  // ── Stars ──
  const STAR_COUNT = 55;
  // Three parallax depth layers: nearer stars are brighter, drift faster, and
  // twinkle harder; far stars are dim and nearly still. Gives the sky depth.
  const LAYER_SPEED   = [0.12, 0.3, 0.6];   // px / second horizontal drift
  const LAYER_BRIGHT  = [60,   110,  190];  // peak brightness
  const LAYER_TWINKLE = [0.15, 0.30, 0.50]; // twinkle depth (0 = steady)
  const stars = [];                          // {x, y, layer, base, phase, rate, tint}

  // ── Shooting star (occasional) ──
  let shoot     = null;   // {x0, y0, vx, vy, t0, dur} while streaking, else null
  let nextShoot = 0;      // millis() at which the next one may appear

  // Multiply an RGB by a factor (twinkle / weather-dim / fade) → a clamped 565.
  function scol(display, r, g, b, f) {
    return display.color565(
      constrain((r * f) | 0, 0, 255),
      constrain((g * f) | 0, 0, 255),
      constrain((b * f) | 0, 0, 255),
    );
  }

  // Draw the moon at `phase`, dimmed by `dim`. Lit side is pale white; the dark
  // limb is a faint blue-grey "earthshine" so the whole disc stays visible.
  function drawMoon(display, phase, dim) {
    const a      = Math.cos(2 * PI * phase);   // terminator bulge: +1 new … −1 full
    const waxing = phase < 0.5;
    // Craters — a few lit pixels knocked down a touch for texture.
    const craters = [[-2, -1], [1, 1], [0, 2], [2, -2]];

    for (let dy = -MOON_R; dy <= MOON_R; dy++) {
      const hw = Math.sqrt(MOON_R * MOON_R - dy * dy);   // disc half-width at this row
      for (let dx = -MOON_R; dx <= MOON_R; dx++) {
        if (dx * dx + dy * dy > MOON_R * MOON_R) continue;   // outside the disc
        const px  = SOUTHERN ? -dx : dx;                     // flip lit side for Sydney
        const xt  = a * hw;                                  // terminator x at this row
        const lit = waxing ? (px >= xt) : (px <= -xt);
        const X = MOON_CX + dx, Y = MOON_CY + dy;
        if (lit) {
          let f = 1;
          for (const [cx, cy] of craters) if (cx === dx && cy === dy) f = 0.72;
          display.drawPixel(X, Y, scol(display, 235, 238, 248, f * dim));
        } else {
          display.drawPixel(X, Y, scol(display, 30, 35, 55, dim));   // earthshine
        }
      }
    }
  }

  // A soft grey cloud blob centred at (cx, cy) — reused for the cloudy sky.
  function cloudBlob(display, cx, cy) {
    const g  = display.color565(118, 122, 134);
    const hi = display.color565(150, 154, 165);
    display.fillRect(cx - 1, cy, 9, 3, g);
    display.fillCircle(cx,     cy,     2, g);
    display.fillCircle(cx + 3, cy,     2, g);
    display.fillCircle(cx + 6, cy + 1, 2, g);
    display.fillCircle(cx + 2, cy - 1, 2, hi);   // lit crown
  }

  registerFace('Starfield', {
    setup(display) {
      stars.length = 0;
      for (let i = 0; i < STAR_COUNT; i++) {
        const layer = random(0, 3);
        // Colour: mostly white, some cool blue, a few warm — a real sky isn't grey.
        const r = random(0, 100);
        const tint = r < 70 ? [1, 1, 1]
                   : r < 88 ? [0.72, 0.82, 1]
                            : [1, 0.90, 0.78];
        stars.push({
          x:     random(0, W),
          y:     random(0, H),
          layer,
          base:  LAYER_BRIGHT[layer] * (0.6 + random(0, 40) / 100),
          phase: random(0, 628) / 100,
          rate:  (80 + random(0, 240)) / 100,    // twinkle speed
          tint,
        });
      }
      shoot     = null;
      nextShoot = millis() + 3000 + random(0, 6000);   // first one appears soon
    },

    loop(display) {
      const t   = millis() / 1000;
      const now = millis();
      display.fillScreen(0);

      // Weather reaches the sky: rain darkens it most, cloud a little.
      const dim = weatherMode === 'rainy'  ? 0.45
                : weatherMode === 'cloudy' ? 0.80
                : 1.0;

      // ── Stars (parallax drift + per-star twinkle) ──
      for (const s of stars) {
        const x = Math.round((((s.x - LAYER_SPEED[s.layer] * t) % W) + W) % W);
        const y = s.y;
        const depth = LAYER_TWINKLE[s.layer];
        const tw = 1 - depth + depth * (0.5 + 0.5 * Math.sin(t * s.rate + s.phase));
        const f  = s.base * tw * dim;
        display.drawPixel(x, y, scol(display, 255 * s.tint[0], 255 * s.tint[1], 255 * s.tint[2], f / 255));
      }

      // ── Moon at tonight's real phase (drawn over stars) ──
      drawMoon(display, moonPhase(Date.now()), dim);

      // ── Shooting star ──
      if (!shoot && now >= nextShoot) {
        const dir = random(0, 2) === 0 ? 1 : -1;         // down-right or down-left
        const x0  = dir === 1 ? random(2, 30) : random(34, 62);
        shoot = { x0, y0: random(2, 10), vx: dir * 20, vy: 9, t0: now, dur: 500 + random(0, 300) };
        nextShoot = now + shoot.dur + 12000 + random(0, 22000);   // long quiet gap after
      }
      if (shoot) {
        const p = (now - shoot.t0) / shoot.dur;
        if (p >= 1) {
          shoot = null;
        } else {
          for (let k = 0; k < 4; k++) {                  // head + fading tail
            const q = p - k * 0.06;
            if (q < 0) continue;
            const tx = Math.round(shoot.x0 + shoot.vx * q);
            const ty = Math.round(shoot.y0 + shoot.vy * q);
            display.drawPixel(tx, ty, scol(display, 255, 255, 255, (1 - p) * (1 - k * 0.28)));
          }
        }
      }

      // ── Cloudy: slow drifting blobs that pass over the sky ──
      if (weatherMode === 'cloudy') {
        const drift = t * 4;                             // px / second
        cloudBlob(display, ((drift) % (W + 24)) - 12, 6);
        cloudBlob(display, ((drift + 34) % (W + 24)) - 12, 13);
      }

      // ── Rainy: a light diagonal drizzle over a darkened sky ──
      if (weatherMode === 'rainy') {
        for (let i = 0; i < 12; i++) {
          const speed = 0.9 + (i % 3) * 0.2;
          const phase = ((t * speed) + i * 0.37) % 1;
          const x = (i * 7 + 3) % W;
          const y = phase * H;
          const drop = display.color565(70, 130, 210);
          display.drawPixel(x | 0, y | 0, drop);
          display.drawPixel((x + 1) | 0, (y + 1) | 0, display.color565(45, 90, 150));
        }
      }

      // ── Dim HH:MM in the corner so it's still a bedside clock at night ──
      const d  = new Date();
      const hh = String(d.getHours()).padStart(2, '0');
      const mm = String(d.getMinutes()).padStart(2, '0');
      display.setTextSize(1);
      display.setTextColor(scol(display, 120, 140, 180, dim));
      display.setCursor(2, 25);
      display.print(hh + ':' + mm);
    },
  });
}
