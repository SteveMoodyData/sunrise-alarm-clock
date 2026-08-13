# WiFi-Scheduled Sunrise (ESP32)

## Context

The sunrise clock currently has no way to schedule itself: a smart outlet cuts power to the ESP32 on an external schedule (SmartThings + Zigbee outlet), and the sketch just runs its 512-step color progression once on boot. The ESP32 board (`SRM_Sunrise_ESP32.ino`) was verified working on real ESP32 hardware — LED array confirmed good.

Decisions made building v1:
- **WiFi only, no BLE** — WiFi gets free NTP time sync and a phone-browser-reachable web page with no companion app; BLE needs an actively-paired phone and no wall-clock reference of its own.
- **Time source: WiFi + NTP only, no RTC module** — despite having DS3231 libraries from unrelated past projects, the user chose not to add RTC hardware for this. The board must sync time from the internet at boot.
- **The board must stay powered continuously** (wall adapter, not the smart outlet) so it can keep WiFi/NTP time and watch for its own alarm trigger. The outlet-cuts-power trigger model is retired for this variant.

## Status

**v1 and v2 are both implemented, pushed, and confirmed working on real hardware.** v3 (below) is proposed and reviewed but not yet implemented — the sections up through "v3" describe what's actually running on the device today; treat them as a build record, not a to-do list.

**v2 shipped these four additions** on top of the working v1 code, requested after v1 was confirmed working and reviewed/resolved against the user in `_spec/wifi-scheduled-sunrise.md`:
1. mDNS hostname (`sunrise-light.local`)
2. Runtime-configurable sunrise duration (was a compile-time `#define`, now web-editable/NVS-persisted)
3. Configurable hold-then-auto-off timer (replaced holding until local midnight)
4. Fully independent per-day schedule (replaced one time + a day-of-week mask with 7 separate hour/minute/enabled slots, since the user gave an example — M/W/F at 6:30, Sa/Su at 7:30, Tue/Thu off — that a weekday/weekend split can't express)

**Two more things landed during/after v2 that this plan didn't originally call for** — see "Also shipped: Start Now button + fails-safe time fix" below for what and why.

## File: `arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`

Stays a fully self-contained `.ino`, consistent with every other sketch in this repo (`arduino/README.md`, `CLAUDE.md`) — no shared library code. The sunrise color math itself (hue 0→45, saturation 255→180, quadratic brightness ease) remains untouched; v2 only changes scheduling, timing, and the web UI around it.

### `#define` / config block — additions

```
MDNS_HOSTNAME             // "sunrise-light" — bare hostname only, NOT "sunrise-light.local".
                          // MDNS.begin() takes the bare name; ".local" is appended by the
                          // resolver. Baking ".local" into this constant would make the
                          // device advertise itself as "sunrise-light.local.local".
DEFAULT_SUNRISE_MINUTES   // 30 — first-boot fallback only; runtime value now lives in NVS
                          // key "sunriseMinutes" and is what's actually used once set.
DEFAULT_HOLD_MINUTES      // 60 — first-boot fallback for NVS key "holdMinutes" (range 0-120).
```

`WIFI_SSID`/`WIFI_PASSWORD`/`WIFI_CONNECT_TIMEOUT_MS`/`TZ_STRING`/`NTP_SERVER`/`NTP_SYNC_TIMEOUT_MS`/`WEB_SERVER_PORT`/`NUM_LEDS`/`DATA_PIN` are unchanged from v1.

### Globals / state — changes

Removed: `alarmHour`, `alarmMinute`, `alarmDaysMask`, `alarmEnabled` (single-alarm-plus-mask model).

Added:
- `uint8_t dayHour[7]`, `uint8_t dayMinute[7]`, `bool dayEnabled[7]` — per-day schedule, indexed by `tm_wday` (0=Sunday…6=Saturday).
- `uint16_t sunriseMinutes` — runtime sunrise duration, replaces compile-time `SUNRISE_MINUTES` as the actual source of truth.
- `uint8_t holdMinutes` — 0–120, minutes to hold the final color before auto-off.
- `uint32_t holdStartMillis` — timestamp recorded on `RUNNING`→`HOLD`, used to time the auto-off.

Unchanged: `currentStep` (file-scope, not function-static — same reasoning as v1: the scheduler needs to reset it), `currentState`, `lastFiredYday`.

### Preferences (NVS) schema — namespace `"sunrise"`, revised

| key pattern | type | meaning |
|---|---|---|
| `d0h`…`d6h` | uint8 (0–23) | per-day hour, keys built in a loop (`"d" + i + "h"`) rather than 7 hand-written constants |
| `d0m`…`d6m` | uint8 (0–59) | per-day minute |
| `d0e`…`d6e` | bool | per-day enabled |
| `sunriseMinutes` | uint16 | sunrise ramp duration |
| `holdMinutes` | uint8 | 0–120, minutes to hold final color before auto-off |

The old v1 keys (`alarmHour`/`alarmMinute`/`alarmDays`/`alarmEnabled`) become orphaned in NVS — no migration path, since this is a single hobby device still under active development, not a fleet with existing installs to preserve. Worth a one-line code comment noting they're superseded, not a silent gap.

### Functions — changes

- `setup()` — after `connectWiFi()`, add a call to `startMDNS()` (only meaningful if WiFi connected).
- `startMDNS()` — **new**. `MDNS.begin(MDNS_HOSTNAME); MDNS.addService("http", "tcp", WEB_SERVER_PORT);`. No-ops (or is skipped) if WiFi never connected, consistent with the existing fails-safe philosophy.
- `loadAlarmSettings()` / `saveAlarmSettings()` — **shipped as no-argument functions** operating directly on the global schedule/timing state (not the multi-parameter signatures earlier drafts of this plan sketched). `loadAlarmSettings()` loops over the 7 days reading `dayHour[i]`/`dayMinute[i]`/`dayEnabled[i]` plus `sunriseMinutes`/`holdMinutes` from NVS into those globals; `saveAlarmSettings()` writes the same globals back out. `handleSet()` validates and updates the globals first, then calls `saveAlarmSettings()` once to persist everything in a single NVS transaction.
- `updateScheduler()` — `WAITING` check now indexes `dayHour`/`dayMinute`/`dayEnabled` by `now.tm_wday` instead of the old mask/single-alarm fields. Entering `RUNNING` (and retuning the timer) is handled by a new shared `beginSunrise()` helper — see the addendum below. On entry to `HOLD`, `holdStartMillis = millis()` is recorded. `HOLD`'s exit condition changed from "local midnight passed" to "`holdMinutes` have elapsed since `holdStartMillis`."
- `sunrise()` — color math unchanged. **Real implementation constraint that shaped the code:** the step interval was originally computed once via `static const uint16_t interval = (SUNRISE_MINUTES * 60000UL) / totalSteps;` and handed to FastLED's `EVERY_N_MILLISECONDS_I(sunriseTimer, interval)` macro. Because that's `static`, it's only evaluated the *first* call ever — changing `sunriseMinutes` afterward wouldn't retroactively change an already-running timer's period. v2 solves this by declaring `sunriseTimer` as a file-scope `CEveryNMillis` object instead of going through the macro (which creates an unreachable function-local static), and explicitly recomputing `interval` + calling `sunriseTimer.setPeriod(interval)`/`.reset()` from `beginSunrise()` every time a sunrise starts — not relying on the macro's implicit static init like v1 did.
- `handleRoot()` / `handleSet()` — expand to render/accept 7 day rows (enabled checkbox + hour + minute per day) plus `sunriseMinutes` and `holdMinutes` fields. `handleSet()` clamps `holdMinutes` to `[0, 120]` the same way `hour`/`minute` are already clamped today.

### State machine — as shipped

```
WAITING  -- (needs getLocalTime() to succeed; does nothing that tick if not)
         -- dayEnabled[now.tm_wday]
         && now.tm_hour == dayHour[now.tm_wday] && now.tm_min == dayMinute[now.tm_wday]
         && lastFiredYday != now.tm_yday                        --> beginSunrise() --> RUNNING

RUNNING  -- call sunrise() every loop (color math unchanged)
         -- currentStep reaches totalSteps-1                     --> HOLD
            (on entry: holdStartMillis = millis())

HOLD     -- LEDs held at final sunrise color
         -- millis() - holdStartMillis >= holdMinutes * 60000UL  --> WAITING
            (on entry: fill_solid(leds, NUM_LEDS, CRGB::Black))
```

Day-rollover no longer ends `HOLD` — `holdMinutes` does. The `lastFiredYday` guard is unaffected and still independently prevents the same day's alarm from firing twice, even though the board now typically leaves `HOLD` well before midnight.

**`beginSunrise()`** (`currentStep = 0`; if time is available, `lastFiredYday = now.tm_yday`; recompute `interval` from `sunriseMinutes` and call `sunriseTimer.setPeriod(interval)`/`.reset()`; `currentState = RUNNING`) is factored out as its own function rather than inlined in `WAITING`'s on-entry actions — see the addendum below for why.

### Web server routes — revised

- **`GET /`** → `handleRoot()`: adds `sunriseMinutes` and `holdMinutes` number inputs, a standalone **Start Now** button/form, and replaces the single hour/minute/day-checkboxes/enabled block with **7 rows** (Sun…Sat), each with its own enabled checkbox, hour input, and minute input, pre-filled from the current per-day arrays.
- **`POST /set`** → `handleSet()`: reads `sunriseMinutes`, `holdMinutes` (clamped 0–120), and 7× (`dayNHour`, `dayNMinute`, `dayNEnabled`) fields in a loop; saves all; redirects back to `/` (303), same as v1.
- **`POST /start`** → `handleStartNow()`: calls `beginSunrise()`; redirects back to `/` (303). Added alongside the Start Now button — see "Also shipped" below.
- **`onNotFound`** — unchanged.

### Caveats to document in-code

1. ~~v1's "EVERY_N_MILLISECONDS_I timestamp is stale across WAITING→RUNNING" caveat~~ — superseded by the explicit `setPeriod()` call above, which v2 needs anyway to support runtime duration changes. No longer a known issue once that's in place.
2. `lastFiredYday` still isn't persisted (unchanged from v1) — a reboot after today's alarm already fired is still willing to fire it again the same day if the time is reached again.
3. `connectWiFi()` is still a one-shot attempt with no retry/backoff (unchanged from v1). New in v2: `startMDNS()` only runs if that one-shot attempt succeeded, so the `.local` hostname won't come up at all if WiFi never connects — falls back to no access at all in that case, same as v1's IP-based access would.
4. `WebServer.h` synchronous request handling — unchanged from v1, still fine given no filesystem I/O.
5. **New**: `.local` (mDNS/Bonjour) resolution is native on macOS/iOS/Android/Linux but **not guaranteed on Windows** without Bonjour installed (bundled with iTunes, or standalone "Bonjour Print Services") or a recent-enough Windows 10/11 build. Worth confirming from whatever device is actually used day-to-day rather than assuming it'll work.

## Also shipped: Start Now button + fails-safe time fix

Two changes landed after the v2 work above but before v3 was proposed — not originally scoped in this plan, added in response to a direct follow-up request and a bug it exposed. Documented here so this plan stays an accurate build record.

**Start Now button:** the web UI needed a way to trigger a sunrise on demand, independent of the schedule. Rather than duplicate the "enter `RUNNING`" logic, it's factored into the shared `beginSunrise()` function (see above), called from both `updateScheduler()`'s `WAITING` case and a new `POST /start` route (`handleStartNow()`). A "Start Now" button (its own `<form>`, separate from the settings form) was added to `handleRoot()`'s status section.

**Fails-safe time fix:** implementing Start Now surfaced a real bug in the original v2 design — `updateScheduler()` had a single `if (!getLocalTime(&now, 0)) return;` guard at the top of the function, before the state-machine `switch`. That meant if NTP hadn't synced (or ever failed to), the entire scheduler did nothing at all, including advancing an already-`RUNNING` or already-`HOLD`ing sunrise — even though `RUNNING`/`HOLD` are driven by `millis()`, not wall-clock time, and never actually needed `now`. Fixed by moving the `getLocalTime()` call inside the `WAITING` case only, so a sunrise (whether scheduled or manually started) now runs and holds correctly regardless of NTP status; only the schedule-matching check genuinely needs the clock.

## v3: AP-mode + captive portal WiFi provisioning (pending review, not yet implemented)

**Driver:** the device needs to be set up on a new WiFi network by someone with no access to the source code or Arduino IDE (e.g. built and gifted to someone else). Already reviewed with the user:
- Uses the community-standard `WiFiManager` library (tzapu/WiFiManager) rather than hand-rolling AP mode + DNS redirect + HTML forms — the first external dependency beyond FastLED, a deliberate exception to this repo's normal no-new-deps convention.
- Re-provisioning needs no new hardware: `WiFiManager`'s `autoConnect()` already auto-falls-back into setup mode if saved credentials fail to connect on boot; a new "Reset WiFi" button on the web UI covers deliberately switching networks while still connected. (A physical reset button was considered and explicitly not chosen, to avoid a BOM/wiring change.)

### Library

Install via Arduino Library Manager: search "WiFiManager", install the `tzapu/WiFiManager` package. `#include <WiFiManager.h>`.

### `#define` additions

```
SETUP_AP_NAME   // "sunrise-light-setup" - WiFi network name broadcast while in setup mode
```

### Removed

- `#include "wifi_secrets.h"` and the `WIFI_SSID`/`WIFI_PASSWORD` `#define`s (credentials now owned by WiFiManager's own storage, not this sketch).
- `arduino/standalone/SRM_Sunrise_Scheduled/wifi_secrets.h` and `wifi_secrets.h.example` - delete both. (`.gitignore`'s `wifi_secrets.h` entry can stay - harmless once unused - or be cleaned up as a trivial follow-up.)
- `scanAndPrintNetworks()` - redundant once WiFiManager's own portal scans and lists networks; remove unless still wanted for Serial-monitor debugging independent of the portal.

### `connectWiFi()` — rewritten

```cpp
void connectWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(180); // seconds; falls through to offline fails-safe if nobody completes setup
  bool connected = wm.autoConnect(SETUP_AP_NAME);

  if (connected) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi setup portal timed out - continuing without network.");
  }
}
```

Replaces the current fixed-credential `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` + polling loop entirely. `autoConnect()` internally handles: try saved creds → on failure, start AP + captive portal → block until connected or `setConfigPortalTimeout()` elapses. `WIFI_CONNECT_TIMEOUT_MS` (the old polling-loop timeout) becomes unused and can be removed.

### New route: `POST /reset-wifi` → `handleResetWifi()`

```cpp
void handleResetWifi() {
  WiFiManager wm;
  wm.resetSettings();
  server.send(200, "text/plain", "WiFi settings cleared. Restarting...");
  delay(1000);
  ESP.restart();
}
```

Register in `setup()`: `server.on("/reset-wifi", HTTP_POST, handleResetWifi);`. Add a "Reset WiFi" button/form to `handleRoot()`'s status section, next to the existing "Start Now" button.

### Caveats (implementation framing, mirrors the spec)

1. `wm.autoConnect()` blocks `setup()` until connected or timed out — only during first-time setup or right after a "Reset WiFi," never during normal runtime. Everything else in `setup()` (mDNS, NTP sync, loading alarm settings, starting the web server) still happens after this call returns, same relative order as today.
2. Config portal timeout (proposed: 180s) means an incomplete setup falls through to the existing fails-safe offline behavior — board boots, LEDs stay off, scheduler never matches anything. The recipient would need to power-cycle to get the portal to try again.
3. While the setup AP is active, the device isn't reachable at `sunrise-light.local` or its normal IP — only at the setup AP's own address (typically `192.168.4.1`).

### Verification (real hardware) — additions

14. Fresh device (or right after "Reset WiFi"): confirm it broadcasts `sunrise-light-setup`; connecting a phone to it surfaces a setup page listing nearby networks; submitting real credentials reboots the device into normal operation on that network.
15. Move a previously-configured device somewhere its saved network isn't reachable; confirm it automatically falls back into the setup hotspot on boot, with no manual reset needed.
16. From the web UI while connected, use "Reset WiFi"; confirm the device restarts, forgets its network, and re-broadcasts the setup hotspot.
17. Let the config portal time out without completing setup; confirm the board continues booting (LEDs off, no crash/hang) rather than blocking forever.

### Files touched — additions to the existing list

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — `connectWiFi()` rewritten, new `/reset-wifi` route, `wifi_secrets.h` include and `scanAndPrintNetworks()` removed
- **`arduino/standalone/SRM_Sunrise_Scheduled/wifi_secrets.h.example`** — deleted (and the real `wifi_secrets.h`, which was already gitignored)
- **`arduino/standalone/README.md`** — the WiFi setup section needs rewriting to describe the captive-portal flow instead of the "copy `wifi_secrets.h.example`" step

## Verification (real hardware, as run against v1+v2)

Day-mask language replaced by per-day language versus the original v1 checklist, plus v2-specific checks appended. Add Start Now to step 3's "confirm the form works" check.

1. Serial monitor confirms WiFi connects and prints the assigned IP.
2. `/` page shows correct local time after NTP sync (check DST correctness for the configured TZ string against actual wall clock).
3. From a phone on the same LAN, load `http://<device-ip>/`; confirm status fields render and the per-day schedule form works.
4. Set a day's alarm ~2 minutes out on today's day-of-week; confirm sunrise starts at the correct wall-clock time.
5. Confirm a day that's disabled, or a different day-of-week's time, does **not** fire.
6. Power-cycle after setting a schedule; reload `/` and confirm all 7 days' settings, plus `sunriseMinutes`/`holdMinutes`, survived (NVS persistence check).
7. Let a sunrise run to completion; confirm LEDs hold steady at the final warm color with no flicker.
8. Load `/` while `RUNNING`; confirm the page still responds promptly and the LED fade doesn't visibly stutter.
9. **New**: change `sunriseMinutes` via the form while `WAITING`, then trigger a sunrise; confirm the ramp actually takes the new duration, not the old one — this is the real test of the `setPeriod()` fix.
10. **New**: set `holdMinutes` to `0`; let a sunrise complete; confirm LEDs go black immediately with no hold.
11. **New**: set `holdMinutes` to something short (e.g. 2) for a fast test; let a sunrise complete; confirm LEDs hold for ~2 minutes then go black, without needing to wait for or reach midnight.
12. **New**: set distinct schedules across multiple days (e.g. two different times on different days, one day left disabled); confirm each enabled day fires independently at its own time and the disabled day never fires.
13. **New**: from a device known to support mDNS (phone or Mac), load `http://sunrise-light.local/` and confirm it resolves and loads the same page as the IP does. If testing from Windows, confirm whether it resolves out of the box or needs Bonjour.

## Files touched (v1 + v2, already done)

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — implemented and pushed
- **`docs/Arduino_Hardware.md`** — v1's continuous-power note still applies unchanged
- **`CLAUDE.md`** — the sketch summary there doesn't enumerate individual fields, so v2 didn't require changes
- **`_spec/wifi-scheduled-sunrise.md`** — kept in sync, now describes v1+v2 as implemented and v3 as proposed
