// ── Panel first-light test (Step 3) ──────────────────────────────────────────
// Standalone sketch to prove the Seengreat model E adapter pins are correct,
// BEFORE running the real firmware. No WiFi, no faces — just:
//   1. cycle full-screen RED → GREEN → BLUE (checks each colour channel + order)
//   2. draw a white 1px border + an X + "OK" text (checks geometry + addressing)
//
// Pin maps are from the Seengreat model E wiki. The board ships in two revisions
// with DIFFERENT pinouts — set BOARD_REV below to match YOUR board's silkscreen.

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>

// ── SET THIS to match your board's printed version (1 for V1.x, 2 for V2.x) ──
#define BOARD_REV 2

#if BOARD_REV == 1
  // Seengreat model E — V1.x
  #define PIN_R1 37
  #define PIN_G1 6
  #define PIN_B1 36
  #define PIN_R2 35
  #define PIN_G2 5
  #define PIN_B2 0
  #define PIN_A  45
  #define PIN_B  1
  #define PIN_C  48
  #define PIN_D  2
  #define PIN_E  4
  #define PIN_LAT 38
  #define PIN_OE  21
  #define PIN_CLK 47
#elif BOARD_REV == 2
  // Seengreat model E — V2.x.
  // NOTE: this panel wires GREEN and BLUE opposite to the official pinout, so
  // G and B are intentionally SWAPPED here vs the wiki (wiki: G1=8,B1=17 / G2=1,B2=15).
  // Verified empirically: with the wiki values, a GREEN command showed blue.
  #define PIN_R1 18
  #define PIN_G1 17   // wiki B1 — swapped
  #define PIN_B1 8    // wiki G1 — swapped
  #define PIN_R2 16
  #define PIN_G2 15   // wiki B2 — swapped
  #define PIN_B2 1    // wiki G2 — swapped
  #define PIN_A  7
  #define PIN_B  48
  #define PIN_C  6
  #define PIN_D  47
  #define PIN_E  2
  #define PIN_LAT 21
  #define PIN_OE  4
  #define PIN_CLK 5
#else
  #error "Set BOARD_REV to 1 or 2 to match your Seengreat model E board revision"
#endif

MatrixPanel_I2S_DMA* dma_display = nullptr;

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.printf("\nPanel first-light test — BOARD_REV %d\n", BOARD_REV);

  // i2s_pins field order: r1,g1,b1,r2,g2,b2,a,b,c,d,e,lat,oe,clk
  HUB75_I2S_CFG::i2s_pins pins = {
    PIN_R1, PIN_G1, PIN_B1, PIN_R2, PIN_G2, PIN_B2,
    PIN_A, PIN_B, PIN_C, PIN_D, PIN_E,
    PIN_LAT, PIN_OE, PIN_CLK
  };

  HUB75_I2S_CFG mxconfig(64, 32, 1, pins);   // 64×32, single panel
  mxconfig.clkphase = false;                 // flip to true if pixels look sheared
  // This Waveshare panel uses FM6124 driver chips — REQUIRED, or it shows
  // random coloured garbage. If FM6124 still looks wrong, try FM6126A.
  mxconfig.driver = HUB75_I2S_CFG::FM6124;

  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  if (!dma_display->begin()) {
    Serial.println("ERROR: dma_display->begin() failed (memory/config)");
    return;
  }
  dma_display->setBrightness8(80);
  dma_display->clearScreen();
}

// Slow, unambiguous colour-channel test. Each pure colour fills the whole
// screen for 3s while Serial prints what it SHOULD be — so you can read off
// exactly how "commanded colour" maps to "colour you actually see".
void loop() {
  if (!dma_display) return;

  struct ColourTest { uint8_t r, g, b; const char* name; };
  static const ColourTest tests[] = {
    { 255,   0,   0, "RED   (255,0,0)"   },
    {   0, 255,   0, "GREEN (0,255,0)"   },
    {   0,   0, 255, "BLUE  (0,0,255)"   },
    { 255, 255, 255, "WHITE (255,255,255)" },
  };

  for (const auto& t : tests) {
    Serial.printf("Showing %s\n", t.name);
    dma_display->fillScreenRGB888(t.r, t.g, t.b);
    delay(3000);
  }
}
