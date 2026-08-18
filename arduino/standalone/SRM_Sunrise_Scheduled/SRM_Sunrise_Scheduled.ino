#include "FastLED.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ezTime.h>
#include <Update.h>

// ---------------------------------------------------------------------------
// POWER SETUP CHANGE: unlike the other sketches in this repo, this variant
// must stay powered continuously (wall adapter / always-on USB power), NOT
// the smart outlet that cuts power on a schedule. The ESP32 now owns the
// alarm schedule itself over WiFi + NTP, so it needs to keep running and
// keep its clock synced at all times. See docs/Arduino_Hardware.md.
// ---------------------------------------------------------------------------

// How many leds in your strip?
#define NUM_LEDS 24

// First-boot fallback only - once the web form has been submitted once, the
// runtime values (sunriseMinutes / holdMinutes below) come from NVS instead.
#define DEFAULT_SUNRISE_MINUTES 30
#define DEFAULT_HOLD_MINUTES 60

// ESP32 dev boards have no D9 (that's a Nano-only silkscreen label).
// GPIO18 is a safe general-purpose output; see SRM_Sunrise_ESP32.ino for why.
#define DATA_PIN 18

// --- WiFi ---
// Credentials are no longer hardcoded here: WiFiManager owns them (its own
// flash storage, separate from this sketch's "sunrise" Preferences
// namespace). On first boot (or after a "Reset WiFi"), the device broadcasts
// this network name and serves a captive-portal setup page.
#define SETUP_AP_NAME "sunrise-light-setup"
// How long the captive portal stays open before giving up and continuing
// offline (same fails-safe philosophy as the rest of this sketch - see
// connectWiFi()).
#define WIFI_CONFIG_PORTAL_TIMEOUT_S 180

// --- mDNS ---
// Bare hostname only - do NOT include ".local" here. MDNS.begin() takes the
// bare name; ".local" is appended by whatever's resolving it. Putting
// ".local" in this constant would make the device advertise itself as
// "sunrise-light.local.local".
#define MDNS_HOSTNAME "sunrise-light"

// --- NTP / timezone ---
// First-boot fallback only - once the web form has been submitted once (by
// hand, via the dropdown, or via the "Detect from this device" button), the
// runtime value (tzLocation below) comes from NVS instead. Any tz-database
// location name works (e.g. "America/Los_Angeles", "Europe/London") - ezTime
// resolves it (including the correct DST rule) via a lookup service, so no
// POSIX TZ string needs to be typed or computed by hand anymore.
#define DEFAULT_TZ_LOCATION "America/Los_Angeles"
#define NTP_SYNC_TIMEOUT_S 15

// --- Web server ---
#define WEB_SERVER_PORT 80

// Shown on the status page - bump this after changes worth confirming took
// effect post-update (see the /update firmware page).
#define FIRMWARE_VERSION "0.1.2"

// Define the array of leds
CRGB leds[NUM_LEDS];

WebServer server(WEB_SERVER_PORT);
Preferences prefs;

enum SunriseState { WAITING, RUNNING, HOLD };
SunriseState currentState = WAITING;

// Moved from sunrise()'s function-local static (as in SRM_Sunrise_NonBlocking.ino)
// to file scope, so updateScheduler() can reset it to 0 when a new alarm fires.
uint16_t currentStep = 0;

// Also file-scope rather than declared via EVERY_N_MILLISECONDS_I: that macro
// creates a *function-local* static timer, which updateScheduler() couldn't
// reach to retune. Declaring the underlying FastLED timer class directly here
// lets updateScheduler() call sunriseTimer.setPeriod()/reset() whenever
// sunriseMinutes changes or a new sunrise starts.
CEveryNMillis sunriseTimer;

// In-RAM cache of the NVS-backed schedule: one independent alarm per day of
// the week, indexed by tm_wday (0=Sunday .. 6=Saturday). Lets e.g. Mon/Wed/Fri
// run at a different time than Sat/Sun, with other days disabled entirely -
// a fixed weekday/weekend split couldn't express that.
uint8_t dayHour[7]   = {6, 6, 6, 6, 6, 6, 6};
uint8_t dayMinute[7] = {30, 30, 30, 30, 30, 30, 30};
bool dayEnabled[7]   = {false, false, false, false, false, false, false};

// Runtime-editable sunrise duration and post-sunrise hold time (both
// web-configurable and NVS-persisted; the DEFAULT_* #defines above are only
// the fallback used before the form has ever been submitted).
uint16_t sunriseMinutes = DEFAULT_SUNRISE_MINUTES;
uint8_t holdMinutes = DEFAULT_HOLD_MINUTES; // 0-120, clamped in handleSet()

// ezTime's timezone object. Resolves a tz-database location name (below) to
// the correct POSIX rule (DST included) via a lookup service - see
// syncTime() for why no cache is configured (a lookup failure just means no
// time until the next successful sync, same as everything else here).
Timezone myTZ;

// Runtime-editable tz-database location name (web-configurable and
// NVS-persisted, same pattern as sunriseMinutes/holdMinutes above). Settable
// by hand, via the web UI's preset dropdown, or via "Detect from this
// device" (reads the browser's own Intl.DateTimeFormat timezone).
String tzLocation = DEFAULT_TZ_LOCATION;

// Timestamp recorded on RUNNING->HOLD, used to time the auto-off.
uint32_t holdStartMillis = 0;

// Guards against firing more than once on the same calendar day. Intentionally
// NOT persisted to NVS: if the board reboots mid-day after already firing
// today, it's willing to fire again if the alarm time is reached again. Fine
// for now - see plan notes. -1 never matches myTZ.dayOfYear()'s 1-366 range.
int32_t lastFiredYday = -1;

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255); // Max brightness, we'll control brightness via HSV
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  connectWiFi();
  startMDNS();
  loadAlarmSettings(); // must run before syncTime() - it loads tzLocation
  syncTime();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/start", HTTP_POST, handleStartNow);
  server.on("/reset-wifi", HTTP_POST, handleResetWifi);
  server.on("/update", HTTP_GET, handleUpdatePage);
  server.on("/update", HTTP_POST, handleFirmwareUpdate, handleFirmwareUpload);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  events(); // ezTime: services scheduled NTP refreshes - must be called regularly
  server.handleClient();
  updateScheduler();
  FastLED.show();
}

// Tries previously-saved credentials (WiFiManager's own storage) first. If
// that fails, it automatically starts an AP + captive portal broadcasting
// SETUP_AP_NAME - no separate fallback logic needed, this is autoConnect()'s
// default behavior. Blocks setup() while the portal is open, but only during
// first-time setup or right after a "Reset WiFi" - never during normal
// runtime, since a successful reconnect to saved credentials is fast.
void connectWiFi() {
  WiFiManager wm;
  wm.setConfigPortalTimeout(WIFI_CONFIG_PORTAL_TIMEOUT_S);
  bool connected = wm.autoConnect(SETUP_AP_NAME);

  if (connected) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    // Fails safe: no WiFi means no NTP time means the scheduler just never
    // matches and never fires. It won't crash or block startup. The portal
    // timed out without anyone finishing setup - a power-cycle will try again.
    Serial.println("WiFi setup portal timed out - continuing without network.");
  }
}

void startMDNS() {
  // Only meaningful once WiFi is up; skipped otherwise, same fails-safe
  // philosophy as syncTime() below.
  if (WiFi.status() != WL_CONNECTED) return;

  if (MDNS.begin(MDNS_HOSTNAME)) {
    MDNS.addService("http", "tcp", WEB_SERVER_PORT);
    Serial.print("mDNS responder started: http://");
    Serial.print(MDNS_HOSTNAME);
    Serial.println(".local/");
  } else {
    Serial.println("mDNS responder failed to start.");
  }
}

void syncTime() {
  if (WiFi.status() != WL_CONNECTED) return;

  // No myTZ.setCache() here: this ezTime build's NVS-backed cache overload
  // is only compiled in when EZTIME_CACHE_NVS is defined ahead of the
  // library's own ezTime.cpp - a sketch-level #define doesn't reach that
  // separately-compiled translation unit, so it link-errors instead
  // (confirmed via a real build). Not worth chasing further: a lookup
  // failure just means no time this boot, same fails-safe behavior as
  // every other network-dependent step in this sketch.
  if (!myTZ.setLocation(tzLocation)) {
    Serial.print("Timezone lookup failed for \"");
    Serial.print(tzLocation);
    Serial.print("\": ");
    Serial.println(errorString());
  }

  Serial.print("Syncing time...");
  if (waitForSync(NTP_SYNC_TIMEOUT_S)) {
    Serial.print("synced: ");
    Serial.println(myTZ.dateTime("Y-m-d H:i:s T"));
  } else {
    Serial.println("NTP sync timed out - scheduler will wait until time is available.");
  }
}

void loadAlarmSettings() {
  prefs.begin("sunrise", /*readOnly=*/false);

  for (int i = 0; i < 7; i++) {
    String prefix = "d" + String(i);
    dayHour[i] = prefs.getUChar((prefix + "h").c_str(), dayHour[i]);
    dayMinute[i] = prefs.getUChar((prefix + "m").c_str(), dayMinute[i]);
    dayEnabled[i] = prefs.getBool((prefix + "e").c_str(), dayEnabled[i]);
  }

  sunriseMinutes = prefs.getUShort("sunriseMinutes", sunriseMinutes);
  holdMinutes = prefs.getUChar("holdMinutes", holdMinutes);
  tzLocation = prefs.getString("tzLocation", tzLocation);

  prefs.end();
}

// Persists whatever is currently in the in-RAM schedule/timing globals.
// handleSet() validates and updates those globals first, then calls this once
// to write everything in a single NVS transaction.
void saveAlarmSettings() {
  prefs.begin("sunrise", /*readOnly=*/false);

  for (int i = 0; i < 7; i++) {
    String prefix = "d" + String(i);
    prefs.putUChar((prefix + "h").c_str(), dayHour[i]);
    prefs.putUChar((prefix + "m").c_str(), dayMinute[i]);
    prefs.putBool((prefix + "e").c_str(), dayEnabled[i]);
  }

  prefs.putUShort("sunriseMinutes", sunriseMinutes);
  prefs.putUChar("holdMinutes", holdMinutes);
  prefs.putString("tzLocation", tzLocation);

  prefs.end();
}

// Resets state and (re)tunes the timer to begin a sunrise immediately. Used
// both when the schedule matches (updateScheduler()'s WAITING case) and when
// the web UI's "Start Now" button is pressed (handleStartNow()). The sunrise
// itself runs off millis(), not wall-clock time, so it proceeds even if NTP
// hasn't synced - only lastFiredYday (which suppresses a same-day re-fire
// from the schedule) is skipped in that case, since it needs today's date.
void beginSunrise() {
  currentStep = 0;

  if (timeStatus() == timeSet) {
    lastFiredYday = myTZ.dayOfYear();
  }

  uint16_t interval = (sunriseMinutes * 60000UL) / 512;
  sunriseTimer.setPeriod(interval);
  sunriseTimer.reset();

  currentState = RUNNING;
}

void updateScheduler() {
  switch (currentState) {
    case WAITING: {
      if (timeStatus() != timeSet) break; // no time yet, can't evaluate the schedule

      // The installed ezTime build's dateTime("w") (and weekday(), which
      // returns the same underlying value) actually output 1=Sunday..7=
      // Saturday despite the library's own "0 = Sunday" comment - confirmed
      // by reading ezTime.cpp's breakTime(), which sets tm.Wday = 1 for
      // Sunday. Subtract 1 to get the 0=Sunday..6=Saturday range this
      // sketch's dayHour[]/dayMinute[]/dayEnabled[] arrays are indexed by -
      // without this, every day was checked against the wrong day's slot
      // (and Saturday indexed one past the end of each 7-element array).
      int wday = myTZ.dateTime("w").toInt() - 1;
      bool timeMatches = (myTZ.hour() == dayHour[wday]) && (myTZ.minute() == dayMinute[wday]);
      bool notAlreadyFiredToday = (lastFiredYday != (int32_t)myTZ.dayOfYear());

      if (dayEnabled[wday] && timeMatches && notAlreadyFiredToday) {
        beginSunrise();
      }
      break;
    }

    case RUNNING: {
      sunrise();
      if (currentStep >= 511) {
        holdStartMillis = millis();
        currentState = HOLD;
      }
      break;
    }

    case HOLD: {
      // LEDs already hold the final sunrise color from the last sunrise() call.
      if (millis() - holdStartMillis >= (uint32_t)holdMinutes * 60000UL) {
        fill_solid(leds, NUM_LEDS, CRGB::Black);
        currentState = WAITING;
      }
      break;
    }
  }
}

void sunrise() {

  // Total number of steps in the sunrise (512 for smooth gradations)
  static const uint16_t totalSteps = 512;

  // sunriseTimer's period is set by updateScheduler() (via setPeriod()) each
  // time RUNNING is entered, using whatever sunriseMinutes currently is - not
  // a static computed once here, since sunriseMinutes can change at runtime
  // via the web form.
  if (sunriseTimer) {
    if (currentStep < totalSteps) {
      currentStep++;
    }
  }

  // Map current step to 0-255 range for easier calculations
  uint8_t progress = map(currentStep, 0, totalSteps - 1, 0, 255);

  // HSV Color mapping for natural sunrise:
  // Hue: Start at 0 (deep red), gradually shift to 45 (golden yellow-orange)
  uint8_t hue = map(progress, 0, 255, 0, 45);

  // Saturation: Start fully saturated (255), gradually reduce to add warmth
  uint8_t saturation = map(progress, 0, 255, 255, 180);

  // Brightness: Start at 0 (black), gradually increase to full brightness
  // Using a quadratic curve for more natural acceleration
  uint16_t brightSquared = progress * progress;
  uint8_t brightness = brightSquared / 255;

  // Set all LEDs to the same color (unified sunrise effect)
  fill_solid(leds, NUM_LEDS, CHSV(hue, saturation, brightness));
}

// --- Web routes ---

const char* dayNames[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// Zero-pads to 2 digits for building "HH:MM" values for <input type=time>.
String zeroPad2(uint8_t n) {
  return (n < 10 ? "0" : "") + String(n);
}

// A small hand-picked set of common tz-database locations for the settings
// form's dropdown - convenience only, not exhaustive. Any location name
// (not just ones listed here) can still be typed into the text field or
// filled in via "Detect from this device".
struct TzPreset { const char* name; const char* label; };
const TzPreset tzPresets[] = {
  {"America/Los_Angeles", "US Pacific"},
  {"America/Denver", "US Mountain"},
  {"America/Chicago", "US Central"},
  {"America/New_York", "US Eastern"},
  {"America/Anchorage", "US Alaska"},
  {"Pacific/Honolulu", "US Hawaii"},
  {"America/Toronto", "Canada Eastern"},
  {"America/Vancouver", "Canada Pacific"},
  {"Europe/London", "UK"},
  {"Europe/Paris", "Central Europe"},
  {"Australia/Sydney", "Australia Eastern"},
  {"Asia/Tokyo", "Japan"},
  {"Asia/Kolkata", "India"},
  {"UTC", "UTC"},
};
const int NUM_TZ_PRESETS = sizeof(tzPresets) / sizeof(tzPresets[0]);

void handleRoot() {
  bool haveTime = (timeStatus() == timeSet);

  String stateName = currentState == WAITING ? "Waiting" : currentState == RUNNING ? "Running" : "Holding";
  String stateClass = currentState == WAITING ? "waiting" : currentState == RUNNING ? "running" : "hold";
  uint8_t progressPct = currentState == RUNNING ? (uint32_t)currentStep * 100 / 511 : (currentState == HOLD ? 100 : 0);
  if (currentState == RUNNING) stateName += " " + String(progressPct) + "%";

  String html = "<!DOCTYPE html><html><head><title>Sunrise Alarm</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'>";

  // iOS-Settings-style theme: light gray background, white grouped cards,
  // uppercase section labels, pure-CSS toggle switches (no JS needed - the
  // underlying <input type=checkbox> still submits normally), single warm
  // accent color (#ff9500) nodding to the product itself. No web fonts/CDN -
  // this page has to render standalone from the ESP32.
  html += "<style>";
  html += "*{box-sizing:border-box;}";
  // max-width + auto margins: on a phone (viewport already narrower than
  // this) it has no effect, page stays full-width same as before. On a
  // wider window it caps the content to a phone-ish column instead of
  // stretching rows edge-to-edge across the screen.
  html += "body{margin:0 auto;max-width:480px;padding:16px 16px 40px;background:#f2f2f7;font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif;color:#000;}";
  html += "h1{font-size:26px;margin:4px 0 16px;}";
  html += ".section-label{font-size:13px;font-weight:600;color:#6e6e73;text-transform:uppercase;letter-spacing:.5px;margin:24px 4px 8px;}";
  html += ".card{background:#fff;border-radius:10px;overflow:hidden;box-shadow:0 1px 2px rgba(0,0,0,.05);}";
  html += ".row{display:flex;align-items:center;justify-content:space-between;padding:12px 16px;gap:10px;}";
  // Border lives on .item (a row plus its optional .explain caption below
  // it), not on .row itself - a row's own border would land between the
  // field and its caption instead of after it, making captions read as
  // belonging to the next field down rather than the one above.
  html += ".item{border-bottom:1px solid #e5e5ea;}";
  html += ".item:last-child{border-bottom:none;}";
  html += ".hero .time{font-size:19px;font-weight:600;}";
  html += ".hero .sub{color:#6e6e73;font-size:13px;margin-top:4px;}";
  html += ".pill{display:inline-block;padding:2px 10px;border-radius:999px;font-size:13px;font-weight:600;color:#fff;margin-left:6px;}";
  html += ".pill.waiting{background:#8e8e93;}.pill.running{background:#ff9500;}.pill.hold{background:#34c759;}";
  html += ".row label{font-size:16px;}";
  html += ".row input[type=number],.row input[type=time]{border:none;background:none;font-size:16px;text-align:right;font-family:inherit;color:#000;}";
  html += ".row input[type=time]{width:110px;}";
  html += ".day-name{font-size:16px;width:40px;}";
  html += ".toggle{position:relative;display:inline-block;width:44px;height:26px;flex-shrink:0;}";
  html += ".toggle input{opacity:0;width:0;height:0;position:absolute;}";
  html += ".toggle .track{position:absolute;inset:0;background:#e5e5ea;border-radius:999px;transition:background .15s;}";
  html += ".toggle .track::before{content:'';position:absolute;width:22px;height:22px;left:2px;top:2px;background:#fff;border-radius:50%;transition:transform .15s;box-shadow:0 1px 3px rgba(0,0,0,.3);}";
  html += ".toggle input:checked+.track{background:#ff9500;}.toggle input:checked+.track::before{transform:translateX(18px);}";
  // Negative margin-top pulls the caption up snug against its own field;
  // the item's border-bottom (after this padding) then reads as the gap
  // before the *next* field - tight coupling to what it explains, clear
  // separation from what it doesn't.
  html += ".explain{font-size:13px;color:#6e6e73;padding:0 16px 14px;margin-top:-6px;}";
  html += ".explain a{color:#ff9500;}";
  html += ".tz-row{flex-direction:column;align-items:stretch;}";
  html += ".tz-row input[type=text]{text-align:left;width:100%;padding:6px 0;}";
  html += ".tz-actions{display:flex;gap:8px;margin-top:6px;}";
  html += ".tz-actions button,.tz-actions select{flex:1;padding:8px;border-radius:8px;border:1px solid #d1d1d6;background:#fff;font-size:14px;font-family:inherit;}";
  html += ".save-btn{display:block;width:100%;padding:14px;margin-top:20px;background:#ff9500;color:#fff;border:none;border-radius:10px;font-size:17px;font-weight:600;}";
  html += ".row-btn{all:unset;box-sizing:border-box;display:flex;align-items:center;justify-content:space-between;width:100%;padding:14px 16px;font-size:17px;color:#000;background:#fff;cursor:pointer;}";
  html += ".row-link{display:flex;align-items:center;justify-content:space-between;padding:14px 16px;font-size:17px;color:#000;text-decoration:none;}";
  html += ".row-btn:active,.row-link:active{background:#f2f2f7;}";
  html += ".danger{color:#ff3b30;}";
  html += ".chev{color:#c7c7cc;}";
  html += "form{margin:0;}";
  html += "</style></head><body>";

  html += "<h1>Sunrise Alarm</h1>";

  html += "<div class='card hero'><div class='row'><div>";
  html += "<div class='time'>";
  html += haveTime ? myTZ.dateTime("Y-m-d H:i:s T") : String("Time not synced yet");
  html += "<span class='pill " + stateClass + "'>" + stateName + "</span>";
  html += "</div>";
  html += "<div class='sub'>" + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("WiFi disconnected"));
  html += " &middot; " + String(MDNS_HOSTNAME) + ".local &middot; v" + FIRMWARE_VERSION + "</div>";
  html += "</div></div></div>";

  html += "<form method='POST' action='/set'>";

  html += "<div class='section-label'>Schedule</div><div class='card'>";
  for (int i = 0; i < 7; i++) {
    String prefix = "d" + String(i);
    String timeVal = zeroPad2(dayHour[i]) + ":" + zeroPad2(dayMinute[i]);
    html += "<div class='item'><div class='row'>";
    html += "<span class='day-name'>" + String(dayNames[i]) + "</span>";
    html += "<input type='time' name='" + prefix + "t' value='" + timeVal + "'>";
    html += "<label class='toggle'><input type='checkbox' name='" + prefix + "e'" + (dayEnabled[i] ? " checked" : "") + "><span class='track'></span></label>";
    html += "</div></div>";
  }
  html += "</div>";

  html += "<div class='section-label'>Timing</div><div class='card'>";
  html += "<div class='item'><div class='row'><label for='sunriseMinutes'>Sunrise duration</label><input type='number' id='sunriseMinutes' name='sunriseMinutes' min='1' value='" + String(sunriseMinutes) + "' style='width:4em'></div>";
  html += "<div class='explain'>How long the light takes to fade from fully dark to fully bright once the scheduled time hits.</div></div>";
  html += "<div class='item'><div class='row'><label for='holdMinutes'>Hold before auto-off</label><input type='number' id='holdMinutes' name='holdMinutes' min='0' max='120' value='" + String(holdMinutes) + "'></div>";
  html += "<div class='explain'>Minutes to stay lit after the ramp finishes before turning off (0-120).</div></div>";
  html += "</div>";

  html += "<div class='section-label'>Timezone</div><div class='card'>";
  html += "<div class='item'><div class='row tz-row'>";
  html += "<input type='text' id='tzLocation' name='tzLocation' value='" + tzLocation + "' placeholder='e.g. America/Los_Angeles'>";
  html += "<div class='tz-actions'>";
  html += "<button type='button' onclick='detectTimezone()'>Detect from this device</button>";
  html += "<select onchange=\"if(this.value)document.getElementById('tzLocation').value=this.value;\">";
  html += "<option value=''>Pick common&hellip;</option>";
  for (int i = 0; i < NUM_TZ_PRESETS; i++) {
    html += "<option value='" + String(tzPresets[i].name) + "'>" + tzPresets[i].label + "</option>";
  }
  html += "</select></div></div>";
  html += "<div class='explain'>Type any <a href='https://en.wikipedia.org/wiki/List_of_tz_database_time_zones' target='_blank'>tz database</a> location name, pick a common one, or click Detect. The correct offset and DST rule are looked up automatically (needs WiFi) the next time this device syncs.</div></div>";
  html += "</div>";

  html += "<button type='submit' class='save-btn'>Save</button>";
  html += "</form>";

  html += "<div class='section-label'>Device</div><div class='card'>";
  html += "<div class='item'><form method='POST' action='/start'><button type='submit' class='row-btn'>Test Run Sunrise<span class='chev'>&rsaquo;</span></button></form>";
  html += "<div class='explain'>Runs the full sunrise sequence right now, using the current duration setting - handy for testing without waiting for the scheduled time. Doesn't change the schedule.</div></div>";
  html += "<div class='item'><form method='POST' action='/reset-wifi' onsubmit=\"return confirm('This forgets the saved WiFi network and restarts into setup mode. Continue?');\"><button type='submit' class='row-btn danger'>Reset WiFi<span class='chev'>&rsaquo;</span></button></form>";
  html += "<div class='explain'>Forgets the saved WiFi network and restarts, broadcasting the sunrise-light-setup hotspot again so it can join a different network. Schedule and other settings aren't affected.</div></div>";
  html += "<div class='item'><a href='/update' class='row-link'>Firmware Update<span class='chev'>&rsaquo;</span></a></div>";
  html += "</div>";

  // Reads the browser's own resolved IANA timezone name directly - modern
  // browsers already know this correctly (DST rules included), so no
  // client-side guesswork is needed here. ezTime resolves this name to the
  // right POSIX rule server-side once saved (see syncTime()/handleSet()).
  html += "<script>";
  html += "function detectTimezone(){document.getElementById('tzLocation').value=Intl.DateTimeFormat().resolvedOptions().timeZone;}";
  html += "</script>";

  html += "</body></html>";

  server.send(200, "text/html", html);
}

void handleSet() {
  uint16_t newSunriseMinutes = server.arg("sunriseMinutes").toInt();
  if (newSunriseMinutes < 1) newSunriseMinutes = 1;
  sunriseMinutes = newSunriseMinutes;

  uint8_t newHoldMinutes = server.arg("holdMinutes").toInt();
  if (newHoldMinutes > 120) newHoldMinutes = 120;
  holdMinutes = newHoldMinutes;

  // Cap length defensively (a legit location name is well under this); keeps
  // the current value if the field was left blank. Only re-resolves via
  // ezTime's lookup service if the location actually changed - setLocation()
  // needs >3s between calls per its own docs, and there's no reason to hit
  // the network again on every settings save if the timezone wasn't touched.
  String newTzLocation = server.arg("tzLocation");
  newTzLocation.trim();
  bool tzChanged = false;
  if (newTzLocation.length() > 0 && newTzLocation.length() <= 60 && newTzLocation != tzLocation) {
    tzLocation = newTzLocation;
    tzChanged = true;
  }

  for (int i = 0; i < 7; i++) {
    String prefix = "d" + String(i);
    bool enabled = server.hasArg(prefix + "e");

    // <input type=time> submits "HH:MM" (24-hour, zero-padded) per the HTML
    // spec regardless of the browser's display locale. Falls back to the
    // existing value if the field is missing/malformed rather than zeroing
    // it out, same defensive spirit as the other fields in this handler.
    String t = server.arg(prefix + "t");
    int colon = t.indexOf(':');
    if (colon > 0) {
      uint8_t hour = t.substring(0, colon).toInt();
      uint8_t minute = t.substring(colon + 1).toInt();
      if (hour > 23) hour = 23;
      if (minute > 59) minute = 59;
      dayHour[i] = hour;
      dayMinute[i] = minute;
    }

    dayEnabled[i] = enabled;
  }

  saveAlarmSettings();

  // Re-resolves immediately rather than waiting for reboot, so the new
  // timezone is reflected in the next page load's "Current time". Only
  // possible with WiFi up, since (unlike the old POSIX-string approach) this
  // needs a network lookup; if offline, the saved location is picked up on
  // the next successful syncTime() (boot or reconnect).
  if (tzChanged && WiFi.status() == WL_CONNECTED) {
    if (!myTZ.setLocation(tzLocation)) {
      Serial.print("Timezone lookup failed for \"");
      Serial.print(tzLocation);
      Serial.print("\": ");
      Serial.println(errorString());
    }
  }

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStartNow() {
  beginSunrise();

  server.sendHeader("Location", "/");
  server.send(303);
}

// Forgets the saved WiFi network (WiFiManager's own storage) and restarts.
// On reboot, autoConnect() finds no saved credentials and immediately opens
// the setup portal - lets someone deliberately switch networks without
// first having to fail a connection to trigger the auto-fallback.
void handleResetWifi() {
  WiFiManager wm;
  wm.resetSettings();
  server.send(200, "text/plain", "WiFi settings cleared. Restarting...");
  delay(1000);
  ESP.restart();
}

// GET /update - a plain file-upload form. Point it at the .bin produced by
// Arduino IDE's Sketch -> Export Compiled Binary (not the .ino itself - the
// ESP32 has no on-device compiler, this flashes an already-built firmware
// image directly into the OTA app partition via the Update library).
// Reachable any time the device is on the network, unlike WiFiManager's own
// /update page (only alive during its transient captive-portal window).
void handleUpdatePage() {
  String html = "<!DOCTYPE html><html><head><title>Firmware Update</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
  html += "<h1>Firmware Update</h1>";
  html += "<p>Select the .bin from Arduino IDE's \"Sketch &gt; Export Compiled Binary\".</p>";
  html += "<form method='POST' action='/update' enctype='multipart/form-data'>";
  html += "<input type='file' name='update' accept='.bin'> ";
  html += "<input type='submit' value='Upload'>";
  html += "</form>";
  html += "<p><a href='/'>Back</a></p>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

// Streams the uploaded file straight into the Update library chunk by
// chunk as it arrives, rather than buffering the whole thing - same
// approach WiFiManager's own /update handler uses internally.
void handleFirmwareUpload() {
  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    Serial.print("Firmware update starting: ");
    Serial.println(upload.filename);
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.print("Firmware update complete: ");
      Serial.print(upload.totalSize);
      Serial.println(" bytes written");
    } else {
      Update.printError(Serial);
    }
  }
}

// Runs after handleFirmwareUpload() has fully processed the request body -
// reports whatever Update ended up recording (success or the error latched
// by the printError() calls above) and reboots either way, since a failed
// partial flash shouldn't be left half-applied.
void handleFirmwareUpdate() {
  bool ok = !Update.hasError();
  server.sendHeader("Connection", "close");
  server.send(200, "text/plain", ok ? "Update successful. Restarting..." : "Update failed. Restarting...");
  delay(1000);
  ESP.restart();
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
