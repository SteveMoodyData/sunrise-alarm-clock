# WiFi-Scheduled Sunrise (ESP32)

## Context

The sunrise clock currently has no way to schedule itself: a smart outlet cuts power to the ESP32 on an external schedule (SmartThings + Zigbee outlet), and the sketch just runs its 512-step color progression once on boot. The ESP32 board (`SRM_Sunrise_ESP32.ino`) was verified working on real ESP32 hardware — LED array confirmed good.

Decisions made building v1:
- **WiFi only, no BLE** — WiFi gets free NTP time sync and a phone-browser-reachable web page with no companion app; BLE needs an actively-paired phone and no wall-clock reference of its own.
- **Time source: WiFi + NTP only, no RTC module** — despite having DS3231 libraries from unrelated past projects, the user chose not to add RTC hardware for this. The board must sync time from the internet at boot.
- **The board must stay powered continuously** (wall adapter, not the smart outlet) so it can keep WiFi/NTP time and watch for its own alarm trigger. The outlet-cuts-power trigger model is retired for this variant.

## Status

**v1 is implemented and confirmed working on real hardware**: WiFi connects (after fixing an SSID typo found via a temporary network-scan diagnostic), NTP syncs, a single weekly alarm (one time + a 7-day bitmask) fires the sunrise and holds until local midnight, settings persist across reboot via NVS, and the whole thing is controlled from a small built-in web page.

**This plan revision covers four v2 additions**, requested after v1 was confirmed working, and already reviewed/resolved against the user in `_spec/wifi-scheduled-sunrise.md`:
1. mDNS hostname (`sunrise-light.local`)
2. Runtime-configurable sunrise duration (currently a compile-time `#define`)
3. Configurable hold-then-auto-off timer (currently holds until local midnight)
4. Fully independent per-day schedule (currently one time + a day-of-week mask; replaced with 7 separate hour/minute/enabled slots, since the user gave an example — M/W/F at 6:30, Sa/Su at 7:30, Tue/Thu off — that a weekday/weekend split can't express)

None of this is implemented yet. This plan describes the target shape of `SRM_Sunrise_Scheduled.ino` after these changes land on top of the working v1 code.

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
- `loadAlarmSettings()` / `saveAlarmSettings(...)` — loop over the 7 days reading/writing `dayHour[i]`/`dayMinute[i]`/`dayEnabled[i]`, plus read/write `sunriseMinutes` and `holdMinutes`. Exact function signature/split (e.g. one combined save vs. separate day-settings and timing-settings saves) is an implementation detail to settle while writing the code, not something that needs deciding up front.
- `updateScheduler()` — `WAITING` check now indexes `dayHour`/`dayMinute`/`dayEnabled` by `now.tm_wday` instead of the old mask/single-alarm fields. On entry to `RUNNING`, also recompute the step interval from the current `sunriseMinutes` and push it into the timer (see `sunrise()` note below). On entry to `HOLD`, record `holdStartMillis = millis()`. `HOLD`'s exit condition changes from "local midnight passed" to "`holdMinutes` have elapsed since `holdStartMillis`."
- `sunrise()` — color math unchanged. **Real implementation constraint, not optional:** the step interval is currently computed once via `static const uint16_t interval = (SUNRISE_MINUTES * 60000UL) / totalSteps;` and handed to FastLED's `EVERY_N_MILLISECONDS_I(sunriseTimer, interval)` macro. Because that's `static`, it's only evaluated the *first* call ever — changing `sunriseMinutes` afterward won't retroactively change an already-running timer's period. v2 needs to explicitly recompute `interval` from the runtime `sunriseMinutes` and call `sunriseTimer.setPeriod(interval)` (the `_I`/"instance" variant of the macro exists specifically to allow this) each time `RUNNING` is entered — not just rely on the macro's implicit static init like v1 does.
- `handleRoot()` / `handleSet()` — expand to render/accept 7 day rows (enabled checkbox + hour + minute per day) plus `sunriseMinutes` and `holdMinutes` fields. `handleSet()` clamps `holdMinutes` to `[0, 120]` the same way `hour`/`minute` are already clamped today.

### State machine — revised

```
WAITING  -- dayEnabled[now.tm_wday]
         && now.tm_hour == dayHour[now.tm_wday] && now.tm_min == dayMinute[now.tm_wday]
         && lastFiredYday != now.tm_yday                        --> RUNNING
            (on entry: currentStep = 0; lastFiredYday = now.tm_yday;
             recompute interval from sunriseMinutes; sunriseTimer.setPeriod(interval))

RUNNING  -- call sunrise() every loop (color math unchanged)
         -- currentStep reaches totalSteps-1                     --> HOLD
            (on entry: holdStartMillis = millis())

HOLD     -- LEDs held at final sunrise color
         -- millis() - holdStartMillis >= holdMinutes * 60000UL  --> WAITING
            (on entry: fill_solid(leds, NUM_LEDS, CRGB::Black))
```

Day-rollover no longer ends `HOLD` — `holdMinutes` does. The `lastFiredYday` guard is unaffected and still independently prevents the same day's alarm from firing twice, even though the board now typically leaves `HOLD` well before midnight.

### Web server routes — revised

- **`GET /`** → `handleRoot()`: adds `sunriseMinutes` and `holdMinutes` number inputs, and replaces the single hour/minute/day-checkboxes/enabled block with **7 rows** (Sun…Sat), each with its own enabled checkbox, hour input, and minute input, pre-filled from the current per-day arrays.
- **`POST /set`** → `handleSet()`: reads `sunriseMinutes`, `holdMinutes` (clamped 0–120), and 7× (`dayNHour`, `dayNMinute`, `dayNEnabled`) fields in a loop; saves all; redirects back to `/` (303), same as v1.
- **`onNotFound`** — unchanged.

### Caveats to document in-code

1. ~~v1's "EVERY_N_MILLISECONDS_I timestamp is stale across WAITING→RUNNING" caveat~~ — superseded by the explicit `setPeriod()` call above, which v2 needs anyway to support runtime duration changes. No longer a known issue once that's in place.
2. `lastFiredYday` still isn't persisted (unchanged from v1) — a reboot after today's alarm already fired is still willing to fire it again the same day if the time is reached again.
3. `connectWiFi()` is still a one-shot attempt with no retry/backoff (unchanged from v1). New in v2: `startMDNS()` only runs if that one-shot attempt succeeded, so the `.local` hostname won't come up at all if WiFi never connects — falls back to no access at all in that case, same as v1's IP-based access would.
4. `WebServer.h` synchronous request handling — unchanged from v1, still fine given no filesystem I/O.
5. **New**: `.local` (mDNS/Bonjour) resolution is native on macOS/iOS/Android/Linux but **not guaranteed on Windows** without Bonjour installed (bundled with iTunes, or standalone "Bonjour Print Services") or a recent-enough Windows 10/11 build. Worth confirming from whatever device is actually used day-to-day rather than assuming it'll work.

## Verification (real hardware)

Carried over from v1, with day-mask language replaced by per-day language, plus new v2-specific checks appended:

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

## Files touched

- **`arduino/standalone/SRM_Sunrise_Scheduled/SRM_Sunrise_Scheduled.ino`** — updated (not new; v1 already exists and works)
- **`docs/Arduino_Hardware.md`** — no changes expected; v1's continuous-power note still applies unchanged
- **`CLAUDE.md`** — no changes expected; the sketch summary there doesn't enumerate individual fields, so v2 doesn't invalidate it
- **`_spec/wifi-scheduled-sunrise.md`** — already updated with the reviewed v2 decisions this plan implements
