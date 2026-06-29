// ── Your sketch ──────────────────────────────────────────────────────────────
// Mirror your Arduino drawing calls here.
//   • display replaces dma_display
//   • display.color565(r, g, b) works the same way
//   • millis() is available and works like Arduino's millis()
//   • delay() is a no-op — use millis() for timing instead:
//       if (millis() - lastUpdate > 1000) { ... lastUpdate = millis(); }
//
// setup(display) runs once on load.
// loop(display)  runs every animation frame (~60 fps).

let hue = 0;

function setup(display) {
  display.fillScreen(0);
}

function loop(display) {
  display.fillScreen(0);

  // Scrolling rainbow border
  const r = Math.round(Math.sin((hue) * Math.PI / 180) * 127 + 128);
  const g = Math.round(Math.sin((hue + 120) * Math.PI / 180) * 127 + 128);
  const b = Math.round(Math.sin((hue + 240) * Math.PI / 180) * 127 + 128);
  display.drawRect(0, 0, 64, 32, display.color565(r, g, b));

  // Text
  display.setCursor(4, 5);
  display.setTextColor(display.color565(255, 255, 255));
  display.setTextSize(1);
  display.print("ESP32-S3");

  display.setCursor(4, 18);
  display.setTextColor(display.color565(100, 220, 255));
  display.print("64 x 32");

  hue = (hue + 2) % 360;
}
