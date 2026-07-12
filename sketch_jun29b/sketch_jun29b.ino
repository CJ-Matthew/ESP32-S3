// ESP32-S3 HUB75 multi-face display — backend-driven face switching.
// See DESIGN.md. First flash = plumbing: WiFi + NTP + two-core render + polling,
// with a bare HH:MM:SS clock and the full fire sim (ported from simulator/sketch.js).
//
// Core 0 (network task): blocking HTTPS polls → writes shared globals. Never draws.
// Core 1 (loop / render): reads shared globals → draws. ONLY this core touches
// dma_display (the HUB75 driver is not thread-safe).

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include "secrets.h"

// ── Panel ────────────────────────────────────────────────────────────────────
static const int PANEL_W = 64;
static const int PANEL_H = 32;
MatrixPanel_I2S_DMA* dma_display = nullptr;

// ── Time ─────────────────────────────────────────────────────────────────────
// Australia/Sydney with DST. NTP gives UTC; this POSIX rule applies the offset.
static const char* TZ_SYDNEY = "AEST-10AEDT,M10.1.0,M4.1.0/3";

// ── Shared state (core 0 writes, core 1 reads) ───────────────────────────────
// Single-word int handoff — no lock needed on the hot path.
volatile int requestedFaceId = 0;   // index into faces[]; default = clock

// Weather + temperature from /weather (network writes, render reads).
volatile int   weatherMode  = 0;    // 0 = sunny, 1 = cloudy, 2 = rainy
volatile float temperatureC = 0.0f;
volatile bool  haveTemp     = false;

// Presence roster from /connected. Written by the network task under rosterMutex,
// copied out by the render task each frame. Colour is pre-parsed from the hex.
static const int MAX_PEOPLE = 5;
struct RosterEntry { char id[18]; uint8_t r, g, b; };
RosterEntry       rosterShared[MAX_PEOPLE];
volatile int      rosterCount = 0;
SemaphoreHandle_t rosterMutex = nullptr;

// ── Face interface ───────────────────────────────────────────────────────────
struct Face {
  const char* id;                              // slug — must match backend + simulator
  void (*setup)();                             // once, when this face becomes active
  void (*loop)(MatrixPanel_I2S_DMA* d);        // every frame
};

// Forward declarations of each face's functions.
void clockSetup();
void clockLoop(MatrixPanel_I2S_DMA* d);
void fireSetup();
void fireLoop(MatrixPanel_I2S_DMA* d);

// Registry. Add a face = write two functions + one entry here + a backend slug.
Face faces[] = {
  { "clock", clockSetup, clockLoop },
  { "fire",  fireSetup,  fireLoop  },
};
static const int FACE_COUNT = sizeof(faces) / sizeof(faces[0]);

static int faceIndexForSlug(const char* slug) {
  for (int i = 0; i < FACE_COUNT; i++)
    if (strcmp(faces[i].id, slug) == 0) return i;
  return -1;
}

// ═════════════════════════════════════════════════════════════════════════════
//  CLOCK FACE  (ported from simulator/face_clock_a.js — weather-driven palette,
//  date, temperature, animated weather icon, grass + presence tally. Wandering
//  critters are added in a later pass.)
// ═════════════════════════════════════════════════════════════════════════════
namespace clockface {
  const char* const DAYS[]   = { "SUN","MON","TUE","WED","THU","FRI","SAT" };
  const char* const MONTHS[] = { "JAN","FEB","MAR","APR","MAY","JUN",
                                 "JUL","AUG","SEP","OCT","NOV","DEC" };

  struct RGB { uint8_t r, g, b; };
  struct Pal { RGB hh, mm, ss, temp, date; };

  // Weather-relative palette (matches WEATHER_PALETTE in the JS): the whole face
  // shifts mood with the condition; H/M/S stay distinct within a theme.
  const Pal PALETTES[3] = {
    // sunny — warm golden hour
    { {255,200,60}, {255,140,50}, {255,96,92},  {255,170,45}, {255,210,120} },
    // cloudy — soft muted steel
    { {150,180,215},{190,185,205},{140,200,195},{175,190,205},{205,212,222} },
    // rainy — cool rain blues
    { {70,205,215}, {85,150,255}, {150,120,255},{90,175,255}, {120,210,230} },
  };

  // Grass / presence layout (region B).
  const int GRASS_L = 0, GRASS_R = 48, GRASS_TOP = 22, GRASS_BOT = 23;
  const int TALLY_Y = 14;

  inline uint16_t col(MatrixPanel_I2S_DMA* d, const RGB& c) {
    return d->color565(c.r, c.g, c.b);
  }

  void drawGrass(MatrixPanel_I2S_DMA* d) {
    uint16_t green = d->color565(55, 170, 70);
    uint16_t brown = d->color565(105, 65, 30);
    for (int x = GRASS_L; x <= GRASS_R; x++) {
      d->drawPixel(x, GRASS_TOP, green);
      d->drawPixel(x, GRASS_BOT, brown);
    }
  }

  // One pixel per person home, in their colour — a "how many home" read-out.
  void drawTally(MatrixPanel_I2S_DMA* d) {
    xSemaphoreTake(rosterMutex, portMAX_DELAY);
    int n = rosterCount;
    for (int i = 0; i < n && i < MAX_PEOPLE; i++) {
      d->drawPixel(GRASS_R - i, TALLY_Y,
                   d->color565(rosterShared[i].r, rosterShared[i].g, rosterShared[i].b));
    }
    xSemaphoreGive(rosterMutex);
  }

  // Sleepy "zzz" when the house is empty. Each Z is a 3×4 glyph.
  void drawZzz(MatrixPanel_I2S_DMA* d) {
    static const int Z[][2] = { {0,0},{1,0},{2,0}, {2,1}, {0,2}, {0,3},{1,3},{2,3} };
    uint16_t c = d->color565(120, 140, 180);
    float t = millis() / 700.0f;
    const int bases[][2] = { {4,19}, {10,17}, {16,15} };
    for (int i = 0; i < 3; i++) {
      int off = (int)roundf(sinf(t + i * 0.9f));
      for (auto& p : Z) d->drawPixel(bases[i][0] + p[0], bases[i][1] + off + p[1], c);
    }
  }

  // ── Weather icons (animated, ~8×8, centred at cx,cy) ──
  void drawSun(MatrixPanel_I2S_DMA* d, int cx, int cy) {
    d->fillCircle(cx, cy, 2, d->color565(255, 200, 0));
    float t = millis() / 8000.0f;
    for (int i = 0; i < 4; i++) {
      float a = t * PI * 2 + i * (PI / 2);
      d->drawPixel((int)roundf(cx + cosf(a) * 3.5f), (int)roundf(cy + sinf(a) * 3.5f),
                   d->color565(255, 130, 0));
    }
  }

  void drawCloud(MatrixPanel_I2S_DMA* d, int cx, int cy) {
    int sx = cx - 3, sy = cy - 1;                 // sun peeking out behind
    for (int i = 0; i < 3; i++) {
      float a = PI + i * (PI / 4);
      d->drawPixel((int)roundf(sx + cosf(a) * 3.5f), (int)roundf(sy + sinf(a) * 3.5f),
                   d->color565(255, 150, 0));
    }
    d->fillCircle(sx, sy, 2, d->color565(255, 200, 0));
    float pulse = 0.85f + 0.15f * sinf(millis() / 1500.0f);
    int v = (int)roundf(255 * pulse);
    uint16_t cc = d->color565(v, v, v);
    d->fillCircle(cx - 2, cy - 1, 2, cc);
    d->fillCircle(cx + 1, cy - 2, 2, cc);
    d->fillRect(cx - 3, cy, 7, 2, cc);
  }

  void drawRain(MatrixPanel_I2S_DMA* d, int cx, int cy) {
    uint16_t cloudC = d->color565(100, 110, 130);
    d->fillCircle(cx - 2, cy - 1, 2, cloudC);
    d->fillCircle(cx + 1, cy - 2, 2, cloudC);
    d->fillRect(cx - 3, cy, 7, 2, cloudC);
    uint32_t t = millis();
    for (int i = 0; i < 3; i++) {
      float phase = fmodf((t / 500.0f) + i * 0.33f, 1.0f);
      d->drawPixel(cx - 2 + i * 2, cy + 2 + (int)roundf(phase * 5), d->color565(60, 140, 255));
    }
  }

  // Temperature readout (region E): "NN°", right-aligned against a degree ring.
  void drawTemp(MatrixPanel_I2S_DMA* d, const Pal& pal) {
    int t;
    if (haveTemp) t = (int)roundf(temperatureC);
    else          t = (weatherMode == 1) ? 19 : (weatherMode == 2) ? 14 : 28;  // fallback
    char label[6];
    snprintf(label, sizeof(label), "%d", t);
    uint16_t c = col(d, pal.temp);
    d->setTextSize(1);
    d->setTextColor(c);
    d->setCursor(61 - (int)strlen(label) * 6, 15);   // right-align against the degree ring
    d->print(label);
    d->drawCircle(62, 15, 1, c);   // tiny degree ring
  }

  // ── House critters — one small sprite per person home (Wi-Fi presence) ──────
  // Each wanders the grass with an idle/sit/walk/jump state machine. Agents
  // persist across polls; the roster (from /connected) drives who's present.
  const int GROUND_Y = 21, CW = 4, LANES = 3;
  const int ROAM_L = 1, ROAM_R = GRASS_R, CRITTER_TOP = 15;

  struct Agent {
    char id[18]; uint8_t r, g, b; bool active;
    float x; int dir; int lane; int state;   // 0 idle, 1 sit, 2 walk, 3 jump
    uint32_t stateEnd; float vx; uint32_t jump0;
  };
  Agent agents[MAX_PEOPLE] = {};
  uint32_t lastTick = 0;

  inline float frand() { return random(0, 10001) / 10000.0f; }   // 0..1

  void initAgent(Agent& a, const char* id) {
    strncpy(a.id, id, sizeof(a.id) - 1); a.id[sizeof(a.id) - 1] = '\0';
    a.x = ROAM_L + frand() * (ROAM_R - ROAM_L - CW);
    a.dir = (random(0, 2) == 0) ? -1 : 1;
    a.lane = random(0, LANES);
    a.state = 0; a.stateEnd = 0; a.vx = 0; a.jump0 = 0; a.active = true;
  }

  void pickState(Agent& a, uint32_t now) {
    float r = frand();
    if      (r < 0.25f) { a.state = 0; a.stateEnd = now + 800  + (uint32_t)(frand() * 2000); }
    else if (r < 0.50f) { a.state = 1; a.stateEnd = now + 1500 + (uint32_t)(frand() * 3000); }
    else if (r < 0.82f) { a.state = 2; a.dir = (random(0, 2) == 0) ? -1 : 1;
                          a.vx = 7 + frand() * 7;
                          a.stateEnd = now + 700 + (uint32_t)(frand() * 1500); }
    else                { a.state = 3; a.jump0 = now; a.stateEnd = now + 520; }
  }

  void updateCritters(uint32_t now) {
    RosterEntry r[MAX_PEOPLE]; int rc;
    xSemaphoreTake(rosterMutex, portMAX_DELAY);
    memcpy(r, rosterShared, sizeof(r)); rc = rosterCount;
    xSemaphoreGive(rosterMutex);

    // Sync agents to roster: refresh present, add newcomers, drop those who left.
    bool keep[MAX_PEOPLE] = { false };
    for (int i = 0; i < rc; i++) {
      int found = -1;
      for (int j = 0; j < MAX_PEOPLE; j++)
        if (agents[j].active && strcmp(agents[j].id, r[i].id) == 0) { found = j; break; }
      if (found < 0)
        for (int j = 0; j < MAX_PEOPLE; j++)
          if (!agents[j].active) { initAgent(agents[j], r[i].id); found = j; break; }
      if (found >= 0) {
        agents[found].r = r[i].r; agents[found].g = r[i].g; agents[found].b = r[i].b;
        keep[found] = true;
      }
    }
    for (int j = 0; j < MAX_PEOPLE; j++)
      if (agents[j].active && !keep[j]) agents[j].active = false;

    // Step each agent (dt clamped so a stalled frame can't teleport anyone).
    float dt = (lastTick == 0) ? 0 : fminf(80.0f, (float)(now - lastTick));
    lastTick = now;
    for (int j = 0; j < MAX_PEOPLE; j++) {
      Agent& a = agents[j];
      if (!a.active) continue;
      if (now >= a.stateEnd) pickState(a, now);
      if (a.state == 2) {
        a.x += a.dir * a.vx * dt / 1000.0f;
        if (a.x <= ROAM_L)          { a.x = ROAM_L;          a.dir =  1; }
        if (a.x >= ROAM_R - CW + 1) { a.x = ROAM_R - CW + 1; a.dir = -1; }
      }
    }
  }

  void drawCritter(MatrixPanel_I2S_DMA* d, Agent& a, uint32_t now) {
    uint16_t bodyC = d->color565(a.r, a.g, a.b);
    uint16_t dark  = d->color565(20, 20, 35);
    int x = (int)roundf(a.x);
    bool sitting = (a.state == 1);
    int yTop = sitting ? GROUND_Y - 3 : GROUND_Y - 4;
    if (a.state == 3) {
      float p = constrain((now - a.jump0) / 520.0f, 0.0f, 1.0f);
      yTop -= (int)roundf(sinf(p * PI) * 2);            // little hop arc
    }
    if (yTop < CRITTER_TOP) yTop = CRITTER_TOP;

    static const int base[][2] = {                       // ears / head / body
      {0,0},{3,0}, {0,1},{1,1},{2,1},{3,1}, {0,2},{3,2}, {0,3},{1,3},{2,3},{3,3},
    };
    for (auto& p : base) d->drawPixel(x + p[0], yTop + p[1], bodyC);
    d->drawPixel(x + 1, yTop + 2, dark);                 // eyes
    d->drawPixel(x + 2, yTop + 2, dark);

    if (!sitting) {
      int legs[2][2];
      if (a.state == 2) {
        if (((int)floorf(a.x / 2) % 2) == 0) { legs[0][0]=0; legs[0][1]=4; legs[1][0]=2; legs[1][1]=4; }
        else                                 { legs[0][0]=1; legs[0][1]=4; legs[1][0]=3; legs[1][1]=4; }
      } else if (a.state == 3)               { legs[0][0]=1; legs[0][1]=4; legs[1][0]=2; legs[1][1]=4; }
      else                                   { legs[0][0]=0; legs[0][1]=4; legs[1][0]=3; legs[1][1]=4; }
      for (auto& l : legs) d->drawPixel(x + l[0], yTop + l[1], bodyC);
    }
  }
}

void clockSetup() {
  // Nothing to seed yet.
}

void clockLoop(MatrixPanel_I2S_DMA* d) {
  using namespace clockface;
  d->fillScreen(0);

  time_t nowSec = time(nullptr);
  struct tm tn;
  localtime_r(&nowSec, &tn);
  bool synced = (tn.tm_year + 1900 >= 2020);

  const Pal& pal = PALETTES[constrain((int)weatherMode, 0, 2)];
  bool colonOn   = millis() % 1000 < 500;
  uint16_t colon = colonOn ? d->color565(255, 255, 255) : 0;

  // ── A: HH:MM:SS — per-segment weather colours, blinking colon ──
  char hh[3], mm[3], ss[3];
  if (synced) {
    snprintf(hh, sizeof(hh), "%02d", tn.tm_hour);
    snprintf(mm, sizeof(mm), "%02d", tn.tm_min);
    snprintf(ss, sizeof(ss), "%02d", tn.tm_sec);
  } else {
    strcpy(hh, "--"); strcpy(mm, "--"); strcpy(ss, "--");
  }
  d->setTextSize(1);
  d->setTextWrap(false);
  d->setCursor(2, 2);
  d->setTextColor(col(d, pal.hh)); d->print(hh);
  d->setTextColor(colon);          d->print(":");
  d->setTextColor(col(d, pal.mm)); d->print(mm);
  d->setTextColor(colon);          d->print(":");
  d->setTextColor(col(d, pal.ss)); d->print(ss);

  // ── B: grass + wandering critters (one per person home) + colour tally ──
  updateCritters(millis());
  drawGrass(d);
  int activeCount = 0;
  for (int j = 0; j < MAX_PEOPLE; j++) if (agents[j].active) activeCount++;
  if (activeCount == 0) {
    drawZzz(d);
  } else {
    // Draw back lanes first so front lanes (lane 0) occlude them cleanly.
    for (int lane = LANES - 1; lane >= 0; lane--)
      for (int j = 0; j < MAX_PEOPLE; j++)
        if (agents[j].active && agents[j].lane == lane) drawCritter(d, agents[j], millis());
  }
  drawTally(d);

  // ── D: weather icon — top-right ──
  if      (weatherMode == 1) drawCloud(d, 58, 7);
  else if (weatherMode == 2) drawRain(d, 57, 7);
  else                       drawSun(d, 57, 7);

  // ── E: temperature — under the weather icon ──
  drawTemp(d, pal);

  // ── C: date — bottom ──
  if (synced) {
    char date[16];
    snprintf(date, sizeof(date), "%s %02d %s",
             DAYS[tn.tm_wday], tn.tm_mday, MONTHS[tn.tm_mon]);
    d->setTextSize(1);
    d->setTextColor(col(d, pal.date));
    d->setCursor(2, 25);
    d->print(date);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  FIRE FACE  (ported from simulator/sketch.js — a single flame + logs + bricks)
// ═════════════════════════════════════════════════════════════════════════════
namespace fire {
  const int W = 64, H = 32;
  const int CX = 32;          // flame centre column
  const int BASE = 28;        // flame root row
  const int FLAME_TOP = 10;   // highest row the flame reaches
  const int FLAME_W = 5;      // half-width of the flame base

  const int LB = 9;           // left brick pillar: x = 0..LB-1
  const int RB = 55;          // right brick pillar: x = RB..63
  const int TB = 8;           // top lintel: y = 0..TB-1

  float heat[W * H];

  struct Ember { int x, y; float phase, rate; };
  Ember embers[12];
  int emberCount = 0;

  uint16_t fireColor(MatrixPanel_I2S_DMA* d, float h) {
    // Hot core white/yellow → orange → deep red at the tips.
    float r, g, b;
    if (h < 80)        { r = h * 3.2f; g = 0; b = 0; }
    else if (h < 180)  { r = 255; g = (h - 80) * 1.6f; b = 0; }
    else               { r = 255; g = 160 + (h - 180) * 1.1f; b = (h - 180) * 1.5f; }
    return d->color565(constrain((int)r, 0, 255),
                       constrain((int)g, 0, 255),
                       constrain((int)b, 0, 255));
  }

  void drawLogs(MatrixPanel_I2S_DMA* d) {
    uint16_t body = d->color565(66, 33, 12);
    uint16_t end  = d->color565(120, 66, 30);
    uint16_t ring = d->color565(90, 48, 20);

    d->fillRect(17, 30, 30, 2, body);          // front log
    d->fillCircle(17, 30, 1, end);
    d->fillCircle(46, 30, 1, end);
    d->drawPixel(46, 30, ring);

    d->fillRect(21, 28, 22, 2, body);          // rear log
    d->fillCircle(21, 28, 1, end);
    d->fillCircle(42, 28, 1, end);
    d->drawPixel(21, 29, ring);
  }

  uint16_t brickPixel(MatrixPanel_I2S_DMA* d, int x, int y) {
    const int bH = 4, bW = 8;
    int band   = y / bH;
    int offset = (band % 2) * (bW / 2);        // every other course shifts half a brick
    int inCol  = (x + offset) % bW;
    int inRow  = y % bH;
    if (inRow == bH - 1 || inCol == 0)         // mortar
      return d->color565(60, 50, 46);
    int col   = (x + offset) / bW;
    int shade = ((col * 29 + band * 53) % 7) - 3;   // stable -3..3 per brick
    return d->color565(constrain(150 + shade * 8, 90, 200),
                       constrain(58  + shade * 3, 30, 95),
                       constrain(42  + shade * 3, 20, 72));
  }

  void drawSurround(MatrixPanel_I2S_DMA* d) {
    for (int y = 0; y < H; y++)
      for (int x = 0; x < W; x++)
        if (y < TB || x < LB || x >= RB)
          d->drawPixel(x, y, brickPixel(d, x, y));
    // Mantel shelf.
    d->drawFastHLine(0, TB,     W, d->color565(120, 110, 100));
    d->drawFastHLine(0, TB - 1, W, d->color565(150, 140, 130));
  }
}

void fireSetup() {
  using namespace fire;
  for (int i = 0; i < W * H; i++) heat[i] = 0;

  const int spots[][2] = {
    {23, 29}, {26, 28}, {29, 29}, {31, 27}, {33, 28},
    {35, 27}, {37, 28}, {40, 29}, {28, 28}, {43, 28},
    {20, 29}, {45, 30},
  };
  emberCount = sizeof(spots) / sizeof(spots[0]);
  for (int i = 0; i < emberCount; i++) {
    embers[i].x = spots[i][0];
    embers[i].y = spots[i][1];
    embers[i].phase = random(0, 628) / 100.0f;
    embers[i].rate  = random(30, 90) / 10.0f;
  }
}

void fireLoop(MatrixPanel_I2S_DMA* d) {
  using namespace fire;
  float t = millis() / 1000.0f;
  d->fillScreen(0);

  // ── Seed the flame base (sway + flicker, parabolic bell profile) ──
  float sway    = sinf(t * 1.7f) * 1.4f + sinf(t * 3.1f) * 0.7f;
  float flicker = 0.78f + 0.22f * sinf(t * 11.0f) * sinf(t * 6.3f);
  for (int x = CX - FLAME_W; x <= CX + FLAME_W; x++) {
    float dd   = (x - CX - sway) / (float)FLAME_W;
    float bell = fmaxf(0.0f, 1 - dd * dd);
    float v    = bell * flicker * 255 + random(-18, 18);
    heat[BASE * W + x] = constrain(v, 0.0f, 255.0f);
  }

  // ── Rise, lean & cool ──
  for (int y = FLAME_TOP; y < BASE; y++) {
    int lean = (int)roundf(sway * (BASE - y) / (float)(BASE - FLAME_TOP));
    for (int x = CX - FLAME_W - 3; x <= CX + FLAME_W + 3; x++) {
      int bx       = constrain(x - lean, 0, W - 1);
      float below  = heat[(y + 1) * W + bx];
      float belowL = heat[(y + 1) * W + constrain(bx - 1, 0, W - 1)];
      float belowR = heat[(y + 1) * W + constrain(bx + 1, 0, W - 1)];
      float v = below * 0.58f + belowL * 0.22f + belowR * 0.22f;
      v -= 1.5f + abs(x - CX) * 1.3f + random(0, 5);
      heat[y * W + x] = constrain(v, 0.0f, 255.0f);
    }
  }

  // ── Draw the flame (skip cold cells so the background stays black) ──
  for (int y = FLAME_TOP; y <= BASE; y++)
    for (int x = CX - FLAME_W - 3; x <= CX + FLAME_W + 3; x++) {
      float h = heat[y * W + x];
      if (h > 14) d->drawPixel(x, y, fireColor(d, h));
    }

  // ── Logs, then embers glowing on top ──
  drawLogs(d);
  for (int i = 0; i < emberCount; i++) {
    Ember& e = embers[i];
    float glow = 0.45f + 0.55f * (0.5f + 0.5f * sinf(t * e.rate + e.phase));
    int r = (int)(120 + 135 * glow);
    int g = (int)(20 + 70 * glow);
    d->drawPixel(e.x, e.y, d->color565(r, g, 0));
  }

  // Brick surround + mantel, drawn last to frame the firebox.
  drawSurround(d);
}

// ═════════════════════════════════════════════════════════════════════════════
//  NETWORK TASK  (core 0) — polls the backend, writes requestedFaceId
// ═════════════════════════════════════════════════════════════════════════════

// Pull the string value of "key" out of a small JSON body without a JSON library.
// e.g. body {"face":"fire"}, key "face" → "fire". Returns "" if not found.
static String extractJsonString(const String& body, const char* key) {
  String needle = String("\"") + key + "\"";
  int k = body.indexOf(needle);
  if (k < 0) return "";
  int colon = body.indexOf(':', k + needle.length());
  if (colon < 0) return "";
  int q1 = body.indexOf('"', colon + 1);
  if (q1 < 0) return "";
  int q2 = body.indexOf('"', q1 + 1);
  if (q2 < 0) return "";
  return body.substring(q1 + 1, q2);
}

static void pollDisplayState() {
  WiFiClientSecure client;
  client.setInsecure();                 // skip cert verification (Railway TLS)

  HTTPClient http;
  String url = String("https://") + BACKEND_BASE_URL + "/display/state";
  if (!http.begin(client, url)) return;
  http.addHeader("X-API-Key", BACKEND_API_KEY);
  http.setTimeout(3000);                // short — recovery over render-priority

  int code = http.GET();
  if (code == 200) {
    String face = extractJsonString(http.getString(), "face");
    if (face.length()) {
      int idx = faceIndexForSlug(face.c_str());
      if (idx >= 0) requestedFaceId = idx;
      else Serial.printf("[net] unknown face slug from backend: %s\n", face.c_str());
    }
  } else {
    Serial.printf("[net] /display/state HTTP %d\n", code);
  }
  http.end();
}

// Parse "#rrggbb" (or "rrggbb") into r/g/b bytes.
static void hexToRGB(const char* hex, uint8_t& r, uint8_t& g, uint8_t& b) {
  if (hex && hex[0] == '#') hex++;
  long n = (hex && *hex) ? strtol(hex, nullptr, 16) : 0xC8C8C8;
  r = (n >> 16) & 0xFF; g = (n >> 8) & 0xFF; b = n & 0xFF;
}

static void pollWeather() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String("https://") + BACKEND_BASE_URL + "/weather";
  if (!http.begin(client, url)) return;
  http.addHeader("X-API-Key", BACKEND_API_KEY);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      const char* cond = doc["condition"] | "sunny";
      weatherMode = (strcmp(cond, "cloudy") == 0) ? 1
                  : (strcmp(cond, "rainy")  == 0) ? 2 : 0;
      if (!doc["temperature"].isNull()) {
        temperatureC = doc["temperature"].as<float>();
        haveTemp = true;
      }
    }
  } else {
    Serial.printf("[net] /weather HTTP %d\n", code);
  }
  http.end();
}

static void pollConnected() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String("https://") + BACKEND_BASE_URL + "/connected";
  if (!http.begin(client, url)) return;
  http.addHeader("X-API-Key", BACKEND_API_KEY);
  http.setTimeout(5000);
  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      String myMac = WiFi.macAddress();   // filter out the display itself
      RosterEntry tmp[MAX_PEOPLE];
      int n = 0;
      for (JsonObject dev : doc["connected_devices"].as<JsonArray>()) {
        if (n >= MAX_PEOPLE) break;
        const char* mac = dev["mac_address"] | "";
        if (myMac.equalsIgnoreCase(mac)) continue;   // don't count ourselves as a person
        const char* colr = dev["colour"] | "#c8c8c8";
        strncpy(tmp[n].id, mac, sizeof(tmp[n].id) - 1);
        tmp[n].id[sizeof(tmp[n].id) - 1] = '\0';
        hexToRGB(colr, tmp[n].r, tmp[n].g, tmp[n].b);
        n++;
      }
      xSemaphoreTake(rosterMutex, portMAX_DELAY);
      memcpy(rosterShared, tmp, sizeof(tmp));
      rosterCount = n;
      xSemaphoreGive(rosterMutex);
    }
  } else {
    Serial.printf("[net] /connected HTTP %d\n", code);
  }
  http.end();
}

static void networkTask(void* pv) {
  // Connect WiFi on this core so a slow join never blocks rendering.
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("[net] connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    vTaskDelay(pdMS_TO_TICKS(500));
    Serial.print(".");
  }
  Serial.printf("\n[net] connected, IP %s\n", WiFi.localIP().toString().c_str());

  // NTP → Sydney local time for the clock face.
  configTzTime(TZ_SYDNEY, "pool.ntp.org", "time.nist.gov");
  Serial.println("[net] NTP requested");

  // Three independent poll cadences (see DESIGN.md). Each tracks its own "last
  // polled" time; the task ticks every 500ms and fires whichever is due.
  const uint32_t STATE_MS     = 3000;      // face switch responsiveness
  const uint32_t WEATHER_MS   = 180000;    // 3 min (backend caches 10 min)
  const uint32_t CONNECTED_MS = 20000;     // 20 s (presence changes slowly)
  uint32_t lastState = 0, lastWeather = 0, lastConnected = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      if (now - lastState >= STATE_MS)              { pollDisplayState(); lastState = now; }
      if (lastWeather == 0 || now - lastWeather >= WEATHER_MS)   { pollWeather();   lastWeather = now; }
      if (lastConnected == 0 || now - lastConnected >= CONNECTED_MS) { pollConnected(); lastConnected = now; }
    } else {
      WiFi.reconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SETUP + RENDER LOOP  (core 1)
// ═════════════════════════════════════════════════════════════════════════════
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\nESP32-S3 multi-face display starting...");

  // ── HUB75 panel ──
  // Pins proven on hardware with the Seengreat model E adapter (Rev 2.2 / V2.x)
  // + this Waveshare panel. NOTE: green & blue are SWAPPED vs the official wiki
  // (this panel wires G/B opposite) and the panel needs the FM6124 driver.
  // Verified via panel_test.ino — see that sketch for the diagnostic history.
  HUB75_I2S_CFG::i2s_pins pins = {
    18, 17, 8,    // r1, g1, b1  (g1/b1 swapped from wiki 8/17)
    16, 15, 1,    // r2, g2, b2  (g2/b2 swapped from wiki 1/15)
    7, 48, 6, 47, 2,   // a, b, c, d, e
    21, 4, 5      // lat, oe, clk
  };
  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1 /* chain length */, pins);
  mxconfig.clkphase = false;
  mxconfig.driver = HUB75_I2S_CFG::FM6124;   // this panel's driver chips
  mxconfig.double_buff = true;               // draw to back buffer, flip atomically
                                             // → no tearing/flicker on static pixels
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(90);      // 0-255
  dma_display->clearScreen();

  // Guards the presence roster shared between the network and render cores.
  rosterMutex = xSemaphoreCreateMutex();

  // Networking on core 0 (large stack — TLS handshake is stack-hungry).
  xTaskCreatePinnedToCore(networkTask, "network", 12288, nullptr, 1, nullptr, 0);

  Serial.printf("[render] %d faces registered, running on core %d\n",
                FACE_COUNT, xPortGetCoreID());
}

void loop() {
  static int currentFaceId = -1;

  uint32_t frameStart = millis();

  // Apply a pending face switch (only this core touches the display).
  int want = requestedFaceId;
  if (want != currentFaceId && want >= 0 && want < FACE_COUNT) {
    currentFaceId = want;
    dma_display->fillScreen(0);
    if (faces[currentFaceId].setup) faces[currentFaceId].setup();
    Serial.printf("[render] switched to face '%s'\n", faces[currentFaceId].id);
  }

  faces[currentFaceId].loop(dma_display);
  dma_display->flipDMABuffer();   // present the completed frame (double buffering)

  // ~30 fps. delay() yields to other tasks and feeds the watchdog.
  uint32_t elapsed = millis() - frameStart;
  if (elapsed < 33) delay(33 - elapsed);
}
