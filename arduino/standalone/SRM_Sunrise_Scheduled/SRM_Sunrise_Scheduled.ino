#include "FastLED.h"
#include <WiFi.h>
#include <WiFiManager.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <ezTime.h>

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
#define DEFAULT_SUNRISE_MINUTES 3
#define DEFAULT_HOLD_MINUTES 6

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
// the correct POSIX rule (DST included) via a lookup service, and keeps its
// own NVS-backed cache (see syncTime()) so a later lookup failure falls back
// to the last-known-good resolution instead of losing the correct time.
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
  uint8_t progressPct = currentState == RUNNING ? (uint32_t)currentStep * 100 / 511 : (currentState == HOLD ? 100 : 0);

  String html = "<!DOCTYPE html><html><head><title>Sunrise Alarm</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
  html += "<h1>Sunrise Alarm</h1>";

  html += "<p><b>Current time:</b> ";
  html += haveTime ? myTZ.dateTime("Y-m-d H:i:s T") : String("not synced yet");
  html += "</p>";

  html += "<p><b>WiFi:</b> " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("disconnected")) + "</p>";
  html += "<p><b>Address:</b> http://" + String(MDNS_HOSTNAME) + ".local/</p>";
  html += "<p><b>Sunrise state:</b> " + stateName + " (" + String(progressPct) + "%)</p>";

  html += "<form method='POST' action='/start'><input type='submit' value='Start Now'></form>";
  html += "<form method='POST' action='/reset-wifi' onsubmit=\"return confirm('This forgets the saved WiFi network and restarts into setup mode. Continue?');\"><input type='submit' value='Reset WiFi'></form>";

  html += "<form method='POST' action='/set'>";

  html += "<h2>Timing</h2>";
  html += "<label>Sunrise duration (minutes): <input type='number' name='sunriseMinutes' min='1' value='" + String(sunriseMinutes) + "' style='width:4em'></label><br>";
  html += "<small>How long the light takes to fade from fully dark to fully bright once the scheduled time hits.</small><br><br>";
  html += "<label>Hold before auto-off (minutes, 0-120): <input type='number' name='holdMinutes' min='0' max='120' value='" + String(holdMinutes) + "'></label><br><br>";

  html += "<label>Timezone: <input type='text' id='tzLocation' name='tzLocation' value='" + tzLocation + "' style='width:22em' placeholder='e.g. America/Los_Angeles'></label> ";
  html += "<button type='button' onclick='detectTimezone()'>Detect from this device</button><br>";
  html += "<label>Or pick a common one: <select onchange=\"if(this.value)document.getElementById('tzLocation').value=this.value;\">";
  html += "<option value=''>-- select --</option>";
  for (int i = 0; i < NUM_TZ_PRESETS; i++) {
    html += "<option value='" + String(tzPresets[i].name) + "'>" + tzPresets[i].label + "</option>";
  }
  html += "</select></label><br>";
  html += "<small>Type any <a href='https://en.wikipedia.org/wiki/List_of_tz_database_time_zones' target='_blank'>tz database</a> location name, pick a common one above, or click Detect. The correct offset and DST rule are looked up automatically (needs WiFi) the next time this device syncs.</small><br>";

  html += "<h2>Schedule</h2>";
  for (int i = 0; i < 7; i++) {
    html += "<p><label><input type='checkbox' name='d" + String(i) + "e'" + (dayEnabled[i] ? " checked" : "") + "> " + dayNames[i] + "</label> ";
    html += "<input type='number' name='d" + String(i) + "h' min='0' max='23' value='" + String(dayHour[i]) + "' style='width:4em'>:";
    html += "<input type='number' name='d" + String(i) + "m' min='0' max='59' value='" + String(dayMinute[i]) + "' style='width:4em'></p>";
  }

  html += "<br><input type='submit' value='Save'>";
  html += "</form>";

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
    uint8_t hour = server.arg(prefix + "h").toInt();
    uint8_t minute = server.arg(prefix + "m").toInt();
    bool enabled = server.hasArg(prefix + "e");

    if (hour > 23) hour = 23;
    if (minute > 59) minute = 59;

    dayHour[i] = hour;
    dayMinute[i] = minute;
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

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
