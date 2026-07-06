// ── Clock Face A ──────────────────────────────────────────────────────────────
// Layout (64×32):
//   A  y= 2-12  HH:MM:SS — one line, printScaled(1,11/7): 48 px wide × 11 px tall
//   B  x=0-47 y=14-23  grass (green over brown) + house critters — one per person
//               home (max 5; count/colour from window.houseRoster, else mockPeople)
//   C  y=25-31  DAY DD MON  size 1
//   D  x=50-63 y=0-11   weather icon (top-right)
//   E  x=50-63 y=13-21  temperature readout "NN°" (under weather; separate from B)
{
  const DAYS   = ['SUN','MON','TUE','WED','THU','FRI','SAT'];
  const MONTHS = ['JAN','FEB','MAR','APR','MAY','JUN',
                  'JUL','AUG','SEP','OCT','NOV','DEC'];

  function pad2(n) { return String(n).padStart(2, '0'); }

  // Weather-relative palette — the time (H/M/S), temperature and date all shift
  // with the current condition so the whole face reads as one mood. Within each
  // theme the H/M/S hues stay distinct.
  const WEATHER_PALETTE = {
    sunny: {                        // warm golden hour
      hh:   [255, 200,  60], mm: [255, 140,  50], ss: [255,  96,  92],
      temp: [255, 170,  45], date: [255, 210, 120],
    },
    cloudy: {                       // soft muted steel
      hh:   [150, 180, 215], mm: [190, 185, 205], ss: [140, 200, 195],
      temp: [175, 190, 205], date: [205, 212, 222],
    },
    rainy: {                        // cool rain blues
      hh:   [ 70, 205, 215], mm: [ 85, 150, 255], ss: [150, 120, 255],
      temp: [ 90, 175, 255], date: [120, 210, 230],
    },
  };
  function weatherPalette() {
    return WEATHER_PALETTE[window.weatherMode] || WEATHER_PALETTE.sunny;
  }

  // ── House critters — one per person home (Wi-Fi presence) ──────────────────
  // Roster comes from window.houseRoster (live via the dev proxy) or, when that's
  // unavailable (e.g. opened as file://), a mock count in window.mockPeople.
  // Each person is a small 4×5 critter that wanders the grass: idle, sit, walk, hop.
  const MOCK_PALETTE = ['#ba66ff', '#4dd2ff', '#ffd54d', '#ff7a7a', '#5cff9d', '#ff9f45'];
  const MAX_PEOPLE = 5;                    // cap critters shown
  const GRASS_L = 0, GRASS_R = 48;         // grass span — left region only (clear of E/temp)
  const GRASS_TOP = 22, GRASS_BOT = 23;    // two full rows: green over brown
  const GROUND_Y  = 21;                    // critter feet rest here (on the grass)
  const CW = 4;                            // critter width
  const LANES = 3;                         // draw-order (z) buckets — stable front/back for clean occlusion
  const ROAM_L = 1, ROAM_R = GRASS_R;      // wander bounds, kept within the grass
  const TALLY_Y = 14;                      // colour-tally row (top of B, above the critters)
  const CRITTER_TOP = 15;                  // critters never rise above this row (keeps tally/time clear)

  function hexToColor(d, hex) {
    const m = /^#?([0-9a-f]{6})$/i.exec(hex || '');
    if (!m) return d.color565(200, 200, 200);
    const n = parseInt(m[1], 16);
    return d.color565((n >> 16) & 255, (n >> 8) & 255, n & 255);
  }

  // Resolve the current roster to [{ id, colour }] — live if present, else mock.
  // Capped at MAX_PEOPLE critters.
  function resolveRoster() {
    let list;
    if (Array.isArray(window.houseRoster)) {
      list = window.houseRoster.map((p, i) => ({
        id: p.mac_address || p.name || ('p' + i),
        colour: p.colour,
      }));
    } else {
      const n = Math.max(0, Math.min(MAX_PEOPLE, window.mockPeople ?? 1));
      list = Array.from({ length: n }, (_, i) => ({
        id: 'mock' + i,
        colour: MOCK_PALETTE[i % MOCK_PALETTE.length],
      }));
    }
    return list.slice(0, MAX_PEOPLE);
  }

  const critters = new Map();   // id → agent, persists across polls/frames
  let lastTick = null;

  function newAgent(id) {
    return {
      id,
      x: ROAM_L + Math.random() * (ROAM_R - ROAM_L - CW),
      dir: Math.random() < 0.5 ? -1 : 1,
      lane: Math.floor(Math.random() * LANES),   // draw-order bucket (0 = front / drawn on top)
      state: 'idle', stateEnd: 0, vx: 0, jump0: 0,
    };
  }

  function pickState(a, now) {
    const r = Math.random();
    if      (r < 0.25) { a.state = 'idle'; a.stateEnd = now + 800  + Math.random() * 2000; }
    else if (r < 0.50) { a.state = 'sit';  a.stateEnd = now + 1500 + Math.random() * 3000; }
    else if (r < 0.82) { a.state = 'walk'; a.dir = Math.random() < 0.5 ? -1 : 1;
                         a.vx = 7 + Math.random() * 7;
                         a.stateEnd = now + 700 + Math.random() * 1500; }
    else               { a.state = 'jump'; a.jump0 = now; a.stateEnd = now + 520; }
  }

  function updateCritters(now) {
    const dt = lastTick == null ? 0 : Math.min(80, now - lastTick);
    lastTick = now;

    const roster = resolveRoster();
    const ids = new Set(roster.map(p => p.id));
    for (const p of roster) {                       // add newcomers, refresh colour
      let a = critters.get(p.id);
      if (!a) { a = newAgent(p.id); critters.set(p.id, a); }
      a.colour = p.colour;
    }
    for (const id of [...critters.keys()])          // drop people who left
      if (!ids.has(id)) critters.delete(id);

    for (const a of critters.values()) {            // step each agent
      if (now >= a.stateEnd) pickState(a, now);
      if (a.state === 'walk') {
        a.x += a.dir * a.vx * dt / 1000;
        if (a.x <= ROAM_L)          { a.x = ROAM_L;          a.dir =  1; }
        if (a.x >= ROAM_R - CW + 1) { a.x = ROAM_R - CW + 1; a.dir = -1; }
      }
    }
    // No mutual separation: critters share the grass and simply pass in front of /
    // behind one another (see lane-based draw order). The colour tally keeps the
    // count unambiguous even when two briefly overlap.
  }

  function drawGrass(d) {
    const green = d.color565( 55, 170,  70);
    const brown = d.color565(105,  65,  30);
    for (let x = GRASS_L; x <= GRASS_R; x++) {
      d.drawPixel(x, GRASS_TOP, green);   // full green layer
      d.drawPixel(x, GRASS_BOT, brown);   // full brown layer
    }
  }

  function drawCritter(d, a, now) {
    const bodyC = hexToColor(d, a.colour);
    const dark  = d.color565(20, 20, 35);
    const x = Math.round(a.x);
    const sitting = a.state === 'sit';
    let yTop = sitting ? GROUND_Y - 3 : GROUND_Y - 4;   // feet on the grass; sit squats, no legs
    if (a.state === 'jump') {
      const p = constrain((now - a.jump0) / 520, 0, 1);
      yTop -= Math.round(sin(p * PI) * 2);              // little hop arc
    }
    yTop = Math.max(CRITTER_TOP, yTop);                 // never rise into the tally/time rows

    const base = [                                       // ears / head / body
      [0,0],[3,0],
      [0,1],[1,1],[2,1],[3,1],
      [0,2],[3,2],
      [0,3],[1,3],[2,3],[3,3],
    ];
    for (const [dx, dy] of base) d.drawPixel(x + dx, yTop + dy, bodyC);
    d.drawPixel(x + 1, yTop + 2, dark);                 // eyes
    d.drawPixel(x + 2, yTop + 2, dark);

    if (!sitting) {
      let leg = 'idle';
      if      (a.state === 'walk') leg = (Math.floor(a.x / 2) % 2 === 0) ? 'walkA' : 'walkB';
      else if (a.state === 'jump') leg = 'jump';
      const legs = {
        idle:  [[0,4],[3,4]],
        walkA: [[0,4],[2,4]],
        walkB: [[1,4],[3,4]],
        jump:  [[1,4],[2,4]],
      }[leg];
      for (const [dx, dy] of legs) d.drawPixel(x + dx, yTop + dy, bodyC);
    }
  }

  // Colour tally — one pixel per person home, in their colour, top-right of B.
  // A quick "how many are home" read-out that can't be confused by overlap.
  function drawTally(d) {
    const roster = resolveRoster();
    for (let i = 0; i < roster.length; i++) {
      d.drawPixel(GRASS_R - i, TALLY_Y, hexToColor(d, roster[i].colour));
    }
  }

  // Sleepy "zzz" when the house is empty. Each Z is a 3×4 glyph:
  //   XXX / ..X / X.. / XXX  — top bar, diagonal, bottom bar.
  const Z_GLYPH = [[0,0],[1,0],[2,0], [2,1], [0,2], [0,3],[1,3],[2,3]];
  function drawZzz(d) {
    const c = d.color565(120, 140, 180);
    const t = millis() / 700;
    const bases = [[4, 19], [10, 17], [16, 15]];       // rising to the right
    for (let i = 0; i < bases.length; i++) {
      const [bx, by] = bases[i];
      const off = Math.round(sin(t + i * 0.9));         // gentle bob
      for (const [dx, dy] of Z_GLYPH) d.drawPixel(bx + dx, by + off + dy, c);
    }
  }

  // ── Temperature readout (region E, under the weather icon) ─────────────────
  // Placeholder value keyed off the weather mode until a real sensor/API exists.
  function drawTemp(d, pal) {
    const fallback = { cloudy: 19, rainy: 14 }[window.weatherMode] ?? 28;
    const live = window.temperature;                   // live °C from backend /weather, or null
    const hasLive = typeof live === 'number' && Number.isFinite(live);
    const t = hasLive ? Math.round(live) : fallback;
    const label = (t >= 0 && t <= 99) ? pad2(t) : String(t);
    const c = d.color565(...pal.temp);
    d.setTextSize(1);
    d.setTextColor(c);
    d.setCursor(62 - label.length * 6, 15);            // right-align against the degree ring
    d.print(label);
    d.drawCircle(62, 15, 1, c);                        // tiny degree ring
  }

  // ── Weather icons (8×8, centred at cx,cy) ─────────────────────────────────

  function drawSun(d, cx, cy) {
    d.fillCircle(cx, cy, 2, d.color565(255, 200, 0));
    const t = millis() / 8000;
    for (let i = 0; i < 4; i++) {
      const angle = t * PI * 2 + i * (PI / 2);
      d.drawPixel(Math.round(cx + cos(angle) * 3.5),
                  Math.round(cy + sin(angle) * 3.5),
                  d.color565(255, 130, 0));
    }
  }

  function drawCloud(d, cx, cy) {
    // little sun peeking out behind the left of the cloud (drawn first)
    const sx = cx - 3, sy = cy - 1;
    for (let i = 0; i < 3; i++) {
      const angle = PI + i * (PI / 4); // rays fan out left / up-left / down-left
      d.drawPixel(Math.round(sx + cos(angle) * 3.5),
                  Math.round(sy + sin(angle) * 3.5),
                  d.color565(255, 150, 0));
    }
    d.fillCircle(sx, sy, 2, d.color565(255, 200, 0));

    // white cloud in front
    const pulse = 0.85 + 0.15 * sin(millis() / 1500);
    const v = Math.round(255 * pulse);
    const cc = d.color565(v, v, v);
    d.fillCircle(cx - 2, cy - 1, 2, cc);
    d.fillCircle(cx + 1, cy - 2, 2, cc);
    d.fillRect(cx - 3, cy, 7, 2, cc);
  }

  function drawRain(d, cx, cy) {
    const cloudC = d.color565(100, 110, 130);
    d.fillCircle(cx - 2, cy - 1, 2, cloudC);
    d.fillCircle(cx + 1, cy - 2, 2, cloudC);
    d.fillRect(cx - 3, cy, 7, 2, cloudC);
    const t = millis();
    for (let i = 0; i < 3; i++) {
      const phase = ((t / 500) + i * 0.33) % 1;
      d.drawPixel(cx - 2 + i * 2, cy + 2 + Math.round(phase * 5),
                  d.color565(60, 140, 255));
    }
  }

  // ── Main face ─────────────────────────────────────────────────────────────

  registerFace('Clock A', {
    setup(d) {},

    loop(d) {
      d.clearScreen();

      const now   = new Date();
      const h     = now.getHours();
      const m     = now.getMinutes();
      const s     = now.getSeconds();
      const colon = millis() % 1000 < 500
        ? d.color565(255, 255, 255)
        : d.color565(0, 0, 0);

      // ── A: HH:MM:SS — one line, tall-narrow glyphs (x1 wide, 11 px tall) ──
      // Colours follow the weather condition (see WEATHER_PALETTE); H/M/S distinct.
      const pal = weatherPalette();
      const TY = 11 / 7; // render 7-px font 11 px tall
      d.setCursor(2, 2);

      d.setTextColor(d.color565(...pal.hh)); d.printScaled(pad2(h), 1, TY);
      d.setTextColor(colon);                 d.printScaled(':',     1, TY);
      d.setTextColor(d.color565(...pal.mm)); d.printScaled(pad2(m), 1, TY);
      d.setTextColor(colon);                 d.printScaled(':',     1, TY);
      d.setTextColor(d.color565(...pal.ss)); d.printScaled(pad2(s), 1, TY);

      // ── B: house critters on the grass — one per person home ────────────
      updateCritters(millis());
      drawGrass(d);
      if (critters.size === 0) drawZzz(d);
      else for (const a of [...critters.values()].sort((p, q) => q.lane - p.lane))
        drawCritter(d, a, millis());   // back lanes first, front lanes drawn over them
      drawTally(d);

      // ── D: Weather icon — top-right ─────────────────────────────────────
      switch (window.weatherMode) {
        case 'cloudy': drawCloud(d, 58, 7); break;
        case 'rainy':  drawRain(d,  57, 7); break;
        default:       drawSun(d,   57, 7); break;
      }

      // ── E: Temperature — under the weather icon ─────────────────────────
      drawTemp(d, pal);

      // ── C: Date — bottom ────────────────────────────────────────────────
      d.setTextSize(1);
      d.setTextColor(d.color565(...pal.date));    // follows the weather palette
      d.setCursor(2, 25);
      d.print(DAYS[now.getDay()] + ' ' + pad2(now.getDate()) + ' ' + MONTHS[now.getMonth()]);
    }
  });
}
