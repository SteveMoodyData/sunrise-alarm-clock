# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

A gradual sunrise-simulation alarm clock built on WS2812B addressable LEDs, driven by an Arduino Nano/ESP32-class microcontroller. There is no app, server, or build system — the repo is Arduino `.ino` sketches plus documentation. Sunrise is triggered by cutting power to the microcontroller (e.g. via a smart outlet on a schedule); the sketch runs the full color progression once on boot, then holds.

## Repo layout

- `arduino/standalone/` — the actual firmware, as standalone `.ino` sketches (see below). `arduino/README.md` is a landing page pointing here.
- `wled/` — placeholder for a planned WLED custom-effect implementation (alternative to the standalone Arduino version); not yet implemented (`wled/README.md` is a stub).
- `docs/Arduino_Hardware.md` — bill of materials, wiring diagram/pinout, soldering steps, power budget, enclosure ideas, and the author's own build (Arduino Nano clone + 24-pixel WS2812B ring + Zigbee outlet + SmartThings).
- Root `README.md` — project overview; note it links to several docs files (`docs/smartthings_setup.md`, `docs/comparison.md`, `docs/troubleshooting.md`, `docs/hardware.md`) that don't exist yet — don't assume they're present.

## The sketches (`arduino/standalone/`)

Four `.ino` files implementing the same idea with increasing sophistication. There is no shared library code — each file is fully self-contained and duplicates the LED setup boilerplate.

- **`SRM_Sunrise_Smooth.ino` — the recommended, current version.** Blocking: one `for` loop of 512 steps over `SUNRISE_MINUTES`, each step computing an HSV color via `map()` (hue 0→45, saturation 255→180, brightness via a quadratic ease `progress²/255`), then `delay(stepDelay)`. Holds final color for an hour after completion, then the Arduino would restart the loop (in practice it's power-cycled before that matters).
- **`SRM_Sunrise_NonBlocking.ino`** — same math as Smooth, restructured to be non-blocking: state (`currentStep`) is `static` inside `sunrise()`, advanced via FastLED's `EVERY_N_MILLISECONDS_I` timer macro instead of `delay()`, and `loop()` calls `sunrise()` every iteration. Use this as the base for any change that needs the MCU to stay responsive during the sunrise (buttons, sensors, serial, a display).
- **`SRM_Sunrise_Test1.ino`** / **`SRM_Sunrise_Test2.ino`** — legacy/original versions kept for reference. Discrete (not smooth) HSV steps, manually unrolled loops that light LEDs symmetrically outward from the center, fixed to red→orange only. Test2 is a 5-minute variant of Test1 for faster manual testing. Not used as a base for new work.
- **`SRM_Sunrise_ESP32.ino`** — port of NonBlocking for ESP32 boards. Same color/timing math; differs only in `DATA_PIN` (18, since Nano's `D9` silkscreen label doesn't exist on ESP32 dev boards — avoid strapping pins 0/2/12/15, flash pins 6-11, and input-only pins 34-39) and a note about ESP32's 3.3V logic vs WS2812B's ~5V data threshold (add a level shifter or series resistor if LED 0 flickers/misreads).
- **`SRM_Sunrise_Scheduled.ino`** — WiFi-scheduled variant built on the ESP32 target, and the one sketch in this repo with real external dependencies beyond FastLED: **WiFiManager** and **ezTime**. Serves a web page (`WebServer.h`, reachable at `http://sunrise-light.local/` via `ESPmDNS.h`) with a fully independent alarm (hour/minute/enabled) per day of the week, an editable sunrise duration and post-sunrise hold time, a manual "Start Now" trigger, and a "Reset WiFi" trigger — all persisted across reboots via `Preferences.h` (NVS, namespace `"sunrise"`). A `WAITING → RUNNING → HOLD` state machine gates the same unchanged `sunrise()` color math behind the alarm check instead of running it unconditionally at boot. WiFi setup needs no source access or reflash: `WiFiManager::autoConnect()` opens a `sunrise-light-setup` captive portal on first boot (or after "Reset WiFi") instead of using hardcoded credentials. Timezone is likewise set through the web UI (typed, picked from a preset dropdown, or auto-filled from the browser's own `Intl.DateTimeFormat` via "Detect from this device") as a tz-database location name (e.g. `"America/Los_Angeles"`), resolved to the correct offset/DST rule by **ezTime**, which also replaces this sketch's NTP/local-time handling (`time.h`/`getLocalTime()` aren't used here). Architecturally different from every other sketch here: **the board must stay powered continuously** (wall adapter, not the smart outlet) since it now owns the schedule itself — see `docs/Arduino_Hardware.md`.

Per-sketch constants to know when touching any of them: `NUM_LEDS`, `SUNRISE_MINUTES` (or `MINUTES` in the Test sketches), `DATA_PIN` (GPIO 9 on the Nano sketches, GPIO 18 on the ESP32 one), and the FastLED chipset/color-order line (`FastLED.addLeds<WS2812B, DATA_PIN, GRB>(...)`, GRB is the default color order — try RGB if colors come out wrong on new hardware).

## ESP32-specific notes

- Requires the `esp32` board package (Espressif) in Arduino IDE's Boards Manager — add `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json` under Preferences → Additional Board Manager URLs, then select **ESP32 Dev Module** under Tools → Board.
- ESP32 GPIOs are 3.3V logic; WS2812B data nominally wants ~5V. Short data wire + LED0 close to the MCU often works unmodified — if not, add a 74AHCT125-style level shifter or a ~330-500 ohm series resistor on the data line.
- ESP32 has WiFi + BLE, which the Nano lacks — that's the natural place to add remote-trigger/web-config features later (see the WLED-effect idea in `wled/`), but the current `SRM_Sunrise_ESP32.ino` doesn't use either yet.

## Working in this repo

- **No build/lint/test tooling.** Verification is: open a sketch in the Arduino IDE (or `arduino-cli compile`/`upload` if available), compile, and flash to real hardware — there's no simulator, headless build script, or CI in this repo. If asked to add one (e.g. `arduino-cli` based compile check), check first before assuming it exists.
- **`FastLED` is the only external dependency for every sketch except `SRM_Sunrise_Scheduled.ino`**, which also deliberately takes on **WiFiManager** and **ezTime** (see above) — explicit exceptions to this repo's normal zero-extra-deps convention, justified because there's no reasonable way to build captive-portal WiFi setup or authoritative timezone resolution on the ESP32 core alone. All color work goes through FastLED's `CHSV`/`CRGB` types and `fill_solid`/`FastLED.show()`.
- When adding a new sketch variant, follow the existing convention: a fully self-contained `.ino` with the same `#define` block at the top (`NUM_LEDS`, `SUNRISE_MINUTES`, `DATA_PIN`), rather than factoring out shared code — that's the pattern every existing sketch follows, even though it duplicates the setup boilerplate.
- Prefer basing new interactive features (buttons, sensors, RTC, display) on `SRM_Sunrise_NonBlocking.ino`, not `SRM_Sunrise_Smooth.ino`, since blocking `delay()` calls in the Smooth version make the MCU unresponsive for the entire sunrise duration.
- Hue/brightness/saturation curves are hand-tuned via `map()` calls with magic numbers documented inline (e.g. hue range 0–45 = red to yellow-orange, brightness minimum ~5–13/255 so the first step is dimly visible rather than off). When changing the color curve, keep changes consistent across whichever sketches you're updating — Smooth and NonBlocking currently share identical color math and are expected to stay in sync unless deliberately diverging.
