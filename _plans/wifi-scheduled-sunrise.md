# WiFi-Scheduled Sunrise (ESP32)

## Context

The sunrise clock currently has no way to schedule itself: a smart outlet cuts power to the ESP32 on an external schedule (SmartThings + Zigbee outlet), and the sketch just runs its 512-step color progression once on boot. The ESP32 board (`SRM_Sunrise_ESP32.ino`) was verified working on real ESP32 hardware — LED array confirmed good.

Decisions made building v1:
- **WiFi only, no BLE** — WiFi gets free NTP time sync and a phone-browser-reachable web page with no companion app; BLE needs an actively-paired phone and no wall-clock reference of its own.
- **Time source: WiFi + NTP only, no RTC module** — despite having DS3231 libraries from unrelated past projects, the user chose not to add RTC hardware for this. The board must sync time from the internet at boot.
- **The board must stay powered continuously** (wall adapter, not the smart outlet) so it can keep WiFi/NTP time and watch for its own alarm trigger. The outlet-cuts-power trigger model is retired for this variant.

## Status

**v1 through v7 are all implemented.** This whole document is a build record, not a to-do list. **v1-v5 are pushed; v6 and v7 are implemented locally but not yet pushed.** v3 (AP-mode + captive portal WiFi provisioning) has not yet been verified on real hardware — see the "Verification (real hardware) — additions" checklist under the v3 section, items 14-17. v4 shipped a web-editable timezone using a client-side JS heuristic to guess a POSIX TZ string; it was superseded almost immediately by v5, which replaced that heuristic with the ezTime library for an authoritative (not guessed) resolution. v4's section below is kept as a build record of what was tried and why it was replaced, not as current behavior — see v5 for what's actually running. v6 (OTA firmware updates via the web UI) was verified once indirectly through WiFiManager's own built-in `/update` page, then reimplemented against this sketch's own always-on server - that reimplementation hasn't been hardware-tested yet. v7 is a visual/UX redesign of the settings page on top of v6 - it has compiled and rendered on real hardware (confirmed via a phone screenshot), and went through a round of layout refinement based on that screenshot (see "Refinements after first hardware render" in the v7 section) - not yet re-tested after those refinements, and OTA (v6) itself still untested.

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

## v3: AP-mode + captive portal WiFi provisioning (implemented, pushed; hardware verification pending)

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

- `#include "wifi_secrets.h"` and the `WIFI_SSID`/`WIFI_PASSWORD` `#define`s (credentials now owned by WiFiManager's own storage, not this sketch). Done.
- `arduino/standalone/SRM_Sunrise_Scheduled/wifi_secrets.h` and `wifi_secrets.h.example` - both deleted (`wifi_secrets.h.example` via `git rm`, `wifi_secrets.h` was already gitignored). `.gitignore`'s `wifi_secrets.h` entry was left in place - harmless now that it's unused.
- `scanAndPrintNetworks()` - removed; redundant once WiFiManager's own portal scans and lists networks.
- `WIFI_CONNECT_TIMEOUT_MS` (the old polling-loop timeout) - removed, replaced by `WIFI_CONFIG_PORTAL_TIMEOUT_S`.

### `connectWiFi()` — as shipped

```cpp
void connectWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S); // 180s; falls through to offline fails-safe if nobody completes setup
  bool connected = wm.autoConnect(SETUP_AP_NAME);

  if (connected) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("WiFi setup portal timed out - continuing without network.");
  }
}
```

Replaced the old fixed-credential `WiFi.begin(WIFI_SSID, WIFI_PASSWORD)` + polling loop entirely. `autoConnect()` internally handles: try saved creds → on failure, start AP + captive portal → block until connected or `setConfigPortalTimeout()` elapses.

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

Registered in `setup()`: `server.on("/reset-wifi", HTTP_POST, handleResetWifi);`. A "Reset WiFi" button/form was added to `handleRoot()`'s status section, next to the existing "Start Now" button, with a JS `confirm()` prompt since it forces an immediate reboot into setup mode.

### Caveats (implementation framing, mirrors the spec)

1. `wm.autoConnect()` blocks `setup()` until connected or timed out — only during first-time setup or right after a "Reset WiFi," never during normal runtime. Everything else in `setup()` (mDNS, NTP sync, loading alarm settings, starting the web server) still happens after this call returns, same relative order as today.
2. **Open question resolved:** `WIFI_CONFIG_PORTAL_TIMEOUT_S` is set to 180 (3 minutes) rather than staying open indefinitely. Reasoning: with no physical reset button, an indefinitely-open portal after a stray "Reset WiFi" tap (with nobody around to finish setup) strands the device just as hard as a timeout would - either way a power-cycle is the recovery path - so the bounded timeout was chosen to stay consistent with the sketch's existing fails-safe philosophy (continue booting rather than hang forever). An incomplete setup falls through to the existing fails-safe offline behavior — board boots, LEDs stay off, scheduler never matches anything. The recipient would need to power-cycle to get the portal to try again.
3. While the setup AP is active, the device isn't reachable at `sunrise-light.local` or its normal IP — only at the setup AP's own address (typically `192.168.4.1`).

### Verification (real hardware) — additions, not yet run

14. Fresh device (or right after "Reset WiFi"): confirm it broadcasts `sunrise-light-setup`; connecting a phone to it surfaces a setup page listing nearby networks; submitting real credentials reboots the device into normal operation on that network.
15. Move a previously-configured device somewhere its saved network isn't reachable; confirm it automatically falls back into the setup hotspot on boot, with no manual reset needed.
16. From the web UI while connected, use "Reset WiFi"; confirm the device restarts, forgets its network, and re-broadcasts the setup hotspot.
17. Let the config portal time out without completing setup; confirm the board continues booting (LEDs off, no crash/hang) rather than blocking forever.

### Files touched — v3, done

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — `connectWiFi()` rewritten, new `/reset-wifi` route + button, `wifi_secrets.h` include and `scanAndPrintNetworks()` removed
- **`arduino/standalone/SRM_Sunrise_Scheduled/wifi_secrets.h.example`** — deleted (and the real `wifi_secrets.h`, which was already gitignored)
- **`arduino/standalone/README.md`** — WiFi setup section rewritten to describe the captive-portal flow instead of the "copy `wifi_secrets.h.example`" step
- **`_spec/wifi-scheduled-sunrise.md`** — v3 section moved out of "proposed" framing into the main implemented-behavior description

## v4: Web-editable, browser-auto-detected timezone (superseded by v5 - kept as build record)

**Driver:** the POSIX TZ string was still a compile-time `#define`, requiring source access and a reflash to change — the same problem v3 just solved for WiFi credentials. Requested directly: "I want to be able to set the timezone in the HTTP setup environment, and if possible set it automatically based on the device setting it up."

**Approach chosen over alternatives considered:**
- The ESP32 has no IANA timezone database, and embedding one (even client-side) would be a large asset for a memory-constrained device serving the page - ruled out.
- Instead, detection happens entirely in the browser: JavaScript embedded in the served HTML reads the visiting device's own `Date` behavior (which the browser/OS already knows correctly, DST rules included) and derives a POSIX TZ string from it. The ESP32 never needs to understand IANA names at all - it only ever stores/uses the POSIX string, exactly as it always has.
- The detect button fills the field but does not auto-submit - consistent with every other form field on this page, and lets an obviously-wrong guess be corrected by hand before saving.

### `#define` change

`TZ_STRING` renamed to `DEFAULT_TZ_STRING` - same first-boot-fallback-only pattern as `DEFAULT_SUNRISE_MINUTES`/`DEFAULT_HOLD_MINUTES`.

### New global

`String tzString = DEFAULT_TZ_STRING;` - runtime value, NVS-persisted (key `"tzString"`, namespace `"sunrise"`) via the existing `loadAlarmSettings()`/`saveAlarmSettings()` pair.

### `setup()` reordering

`loadAlarmSettings()` now runs *before* `syncTime()` (previously after) since `syncTime()`'s `configTzTime()` call needs `tzString` already loaded from NVS, not still at its compile-time default.

### `handleSet()` — timezone handling

Reads `tzString` from the form, trims it, and only accepts it if non-empty and ≤60 characters (defensive cap against a malformed/huge submission; a real POSIX TZ string is well under this) - otherwise silently keeps the previous value. After `saveAlarmSettings()`, calls `configTzTime(tzString.c_str(), NTP_SERVER)` again immediately, so the new zone is reflected in `getLocalTime()`/`strftime()` right away rather than requiring a reboot. This is safe to call without WiFi - it just resets the C library's TZ environment variable (`tzset()` under the hood); no network round-trip needed unlike the very first sync in `syncTime()`.

### `handleRoot()` — Timezone field + detect button

Added to the Timing section: a text input (`id`/`name='tzString'`, pre-filled with the current value) and a `type='button'` (not `submit`) **"Detect from this device"** button, plus a short explanation. A `<script>` block embedded at the end of the page body defines the detection logic:

```js
function pad(n) { return n < 10 ? '0' + n : '' + n; }
function fmtOffset(min) {
  var sign = min < 0 ? '-' : '';
  var abs = Math.abs(min);
  var h = Math.floor(abs / 60);
  var m = abs % 60;
  return sign + h + (m ? ':' + pad(m) : '');
}
function posixRule(date) {
  var month = date.getMonth() + 1;
  var day = date.getDate();
  var weekday = date.getDay();
  var daysInMonth = new Date(date.getFullYear(), month, 0).getDate();
  var n = (day + 7 > daysInMonth) ? 5 : Math.ceil(day / 7);
  return 'M' + month + '.' + n + '.' + weekday;
}
function detectTimezone() {
  var year = new Date().getFullYear();
  var offsets = [];
  for (var m = 0; m < 12; m++) offsets.push(new Date(year, m, 1).getTimezoneOffset());
  var stdOffset = Math.max.apply(null, offsets); // larger = more "west" = standard/winter time
  var dstOffset = Math.min.apply(null, offsets);
  var tz;
  if (stdOffset === dstOffset) {
    tz = 'STD' + fmtOffset(stdOffset);
  } else {
    // Scan day-by-day for the two transition instants. Doesn't assume
    // calendar order, so it's correct for both hemispheres (e.g. New
    // Zealand's DST-start-in-September comes before DST-end-in-April).
    var prev = new Date(year, 0, 1).getTimezoneOffset();
    var dstStart = null, dstEnd = null;
    var d = new Date(year, 0, 1);
    while (d.getFullYear() === year) {
      var cur = d.getTimezoneOffset();
      if (cur !== prev) {
        if (cur < prev) dstStart = new Date(d); // clocks sprang forward -> DST began
        else dstEnd = new Date(d);               // clocks fell back -> DST ended
        prev = cur;
      }
      d.setDate(d.getDate() + 1);
    }
    tz = (dstStart && dstEnd)
      ? ('STD' + fmtOffset(stdOffset) + 'DST,' + posixRule(dstStart) + ',' + posixRule(dstEnd))
      : ('STD' + fmtOffset(stdOffset));
  }
  document.getElementById('tzString').value = tz;
}
```

(Shipped as minified single-line `html +=` statements to match this file's existing string-building style, not formatted like the above - shown formatted here for readability.)

### Known limitations of the detection heuristic

- Assumes the POSIX default DST transition time (02:00 local) and a 1-hour DST shift. Wrong for the small number of zones that differ (e.g. Lord Howe Island's 30-minute DST, or zones whose transition happens at a different local hour) - the field stays hand-editable for those, same as it always was.
- Uses placeholder `STD`/`DST` three-letter abbreviations rather than the zone's real abbreviation (e.g. `PST`/`PDT`) - these only affect the cosmetic `%Z` output in `strftime()`, not the actual offset/DST math, so this is harmless.
- No IANA name is ever seen or stored anywhere - by design, per the "no timezone database on the ESP32" decision above.

### Files touched (v4, superseded - see v5 below for what's current)

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — `TZ_STRING` → `DEFAULT_TZ_STRING`, new `tzString` global, `setup()` reordered, `loadAlarmSettings()`/`saveAlarmSettings()`/`handleSet()`/`handleRoot()` updated
- **`_spec/wifi-scheduled-sunrise.md`** — new "Timezone: web-editable, browser-auto-detectable" section, Configuration table, NVS table, and web-interface section all updated

## v5: Timezone via ezTime library (implemented, pushed; hardware verification pending)

**Driver:** asked directly, right after v4 shipped: "could using the ezTime library help with not having to manually enter a timezone? ... maybe having a dropdown list of zones to set to?" (https://github.com/ropg/ezTime). Discussed trade-offs before implementing (new dependency + new third-party lookup service + a real refactor of this sketch's time-handling code, versus authoritative DST correctness for every zone instead of a client-side heuristic) - user chose to switch.

**What changed vs. v4:** v4's client-side JS guessed a POSIX TZ string from the browser's own DST behavior (documented above) - correct for most zones but not all (e.g. the 30-minute-DST Lord Howe Island case), and stored a raw POSIX string the ESP32 had to already understand. v5 instead stores a plain tz-database *location name* (e.g. `"America/Los_Angeles"`) and lets the **ezTime** library resolve it authoritatively, via a lookup service the library author runs (`timezoned.rop.nl`) - the same kind of data a real `/etc/zoneinfo` would give you, not a guess. This also happened to simplify the client-side detection: instead of ~30 lines of JS scanning for DST transitions, "detect" is now a single call to the standard `Intl.DateTimeFormat().resolvedOptions().timeZone`, which returns the browser's own correct IANA zone name directly.

**Confirmed library API before writing any code** (fetched `ropg/ezTime`'s README and header from GitHub rather than guessing, since a wrong function signature only surfaces as a compile error on real hardware):
- `Timezone::setLocation(String location)` — resolves a location name via the lookup service; returns `false` on failure (unknown location, no network, server error) — check via `errorString()`.
- `Timezone::setPosix(String posix)` — exists for offline/raw use, not used here since location-name resolution is the whole point.
- `Timezone::setCache(String nvs_name, String key)` (ESP32 overload) — NVS-backed fallback cache; if a later `setLocation()` lookup fails, ezTime falls back to the last successful resolution instead of losing the correct time.
- `waitForSync(uint16_t timeout)` (seconds, not ms) / `events()` (must be called every `loop()` iteration - new addition) / `timeStatus()` returning `timeNotSet`/`timeNeedsSync`/`timeSet`.
- `Timezone::dateTime(String format)` — PHP-`date()`-style format codes.
- `Timezone::dayOfYear()` — direct replacement for the old `tm_yday`-based `lastFiredYday` guard (exact 0-vs-1-indexing doesn't matter, since it's only ever compared for equality/inequality against its own previous value).
- `Timezone::hour()`/`minute()` — direct replacements for `tm_hour`/`tm_min`.

### `#include` changes

`<time.h>` removed; `<ezTime.h>` added. `WiFi.h` still needed directly (ezTime uses it internally but doesn't hide it).

### Cache: not used, after two build-error rounds

The plan originally called `myTZ.setCache("eztime", "tzc")` in `syncTime()` for offline resilience (fall back to the last resolution if a lookup ever fails). This went through two real build failures before landing on "don't use it":
1. **Compile error**: `no matching function for call to 'Timezone::setCache(const char [7], const char [4])'`. The installed ezTime build defaults its cache to EEPROM (`Timezone::setCache(int16_t)`); the NVS-based two-string overload only exists in the header when `EZTIME_CACHE_NVS` is defined before `#include <ezTime.h>`. Fixed by adding that `#define` in the sketch, confirmed against the installed library's actual `ezTime.h` (not just the GitHub README, which didn't reflect this installed version's default cache choice).
2. **Link error**: `undefined reference to 'Timezone::setCache(String, String)'`. The declaration now resolved (fixed #1), but the *implementation* lives in the library's own `ezTime.cpp`, compiled as a separate translation unit that includes `ezTime.h` independently - it never sees the sketch's `#define`, so it was still only ever compiled with the EEPROM cache path. This library only exposes the NVS/EEPROM choice by editing the installed library file directly (per its own header comment: "Cache mechanism, either EEPROM or NVS, not both. (See README)") - not something this repo's setup instructions can portably ask every future builder to do by hand.
- **Resolution**: dropped `setCache()` entirely rather than chase a working cache backend. `syncTime()` just calls `setLocation()`/`waitForSync()` with no cache configured - a lookup failure means no time is available until the next successful sync, the same fails-safe behavior every other network-dependent step in this sketch already has. Losing the offline-resilience nice-to-have was judged not worth either an EEPROM dependency (extra setup, a second storage mechanism alongside this sketch's own NVS `Preferences` use) or a non-reproducible local library edit.

### `#define` changes

`DEFAULT_TZ_STRING` (POSIX string) → `DEFAULT_TZ_LOCATION` (`"America/Los_Angeles"`, a tz-database location name). `NTP_SYNC_TIMEOUT_MS` (15000) → `NTP_SYNC_TIMEOUT_S` (15) since `waitForSync()`'s timeout parameter is in seconds. `NTP_SERVER` removed entirely - unused now that ezTime manages its own default NTP source.

### Globals

`Timezone myTZ;` (new) replaces all direct `time.h` calls. `String tzLocation = DEFAULT_TZ_LOCATION;` replaces `tzString`, same NVS-persisted pattern (new key `"tzLocation"` - `"tzString"` becomes an orphaned NVS key, no migration, consistent with this sketch's existing precedent for superseded keys). `lastFiredYday` widened from `int16_t` to `int32_t` to match `dayOfYear()`'s `uint16_t` return type cleanly.

### `syncTime()` — rewritten

```cpp
void syncTime() {
  if (WiFi.status() != WL_CONNECTED) return;

  myTZ.setCache("eztime", "tzc"); // separate NVS namespace from this sketch's own "sunrise" prefs
  if (!myTZ.setLocation(tzLocation)) {
    Serial.print("Timezone lookup failed for \"");
    Serial.print(tzLocation);
    Serial.print("\": ");
    Serial.println(errorString());
  }

  if (waitForSync(NTP_SYNC_TIMEOUT_S)) {
    Serial.print("Time synced: ");
    Serial.println(myTZ.dateTime("Y-m-d H:i:s T"));
  } else {
    Serial.println("NTP sync timed out - scheduler will wait until time is available.");
  }
}
```

`loop()` gained `events();` as its first line - required by ezTime for background NTP housekeeping, easy to miss since nothing fails loudly if it's omitted (time just silently never re-syncs).

### `updateScheduler()`/`beginSunrise()` — rewritten

`WAITING` case: `if (timeStatus() != timeSet) break;` replaces the old `getLocalTime()` guard; `wday`/`timeMatches`/`notAlreadyFiredToday` now read from `myTZ.dateTime("w").toInt()`/`myTZ.hour()`/`myTZ.minute()`/`myTZ.dayOfYear()` instead of a `struct tm`. `beginSunrise()`: `if (timeStatus() == timeSet) lastFiredYday = myTZ.dayOfYear();` replaces the old `getLocalTime()`-gated assignment. `RUNNING`/`HOLD` are untouched - they never depended on wall-clock time.

### Real bug found on hardware: weekday off-by-one

Reported directly after the first successful compile: setting an alarm a few minutes out and waiting past it left the board stuck at "Waiting (0%)" - it never fired. Root cause, found by reading the installed `ezTime.cpp` (not the GitHub docs, which had already proven unreliable for this installed version once, over `setCache`):

- `dateTime("w")`'s own in-source comment claims `0 = Sunday`, and the earlier v5 write-up (above) trusted that.
- But `ezTime.cpp`'s `breakTime()` sets `tm.Wday = ((time + 4) % 7) + 1;  // Sunday is day 1` - i.e. **1=Sunday...7=Saturday** - and the `case 'w':` format handler just outputs `tm.Wday` raw, with no adjustment back to the 0-indexed value its own comment promises. `weekday()` returns the exact same unconverted value. Both are wrong relative to their documented contract in this installed version.
- Effect: `int wday = myTZ.dateTime("w").toInt();` was always one day ahead of `dayHour[]`/`dayMinute[]`/`dayEnabled[]`'s actual 0=Sunday indexing - every day's alarm was being checked against the *next* day's slot (which was usually disabled/different, hence never matching), and on Saturday (`wday` = 7) it indexed one past the end of each 7-element array entirely.
- **Fix**: `int wday = myTZ.dateTime("w").toInt() - 1;` - one-line change in `updateScheduler()`.

### `handleRoot()` — timezone field, preset dropdown, simplified detect

- Current-time display: `haveTime ? myTZ.dateTime("Y-m-d H:i:s T") : "not synced yet"` replaces the old `strftime()` call.
- New small hardcoded `TzPreset { name, label }` array (~14 common zones: US/Canada regions, UK, Central Europe, Australia Eastern, Japan, India, UTC) renders as a `<select>` next to the text field - purely a convenience shortcut, selecting an option just fills the text input via inline `onchange`; it does not add server-side handling of its own.
- Detect button's JS shrank to one line: `document.getElementById('tzLocation').value = Intl.DateTimeFormat().resolvedOptions().timeZone;` - replaces v4's ~20-line DST-scanning heuristic entirely.

### `handleSet()` — change-gated re-resolution

Only calls `myTZ.setLocation()` again if the submitted `tzLocation` actually differs from the current value (`tzChanged` flag) **and** WiFi is connected. Two reasons this differs from v4's unconditional `configTzTime()` call: (1) `setLocation()` is a real network round-trip (unlike v4's local-only `tzset()`), so gating on WiFi is necessary, not just tidy; (2) ezTime's own docs ask for ≥3 seconds between `setLocation()` calls, and re-triggering a lookup on every unrelated settings save (e.g. just editing a day's alarm time) would risk hitting that rate limit for no reason.

### Known limitations / caveats

- Second external dependency in this repo (after WiFiManager) - both are explicit, deliberate exceptions to the normal FastLED-only convention, justified the same way: no reasonable way to build the feature (captive portal / authoritative TZ resolution) from the ESP32 core alone.
- Depends on a third-party lookup service (`timezoned.rop.nl`) beyond NTP itself, with no cache fallback (see above) - if that service or WiFi is unreachable during `syncTime()`, the board gets no time that boot, same as an NTP failure always meant in v1-v4.
- `setLocation()` calls need to stay >3s apart per the library's own docs - `handleSet()`'s change-gating (above) is what keeps normal usage safely under that.

### Files touched

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — `<time.h>` → `<ezTime.h>`, `DEFAULT_TZ_STRING`/`tzString` → `DEFAULT_TZ_LOCATION`/`tzLocation`, new `Timezone myTZ` global, `events()` added to `loop()`, `syncTime()`/`beginSunrise()`/`updateScheduler()`/`handleRoot()`/`handleSet()` all updated
- **`arduino/standalone/README.md`** — WiFi-Scheduled Sunrise section: added ezTime to the libraries-to-install step, updated the "What it adds" bullet and the timezone step in Setup steps
- **`_spec/wifi-scheduled-sunrise.md`** — "Timezone" section rewritten for the ezTime approach, Requirements/Configuration/NVS/web-interface sections updated, state-machine section's weekday/hour/minute/dayOfYear references updated

### Verification (real hardware) — additions, not yet run

23. Load `/`, click "Detect from this device"; confirm the Timezone field fills with your actual IANA zone name (e.g. `"America/Los_Angeles"`), not a POSIX string.
24. Save with a freshly-detected location; confirm `/`'s "Current time" updates to the correct local time (check the `%T`-equivalent abbreviation looks right too, e.g. `PST`/`PDT`).
25. Pick a different zone from the preset dropdown and save; confirm the displayed time shifts accordingly.
26. Submit the settings form with the Timezone field unchanged; confirm no "Timezone lookup failed" line appears in Serial Monitor (proves the change-gating in `handleSet()` is working, not re-resolving on every save).
27. Power-cycle after saving a timezone; reload `/` and confirm the timezone (and correct local time after resync) survived reboot.
28. Disconnect WiFi (or block internet access) and power-cycle; confirm the board boots cleanly with "not synced yet" showing (no cache to fall back to - expected, not a bug) rather than crashing or hanging.
29. Type a deliberately invalid location name (e.g. `"Not/A_Zone"`) and save; confirm Serial Monitor logs a "Timezone lookup failed" line with a real error string, and the board doesn't crash or hang.

## v6: OTA firmware updates via the web UI (implemented, pushed; hardware verification pending)

**Driver:** asked directly - "the WifiManager has an option to update firmware. Does this work with arduino apps? Does it just use the ino file or does it need to be packaged differently?" Explained: WiFiManager's built-in `/update` page (`handleUpdate()`/`handleUpdating()` in the installed library's `WiFiManager.cpp`, confirmed by reading the source) uses the ESP32 core's `Update` library to flash a pre-compiled `.bin` (Arduino IDE's **Sketch → Export Compiled Binary** - there's no on-device compiler, so the raw `.ino` never gets uploaded anywhere). User exported the binary and **verified this works** using WiFiManager's existing page - but that page only exists while `connectWiFi()`'s function-local `WiFiManager wm` object is alive, i.e. only during the transient captive-portal window (first boot / right after "Reset WiFi"), not during normal operation. Asked to make it reachable from the sketch's own always-on server instead.

### Why not just keep using WiFiManager's page

Two ways to make WiFiManager's own `/update` reachable outside the transient portal window were considered and rejected in the earlier discussion:
- `wm.startWebPortal()` to keep WiFiManager's internal server alive in STA mode - but that server also wants port 80, conflicting with this sketch's own `WebServer server(WEB_SERVER_PORT)` already bound there.
- Triggering "Reset WiFi" every time just to get a firmware-update window - technically works (that's how it was verified) but wipes WiFi credentials every time, not a usable repeatable workflow.

Simplest fix: reimplement the same upload-to-`Update`-library pattern directly on this sketch's own server, which is already always running.

### `#include` addition

`#include <Update.h>` - ships with the ESP32 Arduino core (no library install), same as `WebServer.h`/`Preferences.h`/`ESPmDNS.h` above it.

### New routes

- **`GET /update`** (`handleUpdatePage()`) - plain HTML form, `<input type='file' name='update'>` posting multipart data to `/update`, plus a link back to `/`.
- **`POST /update`** - registered as `server.on("/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);`, using `WebServer::on()`'s 4-argument overload (`RequestHandler &on(const Uri &uri, HTTPMethod method, THandlerFunction fn, THandlerFunction ufn)` - confirmed against the installed ESP32 core's `WebServer.h`, comment there literally says "ufn handles file uploads"):
  - `handleFirmwareUpload()` is the `ufn` - called repeatedly as upload chunks arrive (`server.upload()`, `HTTPUpload::status` cycling through `UPLOAD_FILE_START` → `UPLOAD_FILE_WRITE` (repeated) → `UPLOAD_FILE_END`), streaming each chunk into `Update.write()` rather than buffering the whole file in RAM. Mirrors WiFiManager's own `handleUpdating()` almost line-for-line - same library, same pattern, just wired into a different server instance.
  - `handleFirmwareUpdate()` is the `fn` - runs once after the body is fully received, checks `Update.hasError()`, sends a plain-text success/failure response, then `ESP.restart()`s either way after a 1s delay (a failed partial flash shouldn't be left half-applied).
- A **Firmware Update** link (`<a href='/update'>`) added to `handleRoot()`'s button row, next to Start Now / Reset WiFi.

### API verified against the installed ESP32 core before writing code

Given the ezTime `setCache` surprise earlier in this project, checked the actually-installed library sources rather than trusting memory:
- `WebServer.h` (`.../packages/esp32/hardware/esp32/3.3.11/libraries/WebServer/src/WebServer.h`): confirmed the 4-arg `on()` overload exists and its parameter order (handler, then upload-handler).
- `Update.h` (`.../packages/esp32/hardware/esp32/3.3.11/libraries/Update/src/Update.h`): confirmed `UPDATE_SIZE_UNKNOWN`, `begin(size)`, `write(data, len)`, `end(evenIfRemaining)`, `printError(Print&)`, `hasError()` all exist with the signatures used.

### Known limitations / caveats

- **No authentication** on `/update` - same as every other route in this sketch (`/start`, `/set`, `/reset-wifi` are equally open). Anyone on the local network can push firmware. Consistent with this sketch's existing security posture, not a new gap introduced here, but worth having in the record given the "gift to a non-technical user on their own network" scenario v3 was designed around.
- Requires an OTA-capable partition scheme (two app slots) - the default "Default" partition scheme for ESP32 Dev Module already provides this; no sketch-level change needed, but a from-scratch board with a custom minimal partition table could lack it.
- `Update.begin(UPDATE_SIZE_UNKNOWN)` doesn't know the final size up front (the upload's `Content-Length` isn't threaded through to it here) - matches WiFiManager's own approach, and works fine, just can't pre-validate the image will fit before starting to write.

### Files touched

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — `#include <Update.h>` added, `GET /update`/`POST /update` routes registered in `setup()`, three new functions (`handleUpdatePage()`, `handleFirmwareUpload()`, `handleFirmwareUpdate()`), Firmware Update link added to `handleRoot()`. Also fixed a stale comment on the `Timezone myTZ` global left over from the v5 cache-removal fix (claimed a cache was kept when it isn't).
- **`_spec/wifi-scheduled-sunrise.md`** — new "Firmware updates via the web UI" section, `GET /update`/`POST /update` added to the Web interface section, verification pointer updated

### Verification (real hardware) — additions, not yet run

30. From `/`, click **Firmware Update**; confirm the upload form page loads.
31. Export a compiled `.bin` (Sketch → Export Compiled Binary) and upload it via that form while the board is on its normal network (not the captive portal); confirm it flashes and reboots successfully, and that WiFi/schedule settings survived (they live in NVS, untouched by an app-partition flash).
32. Confirm Serial Monitor shows the "Firmware update starting"/"complete" log lines and no `Update.printError()` output during a successful update.
33. Try uploading something that isn't a valid firmware image (e.g. a random file) and confirm the board reports failure and reboots cleanly rather than bricking.

## v7: Settings page visual redesign (implemented, compiled, rendered on hardware; refined after screenshot feedback, re-test pending)

**Driver:** a phone screenshot of the plain-HTML v6 page ("the look and feel on a phone aren't great") plus a direct ask: break functions into sections, move admin actions (Start Now/Reset WiFi/Firmware Update) to the bottom, make it look like a modern settings app rather than "a cheap web page", rename "Start Now". Brainstormed three visual directions first (iOS-style grouped list / warm dark sunrise dashboard / minimal whitespace) with ASCII-mockup previews before writing any code - user picked:
1. **iOS-style grouped list** as the visual direction.
2. **"Test Run Sunrise"** as the new button label (from a few options offered).
3. **Native `<input type="time">`** per day instead of separate hour/minute number boxes.

### Visual design (all inline, no external CSS/fonts/CDN - the page must render standalone from the ESP32)

- Light gray page background (`#f2f2f7`), white rounded `.card` sections (10px radius, subtle shadow), uppercase gray `.section-label`s above each card, `-apple-system`/`Segoe UI`/Roboto system font stack.
- Single warm accent color (`#ff9500`, an amber/orange) used for the Save button, active toggle state, and the "Running" state pill - a deliberate nod to the product itself (a sunrise clock) rather than a neutral iOS blue.
- Hero card at the top: current date/time, a colored state pill (`waiting`=gray `#8e8e93`, `running`=amber `#ff9500` with live percentage appended, `hold`=green `#34c759`), WiFi IP/mDNS address, and `FIRMWARE_VERSION` (new `#define`, bumped manually to confirm an OTA update took effect).
- Pure-CSS toggle switches for each day's enabled state (`.toggle` / `.track` / `::before` with a `:checked + .track` sibling-selector transform) - purely visual, the underlying element is still a normal `<input type=checkbox>` so `server.hasArg()` parsing is completely unchanged.
- `.row-btn` (`all: unset` reset, then re-styled as a full-width flex row) makes the Device section's `<form><button></button></form>` elements look like plain list rows with a trailing `›` chevron, matching the `<a href='/update'>` Firmware Update row styled the same way via `.row-link`.

### Structural change: `handleSet()` now parses `<input type="time">`

Each day's `dNh`/`dNm` number fields became one `dNt` field. `<input type="time">` always submits `"HH:MM"` in 24-hour zero-padded form regardless of the browser's display locale/format, so `handleSet()` now does `t.indexOf(':')` + two `substring()`/`toInt()` calls instead of reading two separate fields - falls back to the existing `dayHour[i]`/`dayMinute[i]` value if the field is missing or malformed (same defensive-parsing spirit as every other field in this handler) rather than zeroing it out. The underlying storage (`dayHour[]`/`dayMinute[]` as separate `uint8_t` arrays, NVS keys `dNh`/`dNm`) is **unchanged** - only the HTTP field format changed, not the data model.

### Refinements after first hardware render

The initial version above compiled and rendered correctly (confirmed via a phone screenshot showing real device data - schedule, timing, timezone, all populated), but the screenshot surfaced three follow-up requests:

**1. Caption/divider placement bug.** The screenshot showed "hard to tell what the text is referring to" for the Timing fields (and, on inspection, the same problem existed in the Device section). Root cause: `.row` (and originally `.row-btn`/`.row-link`) each carried their own `border-bottom`, with only the *last* such element in a card losing it via `:last-child`. Where a field's `.explain` caption came right after its `.row` as a separate sibling, the row was never actually the last child (the explain div was), so the row kept its border - meaning the divider landed **between a field and its own caption**, not between the caption and the next field. That reads exactly backwards: the caption visually attaches to the wrong neighbor.

   Fix: moved the border to a new `.item` wrapper div that contains a field's `.row` *and* its `.explain` together as one unit, with `.item:last-child` losing the border instead. Applied consistently to Schedule (each day, no caption but wrapped anyway for consistency), Timing, Timezone, and Device (each admin action + its caption, where present). Also gave `.explain` a small `margin-top:-6px` to pull it visually snug against its own field, while keeping its own bottom padding (14px) so the gap before the *next* item (caption padding + border + next item's own top padding) reads clearly larger - tight coupling to what it explains, clear separation from what it doesn't.

**2. Missing captions for Device actions.** Asked directly: explain what "Test Run Sunrise" and "Reset WiFi" actually do, since neither is fully self-explanatory from the label alone (deliberately not extended to "Firmware Update", which wasn't asked for and is reasonably self-explanatory). Added `.explain` captions matching the existing tone used elsewhere on the page:
   - Test Run Sunrise: "Runs the full sunrise sequence right now, using the current duration setting - handy for testing without waiting for the scheduled time. Doesn't change the schedule."
   - Reset WiFi: "Forgets the saved WiFi network and restarts, broadcasting the sunrise-light-setup hotspot again so it can join a different network. Schedule and other settings aren't affected."

**3. Desktop width.** Asked: "is there a way to limit the width of the page so it looks ok on a pc and a phone?" The page had no `max-width`, so on a wide desktop browser window the cards/rows would stretch edge-to-edge across the screen. Added `max-width:480px;margin:0 auto;` to `body`. On a phone (viewport already narrower than 480px) this has zero effect - unchanged from before. On a wider window, content caps at a phone-ish column and centers itself instead of stretching.

### Files touched

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — `FIRMWARE_VERSION` `#define` added; `handleRoot()` fully rewritten (inline `<style>` block, restructured markup: hero card, Schedule/Timing/Timezone cards inside the `/set` form, Device card below it); new `zeroPad2()` helper; `handleSet()`'s day-parsing loop rewritten for the `dNt` field; plus the three refinements above (`.item` wrapper restructuring, Device captions, `body` max-width).
- **`_spec/wifi-scheduled-sunrise.md`** — `GET /`/`POST /set` sections in "Web interface" rewritten to describe the new layout and field format; "Start Now" renamed to "Test Run Sunrise" everywhere it's referenced (Purpose, WiFi-provisioning caveats, fails-safe behavior, verification pointer); Device-card description updated to mention the explanatory captions.

### Verification (real hardware) — additions, not yet run

34. Load `/` on a phone; confirm the page renders as grouped cards (not a plain unstyled form) and is comfortably usable without pinch-zooming.
35. Tap a day's time field; confirm the phone's native time-picker UI appears (not a plain text/number keyboard).
36. Toggle a day off/on via the switch, change a time, hit Save; confirm the schedule persisted correctly (this is the real test that `dNt` parsing replaced `dNh`/`dNm` parsing without breaking anything).
37. Confirm the hero card's state pill updates correctly across all three states (gray while `Waiting`, amber with a live percentage while `Running`, green while `Holding`).
38. Confirm **Test Run Sunrise**, **Reset WiFi**, and **Firmware Update** all still work from their new position/styling at the bottom of the page.
39. **New**: reload `/` and confirm each field's caption now visually groups with the field above it (not the one below), and that there's a clearly larger gap before the next field/day - the real test of the `.item` wrapper fix.
40. **New**: confirm Test Run Sunrise and Reset WiFi each show their new explanatory captions.
41. **New**: load `/` in a desktop browser at a wide window size; confirm the page content caps at a narrow, phone-like column centered on the page rather than stretching full-width. Resize the window narrower than ~480px and confirm it behaves the same as it always did (no regression on small windows).

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
- **`_spec/wifi-scheduled-sunrise.md`** — kept in sync, now describes v1+v2+v3 as implemented
