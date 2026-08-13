#include "FastLED.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <ESPmDNS.h>
#include <time.h>

// WIFI_SSID / WIFI_PASSWORD are defined in wifi_secrets.h (gitignored, not
// committed - see wifi_secrets.h.example in this same folder for the
// template and setup instructions).
#include "wifi_secrets.h"

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

// ESP-WROOM-32 dev boards have no D9 (that's a Nano-only silkscreen label).
// GPIO18 is a safe general-purpose output; see SRM_Sunrise_ESP32.ino for why.
#define DATA_PIN 18

// --- WiFi ---
// WIFI_SSID / WIFI_PASSWORD come from wifi_secrets.h, included above.
#define WIFI_CONNECT_TIMEOUT_MS 15000

// --- mDNS ---
// Bare hostname only - do NOT include ".local" here. MDNS.begin() takes the
// bare name; ".local" is appended by whatever's resolving it. Putting
// ".local" in this constant would make the device advertise itself as
// "sunrise-light.local.local".
#define MDNS_HOSTNAME "sunrise-light"

// --- NTP / timezone ---
// POSIX TZ string for your local timezone. Examples:
//   US Eastern:  "EST5EDT,M3.2.0,M11.1.0"
//   US Pacific:  "PST8PDT,M3.2.0,M11.1.0"
// Find yours in the POSIX TZ database (search "POSIX TZ string" for your city).
#define TZ_STRING "PST8PDT,M3.2.0,M11.1.0"
#define NTP_SERVER "pool.ntp.org"
#define NTP_SYNC_TIMEOUT_MS 15000

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

// Timestamp recorded on RUNNING->HOLD, used to time the auto-off.
uint32_t holdStartMillis = 0;

// Guards against firing more than once on the same calendar day. Intentionally
// NOT persisted to NVS: if the board reboots mid-day after already firing
// today, it's willing to fire again if the alarm time is reached again. Fine
// for now - see plan notes.
int16_t lastFiredYday = -1;

void setup() {
  Serial.begin(115200);

  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255); // Max brightness, we'll control brightness via HSV
  fill_solid(leds, NUM_LEDS, CRGB::Black);
  FastLED.show();

  connectWiFi();
  startMDNS();
  syncTime();
  loadAlarmSettings();

  server.on("/", HTTP_GET, handleRoot);
  server.on("/set", HTTP_POST, handleSet);
  server.on("/start", HTTP_POST, handleStartNow);
  server.onNotFound(handleNotFound);
  server.begin();
}

void loop() {
  server.handleClient();
  updateScheduler();
  FastLED.show();
}

// TEMPORARY DIAGNOSTIC: lists every network the ESP32's radio can actually
// see (name, signal strength, security type) before attempting to connect.
// Remove this call once WiFi connect issues are resolved.
void scanAndPrintNetworks() {
  Serial.println("Scanning for WiFi networks...");
  int count = WiFi.scanNetworks();
  if (count == 0) {
    Serial.println("  No networks found at all - check the board is powered/antenna is intact.");
    return;
  }
  for (int i = 0; i < count; i++) {
    Serial.printf("  [%2d] SSID: \"%s\"  RSSI: %d dBm  Encryption: %s\n",
                  i, WiFi.SSID(i).c_str(), WiFi.RSSI(i),
                  WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "open" : "secured");
  }
  Serial.println();
}

void connectWiFi() {
  WiFi.mode(WIFI_STA);
  scanAndPrintNetworks();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  Serial.print("Connecting to WiFi");
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_CONNECT_TIMEOUT_MS) {
    delay(250);
    Serial.print(".");
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
  } else {
    // Fails safe: no WiFi means no NTP time means the scheduler just never
    // matches and never fires. It won't crash or block startup.
    Serial.println();
    Serial.print("WiFi connect timed out (status code ");
    Serial.print(WiFi.status());
    Serial.println(") - continuing without network.");
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

  configTzTime(TZ_STRING, NTP_SERVER);

  Serial.print("Syncing time");
  struct tm timeinfo;
  uint32_t start = millis();
  while (!getLocalTime(&timeinfo, 100) && millis() - start < NTP_SYNC_TIMEOUT_MS) {
    Serial.print(".");
  }
  Serial.println();

  if (getLocalTime(&timeinfo, 100)) {
    Serial.print("Time synced: ");
    Serial.println(&timeinfo, "%Y-%m-%d %H:%M:%S %Z");
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

  struct tm now;
  if (getLocalTime(&now, 0)) {
    lastFiredYday = now.tm_yday;
  }

  uint16_t interval = (sunriseMinutes * 60000UL) / 512;
  sunriseTimer.setPeriod(interval);
  sunriseTimer.reset();

  currentState = RUNNING;
}

void updateScheduler() {
  switch (currentState) {
    case WAITING: {
      struct tm now;
      if (!getLocalTime(&now, 0)) break; // no time yet, can't evaluate the schedule

      int wday = now.tm_wday;
      bool timeMatches = (now.tm_hour == dayHour[wday]) && (now.tm_min == dayMinute[wday]);
      bool notAlreadyFiredToday = (lastFiredYday != now.tm_yday);

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

void handleRoot() {
  struct tm now;
  bool haveTime = getLocalTime(&now, 0);

  String stateName = currentState == WAITING ? "Waiting" : currentState == RUNNING ? "Running" : "Holding";
  uint8_t progressPct = currentState == RUNNING ? (uint32_t)currentStep * 100 / 511 : (currentState == HOLD ? 100 : 0);

  String html = "<!DOCTYPE html><html><head><title>Sunrise Alarm</title>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1'></head><body>";
  html += "<h1>Sunrise Alarm</h1>";

  html += "<p><b>Current time:</b> ";
  if (haveTime) {
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &now);
    html += buf;
  } else {
    html += "not synced yet";
  }
  html += "</p>";

  html += "<p><b>WiFi:</b> " + (WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : String("disconnected")) + "</p>";
  html += "<p><b>Address:</b> http://" + String(MDNS_HOSTNAME) + ".local/</p>";
  html += "<p><b>Sunrise state:</b> " + stateName + " (" + String(progressPct) + "%)</p>";

  html += "<form method='POST' action='/start'><input type='submit' value='Start Now'></form>";

  html += "<form method='POST' action='/set'>";

  html += "<h2>Timing</h2>";
  html += "<label>Sunrise duration (minutes): <input type='number' name='sunriseMinutes' min='1' value='" + String(sunriseMinutes) + "' style='width:4em'></label><br>";
  html += "<small>How long the light takes to fade from fully dark to fully bright once the scheduled time hits.</small><br><br>";
  html += "<label>Hold before auto-off (minutes, 0-120): <input type='number' name='holdMinutes' min='0' max='120' value='" + String(holdMinutes) + "'></label><br>";

  html += "<h2>Schedule</h2>";
  for (int i = 0; i < 7; i++) {
    html += "<p><label><input type='checkbox' name='d" + String(i) + "e'" + (dayEnabled[i] ? " checked" : "") + "> " + dayNames[i] + "</label> ";
    html += "<input type='number' name='d" + String(i) + "h' min='0' max='23' value='" + String(dayHour[i]) + "' style='width:4em'>:";
    html += "<input type='number' name='d" + String(i) + "m' min='0' max='59' value='" + String(dayMinute[i]) + "' style='width:4em'></p>";
  }

  html += "<br><input type='submit' value='Save'>";
  html += "</form>";

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

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleStartNow() {
  beginSunrise();

  server.sendHeader("Location", "/");
  server.send(303);
}

void handleNotFound() {
  server.send(404, "text/plain", "Not found");
}
