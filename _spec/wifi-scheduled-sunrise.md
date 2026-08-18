# Spec: WiFi-Scheduled Sunrise (`SRM_Sunrise_Scheduled.ino`)

Describes the behavior of `arduino/standalone/SRM_Sunrise_Scheduled.ino`. Everything below describes what's **currently implemented** (v1 through v7 - see `_plans/wifi-scheduled-sunrise.md` for the version-by-version build record and design rationale; not everything below has been verified on real hardware yet, see that file's per-version verification checklists for what's confirmed vs. pending).

## Purpose

Runs the same 512-step sunrise color progression as the other sketches in this repo, but triggers it from an onboard per-day alarm schedule (set via a web page) instead of an external smart outlet cutting power, with an editable sunrise duration, an editable post-sunrise hold time, an editable (and auto-detectable) timezone, a manual "Test Run Sunrise" trigger, and phone-based WiFi setup that needs no source-code or Arduino IDE access.

## Timezone: web-editable, resolved via the ezTime library

**Driver:** the POSIX TZ string used to be a compile-time `#define`, meaning changing timezone required editing source and reflashing - not viable once WiFi setup no longer needs source access either (see above). It also required the owner to know/find their own POSIX TZ string, a niche format most people have never seen. An earlier version of this feature computed a best-guess POSIX string via client-side JavaScript (sampling the browser's own DST behavior); this was replaced by the **ezTime** library, which resolves a plain tz-database *location name* (e.g. `"America/Los_Angeles"`) to the correct POSIX rule authoritatively, instead of guessing from heuristics.

- New external dependency: **ezTime** (ropg/ezTime) - the second dependency in this repo beyond FastLED, alongside WiFiManager. All NTP sync and local-time handling now goes through it; `time.h`/`configTzTime()`/`getLocalTime()` are gone from this sketch entirely.
- `Timezone myTZ` (global object) replaces the raw `time.h` calls. `tzLocation` (runtime `String`, NVS-persisted under key `"tzLocation"`) holds the current location name, following the same pattern as `sunriseMinutes`/`holdMinutes`. `DEFAULT_TZ_LOCATION` (`"America/Los_Angeles"`) is only the first-boot fallback.
- `syncTime()` calls `myTZ.setLocation(tzLocation)`, then `waitForSync(NTP_SYNC_TIMEOUT_S)`. No `setCache()` is used - the installed ezTime build's NVS-backed cache overload only compiles into the library's own object file when a flag is defined ahead of *its* compilation, which a sketch-level `#define` can't reach (confirmed via a real link error), so a lookup failure just means no time until the next successful sync, same as every other network-dependent step in this sketch. `loop()` now also calls `events()` every iteration, which ezTime requires for its background NTP housekeeping.
- The settings form's Timing section has a **Timezone** text field (pre-filled with the current location name), a **preset dropdown** of common tz-database locations (a small hardcoded convenience list, not exhaustive - selecting one just fills the text field), and a **Detect from this device** button.
- **Detect from this device** reads `Intl.DateTimeFormat().resolvedOptions().timeZone` - a single standard JavaScript call that returns the browser's own IANA timezone name directly, correct DST rules and all (the browser/OS already knows this; no custom detection logic needed, unlike the earlier POSIX-heuristic approach). Fills the field but does **not** auto-submit, so it can be reviewed before hitting **Save**.
- `handleSet()` only calls `myTZ.setLocation()` again if the submitted location actually differs from the current one, and only if WiFi is connected - `setLocation()` requires a network lookup (unlike the old `configTzTime()`, which just reset a local environment variable), and the library's own docs ask for at least 3 seconds between calls, so an unrelated settings save no longer triggers a redundant lookup.
- `setLocation()` failures (unknown location, no network, lookup service down) are logged to Serial via `errorString()` but never block boot or the settings save - consistent with this sketch's existing fails-safe philosophy. There's no cache fallback (see above), so a failed lookup means no time is available until the next successful `syncTime()` call.

## WiFi setup without source-code access (captive portal provisioning)

**Driver:** WiFi credentials used to live in `wifi_secrets.h`, a file the end user had to hand-edit and reflash via Arduino IDE. That's fine for the person building the device, but not viable if it's gifted to someone with no access to the source code or dev tools — they need to be able to join their own network without any of that.

- New external dependency: **`WiFiManager`** (tzapu/WiFiManager). This is the first dependency beyond FastLED — a deliberate, explicit departure from this repo's zero-extra-deps convention, justified because there's no reasonable way to build out-of-box WiFi setup on top of the ESP32 Arduino core alone.
- `connectWiFi()` no longer calls `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` against a fixed hardcoded network. Instead it's `WiFiManager wm; wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S); wm.autoConnect(SETUP_AP_NAME)`. `autoConnect()` implements the whole flow:
  1. Tries previously-saved credentials (stored by WiFiManager in its own flash storage, separate from this sketch's `"sunrise"` Preferences namespace).
  2. If that fails within its connect timeout, it automatically starts an AP + captive portal **on that same boot** — no separate fallback logic needed, this is the library's default behavior.
  3. The recipient connects their phone to the broadcast network (`SETUP_AP_NAME`, `"sunrise-light-setup"`) using their phone's normal WiFi settings — no app required. A setup page (auto-popup or `192.168.4.1`) lists nearby networks; they pick theirs and enter the password.
  4. Device reboots and joins that network; normal operation (web UI, mDNS, scheduler) proceeds as before.
- `wifi_secrets.h` / `wifi_secrets.h.example` are **removed** — WiFiManager owns credential storage now. The `#include "wifi_secrets.h"` line and the `WIFI_SSID`/`WIFI_PASSWORD` `#define`s are gone from the sketch.
- `scanAndPrintNetworks()` (the temporary diagnostic added earlier to debug an SSID typo) is **removed** — redundant now that WiFiManager's captive portal scans and lists nearby networks itself.

**Re-provisioning, no new hardware required:**
- **Auto-fallback**: free, via `autoConnect()`'s default behavior above. If the saved network becomes unreachable (moved house, changed router), the very next boot drops into the setup hotspot automatically — no custom retry-counter logic needed.
- **Manual reset while still connected**: `POST /reset-wifi` route (`handleResetWifi()`) that calls `wm.resetSettings()` then `ESP.restart()`. A "Reset WiFi" button in the Device section of the status page asks for confirmation (JS `confirm()`) before submitting, since it forces an immediate reboot into setup mode.

**Caveats:**
1. While the captive portal is active (first boot with no saved creds, or right after a "Reset WiFi"), `setup()` blocks inside `wm.autoConnect()` until the user finishes setup or the config portal timeout elapses — LEDs, NTP sync, and the normal web server are all on hold during that window. This is a deliberate, bounded exception to the sketch's otherwise non-blocking philosophy, scoped only to rare setup events, never normal runtime.
2. Config portal timeout is **`WIFI_CONFIG_PORTAL_TIMEOUT_S`, 180 seconds (3 minutes)**. Resolved in favor of a bounded timeout over staying open indefinitely: since there's no physical reset button, an indefinitely-open portal after a stray "Reset WiFi" tap (with nobody around to finish setup) would strand the device with no path back except a power-cycle anyway — a timeout doesn't make that worse, and it keeps the fails-safe behavior consistent with the rest of the sketch (continue booting, don't hang). If it times out unconfigured, the board proceeds with no WiFi, same as v1/v2's behavior when WiFi never connected; the recipient would need to power-cycle to get the portal to try again.
3. During the portal, the device isn't reachable at `sunrise-light.local` or its normal IP — only at the setup AP's own address (typically `192.168.4.1`).
4. Credentials live in WiFiManager's own storage now rather than anywhere visible in this sketch's own NVS namespace or web UI. There's no "show current SSID" affordance needed though — the status page's existing `WiFi.SSID()`/`WiFi.localIP()` display keeps working regardless of how the connection was configured.

## Firmware updates via the web UI

**Driver:** WiFiManager already has a built-in `/update` OTA page (uses the ESP32 core's own `Update` library), but it only exists while WiFiManager's own internal web server is running — which, given this sketch's `connectWiFi()` creates a function-local `WiFiManager wm` object, is only during the brief captive-portal window (first boot or right after "Reset WiFi"), not during normal day-to-day operation. Verified working via that route once, then reimplemented directly against this sketch's own always-on `WebServer` so it's reachable any time the device is on the network, without needing to wipe WiFi credentials first.

- `GET /update` / `POST /update` (see "Web interface" below) added to the same `WebServer server` instance as every other route in this sketch - `#include <Update.h>` (ships with the ESP32 core, no library install needed) is the only addition.
- Upload flow is identical in spirit to WiFiManager's own: a `<input type='file'>` form posts multipart data; `WebServer::on()`'s upload-callback overload streams it chunk-by-chunk into `Update.write()` rather than buffering the whole file in RAM; a final response handler checks `Update.hasError()` and reboots regardless of outcome (a failed partial flash shouldn't be left half-applied).
- Requires the .bin from Arduino IDE's **Sketch → Export Compiled Binary** (or `arduino-cli compile --export-binaries`) - there's no on-device compiler, so the raw `.ino` can't be uploaded directly.
- Requires an OTA-capable partition scheme (Tools → Partition Scheme in Arduino IDE) with two app slots - the default "Default" scheme for ESP32 Dev Module already provides this, so no sketch-level change was needed.
- **No authentication** on `/update`, same as every other route in this sketch (`/start`, `/set`, `/reset-wifi`) - anyone on the local network can push firmware. Consistent with this sketch's existing security posture (no auth anywhere), but worth knowing given the "gift to a non-technical user" scenario the WiFi provisioning feature above was designed around - the device trusts its local network.

## Requirements

- ESP32 dev board, selected as **ESP32 Dev Module** in Arduino IDE.
- **Continuous power** (wall adapter / always-on USB). This sketch does not work with the smart-outlet-cuts-power model used by the other sketches — see `docs/Arduino_Hardware.md`.
- WiFi network with internet access (for NTP). 2.4GHz only, per ESP32 hardware.
- FastLED library (matches the rest of the repo; tested against 3.9.20 — avoid 3.10.3, see `CLAUDE.md`).
- **WiFiManager** (tzapu/WiFiManager) — installed via Arduino Library Manager. Provides the captive-portal WiFi setup flow below.
- **ezTime** (ropg/ezTime) — installed via Arduino Library Manager. Resolves the web-configured timezone location name to the correct offset/DST rule (see "Timezone" below); replaces this sketch's use of the ESP32 core's own `time.h`/`configTzTime()`/`getLocalTime()`.
- `WiFi.h`, `WebServer.h`, `Preferences.h`, `ESPmDNS.h` — ship with the ESP32 Arduino core, no extra libraries to install.
- No credentials file needed — WiFi network selection happens on-device via the captive portal (see below), not by editing source code.

## Configuration (`#define` block, top of file)

| Constant | Default | Meaning |
|---|---|---|
| `NUM_LEDS` | 24 | LED count |
| `DEFAULT_SUNRISE_MINUTES` | 30 | First-boot fallback only — the runtime value (`sunriseMinutes`, web-editable) comes from NVS once set |
| `DEFAULT_HOLD_MINUTES` | 60 | First-boot fallback only — the runtime value (`holdMinutes`, web-editable, range 0–120) comes from NVS once set |
| `DATA_PIN` | 18 | LED data GPIO |
| `SETUP_AP_NAME` | `"sunrise-light-setup"` | WiFi network name broadcast by the captive portal when no saved network is available |
| `WIFI_CONFIG_PORTAL_TIMEOUT_S` | 180 | Max seconds the captive portal stays open before giving up and continuing offline |
| `MDNS_HOSTNAME` | `"sunrise-light"` | Bare hostname only, no `.local` suffix — see in-code comment for why |
| `DEFAULT_TZ_LOCATION` | `"America/Los_Angeles"` | First-boot fallback only — the runtime value (`tzLocation`, web-editable/auto-detectable) comes from NVS once set |
| `NTP_SYNC_TIMEOUT_S` | 15 | Max seconds `setup()` waits for the first time sync (`waitForSync()`'s timeout is in seconds, not ms) |
| `WEB_SERVER_PORT` | 80 | HTTP port |

Compile-time defaults for the per-day schedule itself (used only until the web form is submitted once, since NVS has no value yet): every day defaults to 6:30, all **disabled**.

## Behavior

### Boot sequence (`setup()`)

1. LEDs initialized and set to black.
2. `connectWiFi()` — `WiFiManager::autoConnect(SETUP_AP_NAME)` tries the previously-saved network; if that fails, opens the `sunrise-light-setup` captive portal for up to `WIFI_CONFIG_PORTAL_TIMEOUT_S` (180s), blocking `setup()` for that entire window; then proceeds regardless of outcome (does not halt on failure or timeout).
3. `startMDNS()` — no-ops if WiFi didn't connect; otherwise starts the mDNS responder so the device is reachable at `http://sunrise-light.local/`.
4. `syncTime()` — no-ops if WiFi didn't connect; otherwise sets up ezTime's NVS cache, calls `myTZ.setLocation(tzLocation)`, and calls `waitForSync(NTP_SYNC_TIMEOUT_S)`.
5. `loadAlarmSettings()` — reads the per-day schedule plus `sunriseMinutes`/`holdMinutes`/`tzLocation` from NVS namespace `"sunrise"`, falling back to the compile-time defaults above if unset. Runs *before* `syncTime()` since the timezone lookup needs `tzLocation` already loaded.
6. Web routes registered, server started.

If WiFi, the captive portal, or NTP fail, boot still completes — see "Fails-safe behavior" below.

### Main loop

Every `loop()` iteration, unconditionally and non-blockingly:
1. `events()` — ezTime's own background NTP/cache housekeeping; required every iteration per its own docs.
2. `server.handleClient()` — services any pending HTTP request.
3. `updateScheduler()` — advances the state machine (below).
4. `FastLED.show()` — pushes whatever `leds[]` currently holds.

### State machine

Three states: `WAITING`, `RUNNING`, `HOLD`.

**WAITING** (LEDs off). Requires wall-clock time to evaluate — if `timeStatus() != timeSet` (no NTP sync yet), this case does nothing that tick. When time is available, if all of the following hold for *today's* day-of-week (`myTZ.dateTime("w").toInt() - 1`, giving 0=Sunday…6=Saturday to match `dayHour[]`/`dayMinute[]`/`dayEnabled[]`'s indexing — the installed ezTime build's `dateTime("w")` actually returns **1=Sunday…7=Saturday** despite its own "0 = Sunday" doc comment, confirmed by reading `ezTime.cpp`'s `breakTime()`; the `- 1` corrects for that):
- `dayEnabled[wday]` is true
- current `myTZ.hour()`/`myTZ.minute()` exactly equal `dayHour[wday]`/`dayMinute[wday]`
- the alarm hasn't already fired today (`lastFiredYday != myTZ.dayOfYear()`)

...then `beginSunrise()` runs: `currentStep` resets to 0, `lastFiredYday` is set to today, the sunrise timer's period is recomputed from the current `sunriseMinutes` and applied via `sunriseTimer.setPeriod()`/`reset()`, and state moves to `RUNNING`.

Because the match is on exact hour/minute equality (checked many times per second), a missed minute — e.g. the board reboots or is busy exactly during that minute — means the alarm will **not** fire until its next scheduled day. There is no "catch-up" logic.

**RUNNING**: calls `sunrise()` every tick (see below). Once `currentStep` reaches 511, records `holdStartMillis = millis()` and moves to `HOLD`.

**HOLD**: LEDs hold at the final sunrise color (no changes made; `leds[]` already has the value from the last `sunrise()` call). Once `millis() - holdStartMillis >= holdMinutes * 60000`, LEDs are set to black and state returns to `WAITING`.

**Only `WAITING` needs wall-clock time** — `RUNNING` and `HOLD` are driven entirely by `millis()`, so a sunrise already in progress (or a manually-triggered one, see `beginSunrise()` below) keeps running/holding correctly even if NTP has never synced or WiFi drops mid-sunrise.

### `beginSunrise()` — manual and scheduled trigger, shared

The "enter `RUNNING`" logic is factored into a single `beginSunrise()` function, called from two places: `updateScheduler()`'s `WAITING` case (above) when the schedule matches, and `handleStartNow()` (below) when the web UI's **Start Now** button is pressed. Both paths reset `currentStep`, retune the sunrise timer, and mark today as fired (suppressing the schedule from also firing later that same day) — except that if wall-clock time isn't available, marking "today as fired" is silently skipped (there's no date to record), while the sunrise itself still runs, since it's `millis()`-driven, not date-driven.

### `sunrise()` color math

Unchanged from `SRM_Sunrise_NonBlocking.ino` / `SRM_Sunrise_ESP32.ino`:
- 512 steps, advanced via a step timer.
- Hue: 0 (red) → 45 (yellow-orange).
- Saturation: 255 → 180.
- Brightness: quadratic ease, `progress² / 255`.

Two structural differences from the other sketches:
- `currentStep` lives at file scope (not `static` inside the function), so `beginSunrise()` can reset it to 0 on each new firing.
- The step timer (`sunriseTimer`) is a file-scope `CEveryNMillis` object, declared directly rather than via FastLED's `EVERY_N_MILLISECONDS_I` macro. The macro creates a *function-local* static timer that can't be reached from outside `sunrise()` — declaring the class directly lets `beginSunrise()` call `sunriseTimer.setPeriod()`/`.reset()` to retune it for the current runtime `sunriseMinutes`, since that value can change via the web form and a `static`-initialized-once timer wouldn't pick up the change.

### Persistence (NVS, namespace `"sunrise"`)

| Key pattern | Type | Meaning |
|---|---|---|
| `d0h`…`d6h` | uint8 (0–23) | Per-day hour, one key per day of week |
| `d0m`…`d6m` | uint8 (0–59) | Per-day minute |
| `d0e`…`d6e` | bool | Per-day enabled |
| `sunriseMinutes` | uint16 | Sunrise ramp duration |
| `holdMinutes` | uint8 | 0–120, minutes to hold the final color before auto-off |
| `tzLocation` | string | tz-database location name (e.g. `"America/Los_Angeles"`), hand-typed, picked from the preset dropdown, or filled by "Detect from this device" |

Keys for the 7 days are built programmatically (`"d" + i + "h"`, etc.) in a loop rather than hand-written. Written only when the web form is submitted (`handleSet()` → `saveAlarmSettings()`). Survives reboot and power loss. `lastFiredYday` is **not** persisted — it lives in RAM only and resets to `-1` on every boot.

## Web interface

### `GET /`

Returns an HTML status/config page styled as grouped white cards on a light gray background (iOS-Settings-like: uppercase section labels, pure-CSS toggle switches, one warm accent color `#ff9500`, no external fonts/CSS - the whole `<style>` block is inline since the page must render standalone from the ESP32). The page body caps at `max-width:480px` and centers itself (`margin:0 auto`), so it stays comfortably phone-width on a desktop browser without stretching edge-to-edge, while being a no-op on an actual phone (viewport already narrower than that). Each field's explanatory caption is grouped with the field above it, not the one below, via a shared `.item` wrapper that carries the divider between groups rather than the field itself. Layout, top to bottom:
- A hero card: current local time (or "Time not synced yet"), a colored state pill (`Waiting`/`Running N%`/`Holding` - gray/amber/green respectively), WiFi IP (or "WiFi disconnected"), mDNS address, and firmware version (`FIRMWARE_VERSION`)
- A settings form (posts to `/set`), itself split into three card sections:
  - **Schedule**: one row per day of the week (Sun…Sat), each with a day label, a single native `<input type="time">` field (renders as the phone's own time-picker UI), and a toggle switch for enabled - replaces the old separate hour/minute number inputs
  - **Timing**: sunrise duration in minutes (with a short explanation), hold-before-auto-off in minutes (0–120, with a short explanation)
  - **Timezone**: a location text field, a "Detect from this device" button (fills the field from the browser's own `Intl.DateTimeFormat` timezone), and a preset dropdown of common zones
  - A full-width **Save** button
- A **Device** card, deliberately separated from the settings form/save flow above (each of these acts immediately, not on Save): **Test Run Sunrise** (renamed from "Start Now" - own form, posts to `/start`, with an explanatory line noting it doesn't touch the schedule), **Reset WiFi** (own form, posts to `/reset-wifi`, JS confirm prompt since it forces an immediate reboot into setup mode, with an explanatory line noting other settings aren't affected), **Firmware Update** (link to `/update`)

### `POST /set`

Reads `sunriseMinutes` (clamped to a minimum of 1 — no upper cap), `holdMinutes` (clamped to a maximum of 120), `tzLocation` (trimmed; ignored if empty, over 60 characters, or unchanged from the current value, in which case the previous value is kept), and for each of the 7 days, `dNt`/`dNe` (`dNt` is the `<input type="time">` value, `"HH:MM"` 24-hour per the HTML spec regardless of display locale - parsed via `indexOf(':')`, hour clamped ≤23, minute clamped ≤59, kept at its previous value if the field is missing/malformed rather than zeroed; `dNe` checkbox presence = enabled). Saves everything to NVS in one transaction (`saveAlarmSettings()`), updates the in-RAM cache, and — only if the timezone actually changed and WiFi is connected — re-resolves it immediately via `myTZ.setLocation()`. Responds with a 303 redirect back to `/`.

### `POST /start`

Calls `beginSunrise()` — immediately starts a sunrise regardless of current state or schedule, using the current `sunriseMinutes`. Responds with a 303 redirect back to `/`.

### `POST /reset-wifi`

Calls `WiFiManager::resetSettings()` to forget the saved network, sends a plain-text confirmation, then `ESP.restart()`s after a 1s delay. On reboot, `autoConnect()` finds no saved credentials and immediately opens the `sunrise-light-setup` captive portal.

### `GET /update`

Returns a plain HTML form (`<input type='file'>` + submit) for uploading a firmware `.bin`, plus a link back to `/`. Distinct from - and, unlike - WiFiManager's own built-in `/update` page (see "Firmware updates" below): reachable at any time the device is online, not just during the transient captive-portal window.

### `POST /update`

Multipart file upload, handled via `WebServer::on()`'s upload-callback overload (`handleFirmwareUpload()` processes each chunk as it arrives through the `Update` library - `Update.begin()`/`.write()`/`.end(true)` - rather than buffering the whole file; `handleFirmwareUpdate()` runs once the body is fully received, reports success/failure via `Update.hasError()`, and calls `ESP.restart()` either way after a 1s delay). The uploaded file must be a `.bin` produced by Arduino IDE's **Sketch → Export Compiled Binary** (or `arduino-cli compile --export-binaries`) - the ESP32 has no on-device compiler, so raw `.ino` source can't be uploaded here.

### Anything else

Any other path returns `404 Not found` (plain text).

## Fails-safe behavior (known limitations, not bugs)

- **No WiFi at boot (portal times out or nobody configures it)**: sketch continues; `/` reports "disconnected" and "not synced yet"; the `WAITING` schedule check never runs (no time available), so nothing fires on schedule. Manually pressing **Test Run Sunrise** still works, since running a sunrise doesn't need wall-clock time. A power-cycle is needed to make the captive portal try again.
- **NTP sync fails**: same effect as no WiFi — the schedule can't evaluate, but a `RUNNING`/`HOLD` sunrise already in progress, or one started manually, proceeds and completes normally.
- **Alarm already fired today, then board reboots**: `lastFiredYday` resets to `-1` in RAM, so if that day's alarm time is reached again post-reboot, it **will** fire a second time that day.
- **Missed the exact trigger minute** (e.g. board busy or rebooting during that minute): no catch-up; waits for the next scheduled day.

## Explicitly out of scope (current implementation)

- Bluetooth/BLE interface.
- RTC hardware module (DS3231 or similar) — time comes from NTP only.
- WiFi reconnect/retry loop after the initial boot-time attempt (`autoConnect()`'s own saved-credential retry only, no continuous background reconnect).
- One-shot (non-recurring) alarms — every enabled day repeats weekly.
- Physical reset button for WiFi provisioning — re-provisioning is web-button (`/reset-wifi`) or automatic fallback only.

## Verification checklist (real hardware)

See `_plans/wifi-scheduled-sunrise.md` for the full hardware test plan (WiFi captive-portal setup and reset, NTP accuracy incl. DST, web form round-trip including Test Run Sunrise, correct-day/wrong-day firing, NVS persistence across power-cycle, LED hold with no flicker and correct auto-off timing, responsiveness while `RUNNING`, mDNS resolution, and OTA firmware updates via `/update`).
