# Multi-Face Display — Backend-Driven Face Switching

Design for integrating the simulator-tested faces (clock, fireplace) into real
ESP32-S3 firmware, with the face selectable in near-real-time by a backend.

Status: **design agreed, implementation in progress.** First flash is *plumbing*
(networking + two-core render + face switching) with a bare clock and the full
fire sim; the fancy clock (weather palette, date, presence critters) is a
deliberate second pass.

---

## 1. Overview

```
  ┌────────────────────────┐        HTTPS (Railway, port 443)      ┌───────────────────────┐
  │  Backend (Railway)      │  ◀───────────────────────────────    │   ESP32-S3            │
  │  http.server (stdlib)   │                                       │                       │
  │  web-production-316b     │   GET  /display/state  (every ~3s) ─▶│  core 0: network task │
  │    .up.railway.app      │   GET  /weather        (every 2–5m)   │  core 1: render loop  │
  │  _display_state = {      │   GET  /connected      (every 15–30s) │        └─ dma_display  │
  │     "face": "clock"     │                                       │           (HUB75)     │
  │  }                      │                                       └───────────────────────┘
  │  scheduler thread ──────┼── flips face at 23:30 / 07:00
  │    (Australia/Sydney)   │      (Sydney local time)
  └────────────────────────┘
```

> Railway terminates TLS on port 443 with a managed cert and provides ingress —
> there is no raw public IP or open port to manage. The ESP32 connects over
> **HTTPS** (`WiFiClientSecure`, `setInsecure()`), not plain HTTP.

The **backend is the single source of truth** for which face is showing. The
ESP32 is a dumb renderer: it polls, and it obeys.

---

## 2. Transport: HTTP short-poll (not WebSocket)

The backend is Python's stdlib `http.server.ThreadingHTTPServer`, which **cannot
speak WebSocket** without bolting on a separate asyncio server on another port.
A bedside display tolerates 1–2 s of switch latency, so we short-poll instead.

The ESP32 runs **three independent poll cadences** (not one loop):

| Endpoint          | Cadence    | Why                                                        |
|-------------------|------------|------------------------------------------------------------|
| `/display/state`  | ~3 s       | Switch responsiveness. Over HTTPS a TLS handshake per poll is heavy, so 3 s rather than 1–2 s — still instant for a bedside display |
| `/weather`        | 2–5 min    | Backend already caches 600 s; faster polling is wasted     |
| `/connected`      | 15–30 s    | Presence changes slowly                                    |

Migrating to WebSocket later touches **only the transport** — the face logic,
state model, and scheduler are unchanged.

> **Note on CORS:** CORS is a *browser* mechanism. The ESP32 is not a browser —
> it never sends an `Origin` header and never enforces same-origin policy. The
> ESP32 ↔ backend link is unaffected by CORS regardless of transport. Only the
> browser-based simulator consumes CORS. Auth is the `X-API-Key` header, not CORS.

---

## 3. Backend

### 3.1 State — in-memory global

A new module `display_state.py` holds the current face in process memory:

```python
_state = {"face": "clock"}      # guarded by a threading.Lock
```

- `http.server` is single-process, so a module global is shared across all
  request threads. Read-modify-write is guarded by a `Lock`.
- **Losing it on restart is desired**: the display defaults to `clock`, and a
  backend reboot resetting to `clock` is correct behaviour. No durability needed.
- Promote to Supabase only if "last face survives a reboot" or multi-client reads
  are ever wanted. Same two endpoints, different backing store.

### 3.2 Endpoints

| Method | Route             | Body / Response                    |
|--------|-------------------|------------------------------------|
| GET    | `/display/state`  | → `{"face": "clock"}`              |
| POST   | `/display/state`  | `{"face": "fire"}` → `{"face": "fire"}` |

- Face IDs are **string slugs** (`"clock"`, `"fire"`) — human-readable in `curl`,
  stable across renumbering. Unknown slugs are rejected (400).
- The firmware maps slug → array index on receipt.

### 3.3 Scheduler — backend owns the schedule

A daemon thread (mirrors `presence_logger.start()`) flips the face at boundaries:

| Boundary (Sydney local) | Face        |
|-------------------------|-------------|
| 23:30                   | `starfield` |
| 07:00                   | `clock`     |

- **Anchored explicitly to `Australia/Sydney`** via `zoneinfo.ZoneInfo` — NOT the
  host's local clock. The server is in US-West California; we do **not** add a
  California offset (that would double-count). Host location is irrelevant.
- **Manual overrides stick until the next boundary.** A `POST /display/state`
  wins; the schedule only speaks again at the next boundary. Implementation: the
  scheduler sets the face once, on the *transition* into a boundary minute, not
  continuously — so a manual change mid-window is not stomped.

---

## 4. Firmware (ESP32-S3)

### 4.1 Two FreeRTOS cores — no render stutter

Blocking HTTP GETs (Arduino `HTTPClient`) must never touch the render path, or
the fire/clock animations hitch on every poll.

- **Core 0 — network task** (`xTaskCreatePinnedToCore`): does all blocking HTTP
  polls, parses responses, writes shared globals. Never draws.
- **Core 1 — render loop** (`loop()`): reads shared globals, draws at ~30 fps.

**Rule: the HUB75 display is not thread-safe — only the render core (core 1) may
ever touch `dma_display`.** The network core only writes plain data.

Shared state is a handful of small values (`requestedFaceId`, `weatherMode`,
`temperature`, roster). `requestedFaceId` is a single `int` written by core 0 and
read by core 1 — swap is a one-word handoff.

### 4.2 Face switching

```
network core:   sees new slug in /display/state  →  requestedFaceId = idx
render core:    top of each frame:
                  if (requestedFaceId != currentFaceId):
                      faces[requestedFaceId].setup()   // once
                      currentFaceId = requestedFaceId
                  faces[currentFaceId].loop(dma_display)
```

Switch is a **hard cut** (instant swap, `fillScreen(0)` + new face). No crossfade
— transitions add real complexity for a display that switches ~twice a day.

### 4.3 Modular face interface

```cpp
struct Face {
  const char* id;                                   // "clock", "fire"
  void (*setup)();                                  // once, on activation
  void (*loop)(MatrixPanel_I2S_DMA* d);             // every frame
};
Face faces[] = { clockFace, fireFace, /* add here */ };
```

**Adding a face = write two functions + one array entry + a backend slug.**

### 4.4 Time

- No RTC — time comes from **NTP** at boot (`configTzTime`).
- Timezone: **`Australia/Sydney`**, POSIX `AEST-10AEDT,M10.1.0,M4.1.0/3`
  (handles DST). Display-only; independent of the backend.
- Until NTP syncs, the clock shows `--:--:--`.

### 4.5 Graceful degradation

The render loop **never blocks** on missing data — it draws with what it last had.

| Missing            | Behaviour                                            |
|--------------------|------------------------------------------------------|
| Time (pre-NTP)     | `--:--:--`                                            |
| Weather / temp     | neutral palette + `--°` (like sim's default palette)  |
| Presence roster    | zero critters (empty grass)                           |
| WiFi / backend down| keep last-known values; short ~2–3 s HTTP timeout, retry |

The **starfield and fire faces need no backend data** — the moon phase is computed
from NTP time, so the starfield renders right (moon, stars, shooting stars, clock)
even if the backend is unreachable. Only the weather-driven dimming and cloud/rain
overlays go quiet without `/weather`. (The 23:30 switch itself is backend-driven,
so if the backend is down the panel holds whatever face it last had.)

---

## 5. Configuration & deployment

### 5.1 secrets.h (firmware, gitignored)

```cpp
#define WIFI_SSID         "..."
#define WIFI_PASSWORD     "..."
#define BACKEND_BASE_URL  "web-production-316b.up.railway.app"  // host only, no scheme/port
#define BACKEND_API_KEY   "..."   // must equal backend API_KEY env, byte-for-byte
```

`BACKEND_BASE_URL` is the **bare Railway host** — the firmware prepends `https://`
and appends the path. The ESP32 sends `X-API-Key: BACKEND_API_KEY` on every request.

### 5.2 Backend .env/local.env

```
WEATHER_LAT=-33.78     # Macquarie Park, Sydney (else weather.py defaults to Halifax!)
WEATHER_LON=151.12
API_KEY=...            # must match firmware BACKEND_API_KEY
```

### 5.3 Transport security

Backend is deployed on **Railway** (`*.up.railway.app`), which serves **HTTPS on
443 with a managed cert** and handles ingress. Traffic is encrypted end-to-end, so
the `X-API-Key` is not sent in cleartext. The ESP32 uses `WiFiClientSecure` with
`setInsecure()` (skips cert-chain verification — pragmatic for a hobby device; the
connection is still encrypted, just not authenticated against a pinned CA).

Test-time gotchas:
1. Railway must have the `API_KEY` env var set to the same value as the firmware's
   `BACKEND_API_KEY`, or every poll 401s and the face never switches. (If `API_KEY`
   is unset on Railway, auth is open — the server allows all.)
2. Railway may cold-start / sleep on some plans; the first poll after idle can be
   slow. The 3 s cadence + retry loop absorbs this.

> Railway hosts the CPU wherever it likes — the schedule is still anchored to
> `Australia/Sydney` in code, so the boundary times are unaffected by host region.

---

## 6. First-flash scope

| Included now (plumbing)                         | Deferred to pass 2                     |
|-------------------------------------------------|----------------------------------------|
| WiFi + NTP + two-core tasks                      | Weather palette on clock               |
| `/display/state` poll + hard-cut switching       | Date line, presence critters           |
| Bare clock (`HH:MM:SS`)                           | `/weather` + `/connected` polling wired |
| Full fire sim (ported from `simulator/sketch.js`)| Additional faces                       |

Prove the novel part (two cores + polling + switching without stutter) with a
glance-verifiable clock before porting the pixel-art.

---

## 7. Spotify "now playing" face

A fourth face (`spotify`) shows the current track: a 30×30 album cover, scrolling
title/artist, an elapsed timer, and a progress bar.

### 7.1 Why the backend does the work

The ESP32 can neither run Spotify's OAuth flow nor decode a JPEG mid-render, so
`backend/spotify.py` owns both:

- **Auth:** a one-time browser consent (`python3 -m backend.spotify_auth`) mints a
  long-lived `SPOTIFY_REFRESH_TOKEN`. The backend swaps it for short-lived access
  tokens and calls `GET /v1/me/player/currently-playing`. Results cache 2 s.
- **Album art:** the backend downloads Spotify's smallest image, resizes it to
  30×30 with **Pillow**, and serves raw **RGB565** (big-endian, 1800 bytes). The
  firmware just blits it — no decoding. Art is cached per `track_id`.

### 7.2 Endpoints

| Method | Route             | Response                                                        |
|--------|-------------------|----------------------------------------------------------------|
| GET    | `/spotify/state`  | JSON: `active, playing, title, artist, duration_ms, progress_ms, track_id, has_art` |
| GET    | `/spotify/art`    | raw RGB565 bytes (1800) for the current cover, or **204** if none |

`active:false` = Spotify closed/idle (→ idle screen). `playing:false` + `active:true`
= paused (→ dimmed track + pause glyph, frozen progress).

### 7.3 Firmware

- Polls `/spotify/state` every **5 s, only while the spotify face is showing** (so
  the Spotify API isn't hit all day). Fetches `/spotify/art` **only when `track_id`
  changes**.
- Elapsed time is **interpolated from `millis()`** between polls, so the bar sweeps
  smoothly while playing.
- Graceful degradation: no art → dim placeholder square; backend/Spotify down →
  keep last-known; nothing playing → idle screen.

### 7.4 Config

- Backend env: `SPOTIFY_CLIENT_ID`, `SPOTIFY_CLIENT_SECRET`, `SPOTIFY_REFRESH_TOKEN`;
  `requirements.txt` adds `Pillow`. Redirect URI `http://127.0.0.1:8888/callback`
  must be registered in the Spotify app.
- Firmware: **no new secrets** — reuses `BACKEND_BASE_URL` + `BACKEND_API_KEY`.
