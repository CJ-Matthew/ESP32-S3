# ESP32-S3 LED Matrix Display

Bedside RGB LED matrix display project.

## Hardware

| Component | Part |
|---|---|
| Microcontroller | ESP32-S3-DevKitC-1 (DFR0895) |
| Display | 64×32 RGB LED matrix, HUB75, 2.5mm pitch (Waveshare WS-23707) |
| Adapter | Seengreat RGB Matrix Adapter Board (model E) — connects ESP32-S3-DevKitC-1 to HUB75, dual DC jack power input |
| PSU | 5V 4A, 2.1mm barrel jack |
| Capacitors | 2× 330µF 35V on power rails |

## Software

- Arduino IDE 2
- Board: **ESP32-S3 DevModule**, flashed over UART via Micro-USB
- [ESP32 HUB75 LED MATRIX PANEL DMA Display](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) by MrCodetastic (v3.0.14)
- Adafruit GFX library

## Project structure

```
sketch_jun29b/
  sketch_jun29b.ino   # main sketch
  secrets.h           # WiFi credentials — gitignored, never committed
simulator/
  index.html          # open in browser to preview the panel without flashing
  matrix.js           # MatrixDisplay class, mirrors dma_display->* API
  sketch.js           # copy drawing calls here to preview; edit and reload
```

## Simulator

Open `simulator/index.html` locally in a browser. No server needed.

`display.*` mirrors `dma_display->*` — copy drawing calls from your `.ino` with only `s/dma_display->/display./`.

Arduino globals available in `sketch.js`: `millis()`, `micros()`, `random()`, `delay()` (no-op), `map()`, `constrain()`, `sin()`, `cos()`.

FPS target is configurable in the UI (default 30 fps). Gamma correction is applied so mid-range colours match real panel brightness.

## Key constraints

- No public IP — ESP32 sits behind home router NAT. Backend communication must be ESP32-initiated (polling or WebSocket out to server, or MQTT).
- WiFi credentials live in `secrets.h` which is gitignored.
- Flash: WiFi stack alone consumes ~65% of the 1.28MB flash. ~450KB remains for display library and application code.
