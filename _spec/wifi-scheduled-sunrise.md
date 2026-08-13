# Spec: WiFi-Scheduled Sunrise (`SRM_Sunrise_Scheduled.ino`)

Describes the behavior of `arduino/standalone/SRM_Sunrise_Scheduled.ino`. Everything below "Purpose" through "Verification checklist" describes what's **currently implemented and working on real hardware** (v1). The "Proposed changes (v2)" section immediately after "Purpose" describes four new features requested but **not yet implemented** — pending review before any code changes are made. For the design rationale behind the v1 shape, see `_plans/wifi-scheduled-sunrise.md`.

## Purpose

Runs the same 512-step sunrise color progression as the other sketches in this repo, but triggers it from an onboard weekly alarm schedule (set via a web page) instead of an external smart outlet cutting power.

## Proposed changes (v2 — pending review, not yet implemented)

Four features requested after v1 was confirmed working on real hardware. No code has been touched for these yet. Each includes an implementation note where the change isn't a simple additive tweak, and open questions are called out explicitly rather than decided here.

### 1. mDNS hostname (`sunrise-light`)

Replace/augment IP-address access with a friendly hostname.

- Add `#include <ESPmDNS.h>` (ships with ESP32 Arduino core, no new dependency).
- New config constant `#define MDNS_HOSTNAME "sunrise-light"`. **This constant holds the bare name only — do not include `.local` in it.** `MDNS.begin()` takes the bare hostname; the `.local` suffix is appended automatically by the resolver, not stored by the device. Passing `"sunrise-light.local"` here would make the device advertise itself as `sunrise-light.local.local`, which is wrong.
- After a successful WiFi connect: `MDNS.begin(MDNS_HOSTNAME)` then `MDNS.addService("http", "tcp", WEB_SERVER_PORT)`.
- Device becomes reachable at `http://sunrise-light.local/` in addition to its IP (the `.local` suffix only ever appears in the URL you type/resolve, never in the sketch's config).

**Caveat to flag, not a blocker:** `.local` (mDNS/Bonjour) resolution is native on macOS, iOS, Android, and Linux, but **Windows does not resolve `.local` names out of the box** — it needs Bonjour installed (comes bundled with iTunes, or standalone "Bonjour Print Services") or Windows' own mDNS support (present on recent Windows 10/11, not guaranteed on older versions). Worth confirming this works from whatever device you'll actually use day-to-day before relying on it instead of the IP.

### 2. Runtime-configurable sunrise duration

Today `SUNRISE_MINUTES` is a compile-time `#define` — changing it requires re-flashing. Make it a web-editable, NVS-persisted setting.

- New NVS key `sunriseMinutes` (uint16, minutes), replacing the compile-time-only value as the source of truth at runtime. Keep a `#define DEFAULT_SUNRISE_MINUTES` for first-boot-before-any-save fallback.
- Add a number input to the web form; `handleSet()` validates and saves it.

**Implementation note (not a design question, just a real constraint):** `sunrise()`'s step interval is currently computed once via `static const uint16_t interval = (SUNRISE_MINUTES * 60000UL) / totalSteps;` and handed to FastLED's `EVERY_N_MILLISECONDS_I(sunriseTimer, interval)` macro. Because `interval` is `static`, it's only evaluated the *first* time `sunrise()` runs — changing the underlying variable afterward won't retroactively change an already-running timer's period. Making duration runtime-editable means explicitly recomputing `interval` and calling `sunriseTimer.setPeriod(interval)` (the `_I` "instance" variant of the macro exists specifically to allow this) at the point `RUNNING` is entered, not just relying on the macro's implicit static init.

### 3. Hold-time variable (auto-off after sunrise)

Today, `HOLD` holds the final color indefinitely and only turns off at local midnight (day-rollover triggers the transition back to `WAITING`). Replace this with an explicit, configurable duration.

- New NVS key `holdMinutes` (uint8 is sufficient — range below fits in 0–120 — minutes to hold the final color after the sunrise ramp finishes, before turning LEDs off). `#define DEFAULT_HOLD_MINUTES 60`.
- **Valid range: 0–120 (2 hours), default 60.** `0` is explicitly valid — LEDs go straight to black the instant the ramp completes. `handleSet()` clamps any submitted value to this range the same way `hour`/`minute` are already clamped today.
- On entering `HOLD`, record `holdStartMillis = millis()`.
- `HOLD` exits to `WAITING` (LEDs set to black) once `millis() - holdStartMillis >= holdMinutes * 60000UL`, **replacing** the current midnight-rollover exit condition entirely.
- The existing `lastFiredYday` guard is untouched and still independently prevents re-firing later the same day, even though the state machine now leaves `HOLD` well before midnight in the typical case.

### 4. Independent per-day schedule

Resolved: **not** a fixed weekday/weekend split — each of the 7 days gets its own independently configurable alarm time and enabled toggle, so e.g. Mon/Wed/Fri can be set to 6:30, Sat/Sun to 7:30, and Tue/Thu left disabled entirely, all as individual per-day settings (a weekday/weekend split couldn't express "M/W/F but not Tu/Th" without still being per-day underneath, so this replaces that idea rather than sitting alongside it).

- Replaces the v1 `alarmHour`/`alarmMinute`/`alarmDaysMask`/`alarmEnabled` single-alarm-plus-mask model entirely.
- In-RAM state becomes arrays indexed by `tm_wday` (0=Sunday…6=Saturday): `uint8_t dayHour[7]`, `uint8_t dayMinute[7]`, `bool dayEnabled[7]`.
- NVS keys built programmatically per index in a loop (e.g. `"d0h"/"d0m"/"d0e"` … `"d6h"/"d6m"/"d6e"`) rather than 21 hand-written named constants — `loadAlarmSettings()`/`saveAlarmSettings()` iterate `for (int i = 0; i < 7; i++)` instead of reading/writing fixed keys.
- `updateScheduler()`'s `WAITING` check becomes: `dayEnabled[now.tm_wday] && now.tm_hour == dayHour[now.tm_wday] && now.tm_min == dayMinute[now.tm_wday] && lastFiredYday != now.tm_yday`.
- Web form becomes 7 rows (Sun…Sat), each with its own enabled checkbox, hour input, and minute input, pre-filled from the current per-day arrays. `handleSet()` reads and validates all 7 rows in a loop, same clamping rules as today (hour ≤23, minute ≤59) applied per day.

## Requirements

- ESP-WROOM-32 board (or compatible ESP32 dev board), selected as **ESP32 Dev Module** in Arduino IDE.
- **Continuous power** (wall adapter / always-on USB). This sketch does not work with the smart-outlet-cuts-power model used by the other sketches — see `docs/Arduino_Hardware.md`.
- WiFi network with internet access (for NTP). 2.4GHz only, per ESP32 hardware.
- FastLED library (matches the rest of the repo; tested against 3.9.20 — avoid 3.10.3, see `CLAUDE.md`).
- `WiFi.h`, `WebServer.h`, `Preferences.h`, `time.h` — all ship with the ESP32 Arduino core, no extra libraries to install.

## Configuration (`#define` block, top of file)

| Constant | Default | Meaning |
|---|---|---|
| `NUM_LEDS` | 24 | LED count |
| `SUNRISE_MINUTES` | 30 | Sunrise duration |
| `DATA_PIN` | 18 | LED data GPIO |
| `WIFI_SSID` / `WIFI_PASSWORD` | placeholder strings | Must be edited before flashing |
| `WIFI_CONNECT_TIMEOUT_MS` | 15000 | Max time `setup()` waits for WiFi before giving up and continuing offline |
| `TZ_STRING` | `"EST5EDT,M3.2.0,M11.1.0"` | POSIX TZ string; must be edited for your timezone |
| `NTP_SERVER` | `"pool.ntp.org"` | NTP source |
| `NTP_SYNC_TIMEOUT_MS` | 15000 | Max time `setup()` waits for the first time sync |
| `WEB_SERVER_PORT` | 80 | HTTP port |

Compile-time defaults for the alarm itself (used only until the web form is submitted once, since NVS has no value yet): hour `6`, minute `30`, days `Mon–Fri` (`0b0111110`), enabled `false`.

## Behavior

### Boot sequence (`setup()`)

1. LEDs initialized and set to black.
2. `connectWiFi()` — connects to `WIFI_SSID`; blocks up to `WIFI_CONNECT_TIMEOUT_MS`, then proceeds regardless of outcome (does not halt on failure).
3. `syncTime()` — no-ops if WiFi didn't connect; otherwise calls `configTzTime()` and polls `getLocalTime()` up to `NTP_SYNC_TIMEOUT_MS`.
4. `loadAlarmSettings()` — reads alarm hour/minute/days/enabled from NVS namespace `"sunrise"`, falling back to the compile-time defaults above if unset.
5. Web routes registered, server started.

If WiFi or NTP fail, boot still completes — see "Fails-safe behavior" below.

### Main loop

Every `loop()` iteration, unconditionally and non-blockingly:
1. `server.handleClient()` — services any pending HTTP request.
2. `updateScheduler()` — advances the state machine (below).
3. `FastLED.show()` — pushes whatever `leds[]` currently holds.

### State machine

Three states: `WAITING`, `RUNNING`, `HOLD`.

**WAITING** (LEDs off). On every tick, if all of the following hold:
- `alarmEnabled` is true
- today's `tm_wday` bit is set in `alarmDaysMask`
- current `tm_hour`/`tm_min` exactly equal `alarmHour`/`alarmMinute`
- the alarm hasn't already fired today (`lastFiredYday != tm_yday`)

...then `currentStep` resets to 0, `lastFiredYday` is set to today, and state moves to `RUNNING`.

Because the match is on exact hour/minute equality (checked once per `loop()` iteration, effectively many times per second), a missed minute — e.g. the board reboots or is busy exactly during that minute — means the alarm will **not** fire until its next scheduled day. There is no "catch-up" logic.

**RUNNING**: calls `sunrise()` every tick (see below). Once `currentStep` reaches 511, moves to `HOLD`.

**HOLD**: LEDs hold at the final sunrise color (no changes made; `leds[]` already has the value from the last `sunrise()` call). When local calendar day changes (`tm_yday != lastFiredYday`), LEDs are set to black and state returns to `WAITING`.

### `sunrise()` color math

Unchanged from `SRM_Sunrise_NonBlocking.ino` / `SRM_Sunrise_ESP32.ino`:
- 512 steps over `SUNRISE_MINUTES`, advanced via FastLED's `EVERY_N_MILLISECONDS_I` timer macro.
- Hue: 0 (red) → 45 (yellow-orange).
- Saturation: 255 → 180.
- Brightness: quadratic ease, `progress² / 255`.

Difference from the other sketches: `currentStep` lives at file scope (not `static` inside the function), so `updateScheduler()` can reset it to 0 on each new alarm firing.

### Persistence (NVS, namespace `"sunrise"`)

| Key | Type | Range |
|---|---|---|
| `alarmHour` | uint8 | 0–23 |
| `alarmMinute` | uint8 | 0–59 |
| `alarmDays` | uint8 bitmask | bit0=Sunday … bit6=Saturday (matches `struct tm.tm_wday`) |
| `alarmEnabled` | bool | — |

Written only when the web form is submitted (`handleSet()`). Survives reboot and power loss. `lastFiredYday` is **not** persisted — it lives in RAM only and resets to `-1` on every boot.

## Web interface

### `GET /`

Returns an HTML status/config page showing:
- Current local time (or "not synced yet" if NTP hasn't succeeded)
- WiFi status (IP address, or "disconnected")
- Sunrise state (`Waiting` / `Running` / `Holding`) and progress percentage (0 in `WAITING`, `currentStep/511` in `RUNNING`, 100 in `HOLD`)
- A form (posts to `/set`) with: hour number input (0–23), minute number input (0–59), seven day checkboxes (Sun–Sat), an enabled checkbox, and a submit button. Fields are pre-filled with current values.

### `POST /set`

Reads form fields `hour`, `minute`, `day0`…`day6` (checkbox presence = that day is set), `enabled` (checkbox presence = true). Clamps `hour` to ≤23 and `minute` to ≤59 (no error is shown if out-of-range values were submitted — they're silently clamped). Builds the day bitmask from whichever `dayN` checkboxes were present. Saves to NVS and updates the in-RAM cache. Responds with a 303 redirect back to `/`.

### Anything else

Any other path returns `404 Not found` (plain text).

## Fails-safe behavior (known limitations, not bugs)

- **No WiFi at boot**: sketch continues; `/` reports "disconnected" and "not synced yet"; scheduler never matches (no time available), so it simply never fires. No crash, no retry — WiFi is only attempted once, in `setup()`.
- **NTP sync fails**: same outcome as above — waits forever for a first successful `getLocalTime()` read before the scheduler can do anything.
- **Alarm already fired today, then board reboots**: `lastFiredYday` resets to `-1` in RAM, so if the alarm's hour/minute is reached again the same day post-reboot, it **will** fire a second time that day.
- **Missed the exact trigger minute** (e.g. board busy or rebooting during that minute): no catch-up; waits for the next scheduled day.

## Explicitly out of scope (v1)

- Bluetooth/BLE interface.
- RTC hardware module (DS3231 or similar) — time comes from NTP only.
- WiFi credential provisioning via captive portal / WiFiManager — credentials are hardcoded in the sketch.
- WiFi reconnect/retry loop after the initial boot-time attempt.
- One-shot (non-recurring) alarms — every enabled alarm repeats weekly on its selected days.

## Verification checklist (real hardware)

See `_plans/wifi-scheduled-sunrise.md` for the full 9-step hardware test plan (WiFi connect, NTP accuracy incl. DST, web form round-trip, correct-day/wrong-day firing, NVS persistence across power-cycle, LED hold with no flicker, responsiveness while `RUNNING`, and day-rollover re-arm).
