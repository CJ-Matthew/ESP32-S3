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

// Spotify "now playing" from /spotify/state + /spotify/art (network writes, render
// reads, guarded by spotifyMutex). progressAtMillis lets the render core interpolate
// elapsed time between polls so the progress bar sweeps smoothly.
static const int ALBUM_ART = 30;                 // cover is 30×30 px (matches backend)
struct SpotifyState {
  bool     active;                               // is a track loaded? (false → idle screen)
  bool     playing;                              // playing vs paused
  char     title[64];
  char     artist[64];
  char     trackId[24];
  uint32_t durationMs;
  uint32_t progressMs;
  uint32_t progressAtMillis;                     // millis() when progressMs was captured
  bool     hasArt;
};
SpotifyState      spotifyShared = {};
uint16_t          albumArt[ALBUM_ART * ALBUM_ART];   // RGB565 cover, guarded by spotifyMutex
bool              albumArtValid = false;
SemaphoreHandle_t spotifyMutex = nullptr;
int               spotifyFaceId = -1;            // index of the spotify face (set in setup)

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
void starfieldSetup();
void starfieldLoop(MatrixPanel_I2S_DMA* d);
void spotifySetup();
void spotifyLoop(MatrixPanel_I2S_DMA* d);

// Registry. Add a face = write two functions + one entry here + a backend slug.
Face faces[] = {
  { "clock",     clockSetup,     clockLoop     },
  { "fire",      fireSetup,      fireLoop      },
  { "starfield", starfieldSetup, starfieldLoop },
  { "spotify",   spotifySetup,   spotifyLoop   },
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
//  STARFIELD FACE  (ported from simulator/face_starfield.js — parallax stars,
//  the moon at tonight's real phase, an occasional shooting star, weather in the
//  sky, and a dim HH:MM in the corner.)
// ═════════════════════════════════════════════════════════════════════════════
namespace starfield {
  const int W = 64, H = 32;

  // Moon geometry — top-right, ~11px disc.
  const int  MOON_CX = 50, MOON_CY = 9, MOON_R = 5;
  // Southern hemisphere (Sydney): the waxing moon is lit on the LEFT — flip the
  // terminator so the drawn phase matches what's actually out the window.
  const bool SOUTHERN = true;

  // Synodic month (new moon → new moon) and a known reference new moon, as a Unix
  // timestamp: 2000-01-06 18:14:00 UTC. age = (now − epoch) mod SYNODIC.
  const double SYNODIC        = 29.530588853;   // days
  const time_t NEW_MOON_EPOCH = 947182440;      // seconds

  // Three parallax depth layers: nearer stars are brighter, drift faster, twinkle
  // harder. Gives the sky depth.
  const float LAYER_SPEED[3]   = { 0.12f, 0.30f, 0.60f };   // px / second
  const int   LAYER_BRIGHT[3]  = { 60,    110,   190   };   // peak brightness
  const float LAYER_TWINKLE[3] = { 0.15f, 0.30f, 0.50f };   // twinkle depth

  const int STAR_COUNT = 55;
  struct Star { float x, y; uint8_t layer; float base, phase, rate, tr, tg, tb; };
  Star stars[STAR_COUNT];

  struct Shoot { bool active; float x0, y0, vx, vy; uint32_t t0, dur; };
  Shoot    shoot     = { false, 0, 0, 0, 0, 0, 0 };
  uint32_t nextShoot = 0;

  // Multiply an RGB by a factor (twinkle / weather-dim / fade) → a clamped 565.
  inline uint16_t scol(MatrixPanel_I2S_DMA* d, float r, float g, float b, float f) {
    return d->color565(constrain((int)(r * f), 0, 255),
                       constrain((int)(g * f), 0, 255),
                       constrain((int)(b * f), 0, 255));
  }

  // Illumination phase 0..1 for a time: 0 = new, 0.5 = full, → 1 back to new.
  double moonPhase(time_t nowSec) {
    double days = (double)(nowSec - NEW_MOON_EPOCH) / 86400.0;
    double p = fmod(days, SYNODIC) / SYNODIC;
    return p < 0 ? p + 1 : p;
  }

  // Moon at `phase`, dimmed by `dim`. Lit side pale white; dark limb a faint
  // blue-grey "earthshine" so the whole disc stays visible. A few crater pixels.
  void drawMoon(MatrixPanel_I2S_DMA* d, double phase, float dim) {
    float a = cosf(2.0f * PI * (float)phase);   // terminator bulge: +1 new … −1 full
    bool  waxing = phase < 0.5;
    const int craters[][2] = { {-2, -1}, {1, 1}, {0, 2}, {2, -2} };

    for (int dy = -MOON_R; dy <= MOON_R; dy++) {
      float hw = sqrtf((float)(MOON_R * MOON_R - dy * dy));   // disc half-width at row
      for (int dx = -MOON_R; dx <= MOON_R; dx++) {
        if (dx * dx + dy * dy > MOON_R * MOON_R) continue;    // outside the disc
        int   px  = SOUTHERN ? -dx : dx;                      // flip lit side for Sydney
        float xt  = a * hw;                                   // terminator x at this row
        bool  lit = waxing ? (px >= xt) : (px <= -xt);
        int X = MOON_CX + dx, Y = MOON_CY + dy;
        if (lit) {
          float f = 1.0f;
          for (auto& c : craters) if (c[0] == dx && c[1] == dy) f = 0.72f;
          d->drawPixel(X, Y, scol(d, 235, 238, 248, f * dim));
        } else {
          d->drawPixel(X, Y, scol(d, 30, 35, 55, dim));       // earthshine
        }
      }
    }
  }

  // A soft grey cloud blob centred at (cx, cy) — reused for the cloudy sky.
  void cloudBlob(MatrixPanel_I2S_DMA* d, int cx, int cy) {
    uint16_t g  = d->color565(118, 122, 134);
    uint16_t hi = d->color565(150, 154, 165);
    d->fillRect(cx - 1, cy, 9, 3, g);
    d->fillCircle(cx,     cy,     2, g);
    d->fillCircle(cx + 3, cy,     2, g);
    d->fillCircle(cx + 6, cy + 1, 2, g);
    d->fillCircle(cx + 2, cy - 1, 2, hi);   // lit crown
  }
}

void starfieldSetup() {
  using namespace starfield;
  for (int i = 0; i < STAR_COUNT; i++) {
    int layer = random(0, 3);
    // Colour: mostly white, some cool blue, a few warm — a real sky isn't grey.
    int r = random(0, 100);
    float tr, tg, tb;
    if      (r < 70) { tr = 1.00f; tg = 1.00f; tb = 1.00f; }
    else if (r < 88) { tr = 0.72f; tg = 0.82f; tb = 1.00f; }
    else             { tr = 1.00f; tg = 0.90f; tb = 0.78f; }
    stars[i].x     = random(0, W);
    stars[i].y     = random(0, H);
    stars[i].layer = layer;
    stars[i].base  = LAYER_BRIGHT[layer] * (0.6f + random(0, 40) / 100.0f);
    stars[i].phase = random(0, 628) / 100.0f;
    stars[i].rate  = (80 + random(0, 240)) / 100.0f;    // twinkle speed
    stars[i].tr = tr; stars[i].tg = tg; stars[i].tb = tb;
  }
  shoot.active = false;
  nextShoot    = millis() + 3000 + random(0, 6000);     // first one appears soon
}

void starfieldLoop(MatrixPanel_I2S_DMA* d) {
  using namespace starfield;
  float    t   = millis() / 1000.0f;
  uint32_t now = millis();
  d->fillScreen(0);

  // Weather reaches the sky: rain darkens it most, cloud a little.
  float dim = (weatherMode == 2) ? 0.45f : (weatherMode == 1) ? 0.80f : 1.0f;

  // ── Stars (parallax drift + per-star twinkle) ──
  for (int i = 0; i < STAR_COUNT; i++) {
    Star& s = stars[i];
    float xf = fmodf(fmodf(s.x - LAYER_SPEED[s.layer] * t, (float)W) + W, (float)W);
    int   x  = (int)roundf(xf);
    int   y  = (int)s.y;
    float depth = LAYER_TWINKLE[s.layer];
    float tw = 1 - depth + depth * (0.5f + 0.5f * sinf(t * s.rate + s.phase));
    float f  = s.base * tw * dim;
    d->drawPixel(x, y, scol(d, 255 * s.tr, 255 * s.tg, 255 * s.tb, f / 255.0f));
  }

  // ── Moon at tonight's real phase (drawn over the stars) ──
  drawMoon(d, moonPhase(time(nullptr)), dim);

  // ── Shooting star ──
  if (!shoot.active && now >= nextShoot) {
    int dir = (random(0, 2) == 0) ? 1 : -1;                 // down-right or down-left
    int x0  = (dir == 1) ? random(2, 30) : random(34, 62);
    shoot.active = true; shoot.x0 = x0; shoot.y0 = random(2, 10);
    shoot.vx = dir * 20; shoot.vy = 9; shoot.t0 = now; shoot.dur = 500 + random(0, 300);
    nextShoot = now + shoot.dur + 12000 + random(0, 22000);  // long quiet gap after
  }
  if (shoot.active) {
    float p = (float)(now - shoot.t0) / shoot.dur;
    if (p >= 1) {
      shoot.active = false;
    } else {
      for (int k = 0; k < 4; k++) {                          // head + fading tail
        float q = p - k * 0.06f;
        if (q < 0) continue;
        int tx = (int)roundf(shoot.x0 + shoot.vx * q);
        int ty = (int)roundf(shoot.y0 + shoot.vy * q);
        d->drawPixel(tx, ty, scol(d, 255, 255, 255, (1 - p) * (1 - k * 0.28f)));
      }
    }
  }

  // ── Cloudy: slow drifting blobs that pass over the sky ──
  if (weatherMode == 1) {
    float drift = t * 4;                                     // px / second
    cloudBlob(d, (int)fmodf(drift,      (float)(W + 24)) - 12, 6);
    cloudBlob(d, (int)fmodf(drift + 34, (float)(W + 24)) - 12, 13);
  }

  // ── Rainy: a light diagonal drizzle over a darkened sky ──
  if (weatherMode == 2) {
    for (int i = 0; i < 12; i++) {
      float speed = 0.9f + (i % 3) * 0.2f;
      float phase = fmodf(t * speed + i * 0.37f, 1.0f);
      int   x = (i * 7 + 3) % W;
      int   y = (int)(phase * H);
      d->drawPixel(x,     y,     d->color565(70, 130, 210));
      d->drawPixel(x + 1, y + 1, d->color565(45,  90, 150));
    }
  }

  // ── Dim HH:MM in the corner so it's still a bedside clock at night ──
  time_t nowSec = time(nullptr);
  struct tm tn;
  localtime_r(&nowSec, &tn);
  if (tn.tm_year + 1900 >= 2020) {
    char buf[6];
    snprintf(buf, sizeof(buf), "%02d:%02d", tn.tm_hour, tn.tm_min);
    d->setTextSize(1);
    d->setTextWrap(false);
    d->setTextColor(scol(d, 120, 140, 180, dim));
    d->setCursor(2, 25);
    d->print(buf);
  }
}

// ═════════════════════════════════════════════════════════════════════════════
//  SPOTIFY FACE  (ported from simulator/face_spotify.js — 30×30 album cover +
//  compact 3×5 text: title/artist marquees, elapsed timer, progress bar. Album
//  art is RGB565 pushed from the backend; this face only blits it.)
// ═════════════════════════════════════════════════════════════════════════════
namespace spotifyface {
  const int   RCOL = 34, RW = 30;                // right column: x start, width (RCOL, not RX — RX is a pin macro)
  const float SCROLL_SPEED = 14.0f;              // marquee px/sec
  const int   SCROLL_GAP   = 8;                  // px between marquee repeats

  inline uint16_t dimc(MatrixPanel_I2S_DMA* d, int r, int g, int b, float f) {
    return d->color565((int)(r * f), (int)(g * f), (int)(b * f));
  }

  // ── Compact 3×5 font (verbatim from the JS face; uppercase only) ──
  struct TinyGlyph { char c; const char* rows[5]; };
  const TinyGlyph TINY[] = {
    {' ', {"...","...","...","...","..."}},
    {'0', {"###","#.#","#.#","#.#","###"}}, {'1', {".#.","##.",".#.",".#.","###"}},
    {'2', {"##.","..#",".#.","#..","###"}}, {'3', {"##.","..#",".#.","..#","##."}},
    {'4', {"#.#","#.#","###","..#","..#"}}, {'5', {"###","#..","##.","..#","##."}},
    {'6', {".##","#..","###","#.#","###"}}, {'7', {"###","..#",".#.",".#.",".#."}},
    {'8', {"###","#.#","###","#.#","###"}}, {'9', {"###","#.#","###","..#","##."}},
    {'A', {".#.","#.#","###","#.#","#.#"}}, {'B', {"##.","#.#","##.","#.#","##."}},
    {'C', {".##","#..","#..","#..",".##"}}, {'D', {"##.","#.#","#.#","#.#","##."}},
    {'E', {"###","#..","##.","#..","###"}}, {'F', {"###","#..","##.","#..","#.."}},
    {'G', {".##","#..","#.#","#.#",".##"}}, {'H', {"#.#","#.#","###","#.#","#.#"}},
    {'I', {"###",".#.",".#.",".#.","###"}}, {'J', {"..#","..#","..#","#.#",".#."}},
    {'K', {"#.#","#.#","##.","#.#","#.#"}}, {'L', {"#..","#..","#..","#..","###"}},
    {'M', {"#.#","###","###","#.#","#.#"}}, {'N', {"#.#","##.","###",".##","#.#"}},
    {'O', {".#.","#.#","#.#","#.#",".#."}}, {'P', {"##.","#.#","##.","#..","#.."}},
    {'Q', {".#.","#.#","#.#","###",".##"}}, {'R', {"##.","#.#","##.","#.#","#.#"}},
    {'S', {".##","#..",".#.","..#","##."}}, {'T', {"###",".#.",".#.",".#.",".#."}},
    {'U', {"#.#","#.#","#.#","#.#","###"}}, {'V', {"#.#","#.#","#.#","#.#",".#."}},
    {'W', {"#.#","#.#","###","###","#.#"}}, {'X', {"#.#","#.#",".#.","#.#","#.#"}},
    {'Y', {"#.#","#.#",".#.",".#.",".#."}}, {'Z', {"###","..#",".#.","#..","###"}},
    {'.', {"...","...","...","...",".#."}}, {',', {"...","...","...",".#.","#.."}},
    {'\'',{".#.",".#.","...","...","..."}}, {'-', {"...","...","###","...","..."}},
    {':', {"...",".#.","...",".#.","..."}}, {'!', {".#.",".#.",".#.","...",".#."}},
    {'?', {"##.","..#",".#.","...",".#."}}, {'&', {".#.","#.#",".#.","#.#",".##"}},
    {'/', {"..#","..#",".#.","#..","#.."}}, {'(', {".#.","#..","#..","#..",".#."}},
    {')', {".#.","..#","..#","..#",".#."}}, {'+', {"...",".#.","###",".#.","..."}},
  };
  const int TINY_N = sizeof(TINY) / sizeof(TINY[0]);

  const char* const* glyphFor(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;           // lowercase → small-caps
    for (int i = 0; i < TINY_N; i++) if (TINY[i].c == c) return TINY[i].rows;
    return TINY[0].rows;                         // unknown → blank
  }

  // Draw 3×5 text, clipped to [clipL, clipR] so scrolling text can't bleed left.
  void drawTiny(MatrixPanel_I2S_DMA* d, const char* s, int x, int y, uint16_t color,
                int clipL, int clipR) {
    int cx = x;
    for (const char* p = s; *p; ++p) {
      const char* const* g = glyphFor(*p);
      for (int row = 0; row < 5; row++)
        for (int col = 0; col < 3; col++)
          if (g[row][col] == '#') {
            int px = cx + col;
            if (px >= clipL && px <= clipR) d->drawPixel(px, y + row, color);
          }
      cx += 4;                                   // 3 px glyph + 1 px gap
    }
  }
  int textW(const char* s) { int n = 0; for (const char* p = s; *p; ++p) n++; return n * 4; }

  void marquee(MatrixPanel_I2S_DMA* d, const char* s, int y, uint16_t color) {
    int w = textW(s);
    if (w <= RW) { drawTiny(d, s, RCOL, y, color, RCOL, 63); return; }   // fits → static
    int period = w + SCROLL_GAP;
    int off = (int)fmodf(millis() * SCROLL_SPEED / 1000.0f, (float)period);
    drawTiny(d, s, RCOL - off,          y, color, RCOL, 63);             // two copies → wrap
    drawTiny(d, s, RCOL - off + period, y, color, RCOL, 63);
  }

  void msFmt(uint32_t ms, char* buf) {           // "m:ss"
    uint32_t s = ms / 1000;
    snprintf(buf, 8, "%u:%02u", (unsigned)(s / 60), (unsigned)(s % 60));
  }

  void drawEq(MatrixPanel_I2S_DMA* d, int x, int baseY) {   // 3-bar equaliser (playing)
    uint16_t c = d->color565(29, 185, 84);
    for (int i = 0; i < 3; i++) {
      int h = 1 + (int)roundf((sinf(millis() / (140.0f + i * 55) + i * 1.7f) * 0.5f + 0.5f) * 5);
      d->drawFastVLine(x + i * 2, baseY - h + 1, h, c);
    }
  }

  void drawPause(MatrixPanel_I2S_DMA* d, int x, int y) {    // pause glyph (paused)
    uint16_t c = d->color565(120, 150, 130);
    d->fillRect(x, y, 1, 5, c);
    d->fillRect(x + 2, y, 1, 5, c);
  }

  void drawIdle(MatrixPanel_I2S_DMA* d) {        // nothing playing → note + label
    uint16_t g = d->color565(25, 157, 71);
    d->fillCircle(29, 15, 2, g);
    d->drawFastVLine(31, 7, 9, g);
    d->drawFastHLine(31, 7, 4, g);
    d->drawPixel(34, 8, g);
    drawTiny(d, "NOT PLAYING", 10, 22, d->color565(120, 150, 130), 0, 63);
  }
}

void spotifySetup() {
  // Nothing to seed — state arrives from the network task.
}

void spotifyLoop(MatrixPanel_I2S_DMA* d) {
  using namespace spotifyface;
  d->fillScreen(0);

  // Snapshot shared state (+ art) under the mutex, then render lock-free.
  static uint16_t artCopy[ALBUM_ART * ALBUM_ART];
  SpotifyState s;
  bool haveArt;
  xSemaphoreTake(spotifyMutex, portMAX_DELAY);
  s = spotifyShared;
  haveArt = albumArtValid;
  if (haveArt) memcpy(artCopy, albumArt, sizeof(artCopy));
  xSemaphoreGive(spotifyMutex);

  if (!s.active) { drawIdle(d); return; }        // Spotify closed / idle

  // Interpolate elapsed time from the last poll (only while actually playing).
  uint32_t prog = s.progressMs;
  if (s.playing) prog += millis() - s.progressAtMillis;
  if (prog > s.durationMs) prog = s.durationMs;
  float frac = s.durationMs ? (float)prog / s.durationMs : 0.0f;
  float dimF = s.playing ? 1.0f : 0.5f;          // paused → dim

  // ── Album cover (inset 1 px off the edges) ──
  if (haveArt) {
    for (int yy = 0; yy < ALBUM_ART; yy++)
      for (int xx = 0; xx < ALBUM_ART; xx++)
        d->drawPixel(1 + xx, 1 + yy, artCopy[yy * ALBUM_ART + xx]);
  } else {
    d->fillRect(1, 1, ALBUM_ART, ALBUM_ART, d->color565(20, 25, 22));   // placeholder
  }

  // ── Title / artist marquees ──
  marquee(d, s.title,  2, dimc(d, 235, 235, 235, dimF));
  marquee(d, s.artist, 8, dimc(d, 120, 150, 130, dimF));

  // ── Elapsed timer (just above the bar) + equaliser / pause glyph ──
  char t[8]; msFmt(prog, t);
  drawTiny(d, t, RCOL, 22, s.playing ? d->color565(29, 185, 84) : d->color565(120, 150, 130), RCOL, 63);
  if (s.playing) drawEq(d, 57, 26);
  else           drawPause(d, 58, 22);

  // ── Progress bar (1 px) + playhead ──
  d->drawFastHLine(RCOL, 29, RW, d->color565(45, 55, 50));
  int fillW = (int)roundf(RW * frac);
  if (fillW > 0) d->drawFastHLine(RCOL, 29, fillW, dimc(d, 29, 185, 84, dimF));
  d->drawFastVLine(RCOL + (fillW < RW ? fillW : RW - 1), 28, 2, d->color565(120, 255, 160));
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

// Which track's cover we currently hold (network-task-local). Empty = none yet.
static char spotifyArtTrack[24] = "";

// Fetch the current cover as raw RGB565 bytes into albumArt[]. Returns true on success.
static bool pollSpotifyArt() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String("https://") + BACKEND_BASE_URL + "/spotify/art";
  if (!http.begin(client, url)) return false;
  http.addHeader("X-API-Key", BACKEND_API_KEY);
  http.setTimeout(6000);

  const int EXPECT = ALBUM_ART * ALBUM_ART * 2;   // 1800 bytes
  bool ok = false;
  int code = http.GET();
  if (code == 200 && http.getSize() == EXPECT) {
    static uint8_t raw[ALBUM_ART * ALBUM_ART * 2];   // static: keep it off the TLS stack
    WiFiClient* stream = http.getStreamPtr();
    int got = 0;
    uint32_t start = millis();
    while (got < EXPECT && millis() - start < 6000) {
      int avail = stream->available();
      if (avail > 0) {
        got += stream->readBytes(raw + got, min(avail, EXPECT - got));
      } else if (!http.connected()) {
        break;
      } else {
        delay(2);
      }
    }
    if (got == EXPECT) {
      xSemaphoreTake(spotifyMutex, portMAX_DELAY);
      for (int i = 0; i < ALBUM_ART * ALBUM_ART; i++)
        albumArt[i] = ((uint16_t)raw[2 * i] << 8) | raw[2 * i + 1];   // big-endian
      albumArtValid = true;
      xSemaphoreGive(spotifyMutex);
      ok = true;
    }
  } else if (code != 204) {
    Serial.printf("[net] /spotify/art HTTP %d (size %d)\n", code, http.getSize());
  }
  http.end();
  return ok;
}

static void pollSpotifyState() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  String url = String("https://") + BACKEND_BASE_URL + "/spotify/state";
  if (!http.begin(client, url)) return;
  http.addHeader("X-API-Key", BACKEND_API_KEY);
  http.setTimeout(6000);

  int code = http.GET();
  if (code == 200) {
    JsonDocument doc;
    if (deserializeJson(doc, http.getString()) == DeserializationError::Ok) {
      bool        active  = doc["active"]  | false;
      const char* trackId = doc["track_id"] | "";
      bool        hasArt  = doc["has_art"] | false;

      xSemaphoreTake(spotifyMutex, portMAX_DELAY);
      spotifyShared.active  = active;
      spotifyShared.playing = doc["playing"] | false;
      strncpy(spotifyShared.title,  doc["title"]  | "", sizeof(spotifyShared.title)  - 1);
      strncpy(spotifyShared.artist, doc["artist"] | "", sizeof(spotifyShared.artist) - 1);
      strncpy(spotifyShared.trackId, trackId,           sizeof(spotifyShared.trackId) - 1);
      spotifyShared.title[sizeof(spotifyShared.title) - 1]     = '\0';
      spotifyShared.artist[sizeof(spotifyShared.artist) - 1]   = '\0';
      spotifyShared.trackId[sizeof(spotifyShared.trackId) - 1] = '\0';
      spotifyShared.durationMs       = doc["duration_ms"] | 0;
      spotifyShared.progressMs       = doc["progress_ms"] | 0;
      spotifyShared.progressAtMillis = millis();
      spotifyShared.hasArt           = hasArt;
      xSemaphoreGive(spotifyMutex);

      // Fetch the cover only when the track changes (not every poll).
      if (!active) {
        spotifyArtTrack[0] = '\0';                     // reset → next track refetches
      } else if (strcmp(trackId, spotifyArtTrack) != 0) {
        if (hasArt && pollSpotifyArt()) {
          strncpy(spotifyArtTrack, trackId, sizeof(spotifyArtTrack) - 1);
          spotifyArtTrack[sizeof(spotifyArtTrack) - 1] = '\0';
        } else if (!hasArt) {
          xSemaphoreTake(spotifyMutex, portMAX_DELAY);
          albumArtValid = false;                        // no cover for this track
          xSemaphoreGive(spotifyMutex);
          strncpy(spotifyArtTrack, trackId, sizeof(spotifyArtTrack) - 1);
          spotifyArtTrack[sizeof(spotifyArtTrack) - 1] = '\0';
        }
      }
    }
  } else {
    Serial.printf("[net] /spotify/state HTTP %d\n", code);
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
  const uint32_t WEATHER_MS   = 180000;    // 3 min (backend caches 3 min)
  const uint32_t CONNECTED_MS = 20000;     // 20 s (presence changes slowly)
  const uint32_t SPOTIFY_MS   = 5000;      // 5 s — only while the spotify face is up
  uint32_t lastState = 0, lastWeather = 0, lastConnected = 0, lastSpotify = 0;

  for (;;) {
    if (WiFi.status() == WL_CONNECTED) {
      uint32_t now = millis();
      if (now - lastState >= STATE_MS)              { pollDisplayState(); lastState = now; }
      if (lastWeather == 0 || now - lastWeather >= WEATHER_MS)   { pollWeather();   lastWeather = now; }
      if (lastConnected == 0 || now - lastConnected >= CONNECTED_MS) { pollConnected(); lastConnected = now; }

      // Poll Spotify only when its face is showing (saves API calls the rest of the
      // day). Reset the timer when it isn't, so it polls immediately on activation.
      if (requestedFaceId == spotifyFaceId) {
        if (lastSpotify == 0 || now - lastSpotify >= SPOTIFY_MS) { pollSpotifyState(); lastSpotify = now; }
      } else {
        lastSpotify = 0;
      }
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

  // Guards the Spotify state + album-art buffer shared across cores.
  spotifyMutex = xSemaphoreCreateMutex();
  spotifyFaceId = faceIndexForSlug("spotify");

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
