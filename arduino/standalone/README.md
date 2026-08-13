# Arduino Sunrise Alarm - Software Setup

This folder contains standalone Arduino sketches for the sunrise alarm clock. No additional software (like WLED) is required - just Arduino IDE and the FastLED library.

## 📁 Files in This Folder

### Available Sketches

**`SRM_Sunrise_Smooth.ino`** - ⭐ **RECOMMENDED**
- **Best for:** Most users, dedicated alarm clock
- **Execution:** Blocking (sunrise runs once, then holds)
- **Steps:** 512 smooth color transitions
- **Brightness:** Linear with 5% minimum (visible from start)
- **Duration:** 30 minutes (easily adjustable)
- **Pros:** Simple code, very smooth, proven to work
- **Cons:** Arduino "pauses" during sunrise (can't add buttons easily)

**`SRM_Sunrise_NonBlocking.ino`**
- **Best for:** Advanced users who want to add features
- **Execution:** Non-blocking (Arduino stays responsive)
- **Steps:** 512 smooth color transitions  
- **Brightness:** Linear with 5% minimum
- **Duration:** 30 minutes (easily adjustable)
- **Pros:** Can add buttons, sensors, display while sunrise runs
- **Cons:** Slightly more complex code

**`SRM_Sunrise_ESP32.ino`**
- **Best for:** ESP32 boards that don't need WiFi
- **Execution:** Non-blocking, same as `SRM_Sunrise_NonBlocking.ino`
- Same color math and steps; only differs in `DATA_PIN` (GPIO 18, since ESP32 boards don't have a `D9` silkscreen label like the Nano) and a note about ESP32's 3.3V logic vs WS2812B's ~5V data threshold
- **Pros:** Runs on ESP32 hardware, still works with the smart-outlet trigger model
- **Cons:** No WiFi features - if you want scheduling/web control, use `SRM_Sunrise_Scheduled.ino` instead

**`SRM_Sunrise_Scheduled.ino`** - 📶 **WiFi-connected, ESP32 only**
- **Best for:** ESP32 users who want onboard scheduling and a web UI instead of a smart outlet
- **Execution:** Non-blocking; connects to WiFi, syncs time via NTP, and serves a web page
- **Steps:** Same 512-step color math as the other sketches
- **Pros:** Independent alarm per day of the week, editable sunrise duration and hold time, manual "Start Now" trigger, settings survive reboot
- **Cons:** Needs continuous power (not the smart outlet), needs a 2.4GHz WiFi network, more setup than the other sketches
- See **[WiFi-Scheduled Sunrise setup](#-wifi-scheduled-sunrise-srm_sunrise_scheduledino)** below for full instructions

**`SRM_Sunrise_Test1.ino`** (Original)
- Legacy version with discrete color steps
- 30-minute duration
- Kept for reference/history

**`SRM_Sunrise_Test2.ino`** (Original) 
- Legacy version with discrete color steps
- 5-minute duration for testing
- Kept for reference/history

> **Note:** every sketch above lives in its own folder matching the `.ino` filename (e.g. `SRM_Sunrise_Smooth/SRM_Sunrise_Smooth.ino`) - the Arduino IDE requires this layout.

---

## 🚀 Quick Start Guide

### Prerequisites

You'll need:
- ✅ Arduino-compatible board (Nano, Uno, ESP32, etc.)
- ✅ WS2812B LED strip or ring (24 LEDs recommended)
- ✅ USB cable for programming
- ✅ Computer with Arduino IDE

### Step 1: Install Arduino IDE

1. **Download Arduino IDE**
   - Go to [arduino.cc/en/software](https://www.arduino.cc/en/software)
   - Download version 1.8.19 or later (or 2.x)
   - Install and launch

2. **Add Board Support** (if using ESP32 or other non-standard board)
   
   **For ESP32:**
   - File → Preferences
   - Add to "Additional Board Manager URLs":
     ```
     https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json
     ```
   - Tools → Board → Boards Manager
   - Search "esp32" and install "esp32 by Espressif Systems"

   **For Arduino Nano (CH340 clone):**
   - May need CH340 USB driver
   - Download from manufacturer or [here](https://learn.sparkfun.com/tutorials/how-to-install-ch340-drivers/all)

### Step 2: Install FastLED Library

FastLED is required for controlling WS2812B LEDs.

**Method 1: Library Manager (Recommended)**
1. In Arduino IDE: Sketch → Include Library → Manage Libraries
2. Search for "FastLED"
3. Install "FastLED by Daniel Garcia"
4. Wait for installation to complete

**Method 2: Manual Install**
1. Download from [FastLED GitHub](https://github.com/FastLED/FastLED)
2. Sketch → Include Library → Add .ZIP Library
3. Select downloaded ZIP file

### Step 3: Configure Hardware Settings

Open your chosen sketch (recommend `SRM_Sunrise_Smooth.ino`) and verify these settings match your hardware:

```cpp
// LED Configuration
#define NUM_LEDS 24              // Change to match your LED count
#define DATA_PIN 9               // GPIO pin connected to LED data
#define SUNRISE_MINUTES 30       // Duration of sunrise in minutes

// LED Type (uncomment the line matching your LEDs)
FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
```

**Common LED Types:**
- `WS2812B` - Most common (NeoPixel compatible)
- `WS2811` - Older version
- `SK6812` - Similar to WS2812B
- `APA102` - Different chipset (requires clock pin)

**Color Order:**
- `GRB` - Most common for WS2812B
- `RGB` - Try this if colors look wrong
- `BRG`, `BGR` - Other variants

### Step 4: Upload to Arduino

1. **Connect your Arduino** via USB cable
2. **Select Board:** Tools → Board → Your board type
   - Arduino Nano: "Arduino Nano"
   - ESP32: "ESP32 Dev Module"
3. **Select Port:** Tools → Port → (Your Arduino's port)
   - Windows: COM3, COM4, etc.
   - Mac: /dev/cu.usbserial-XXXX
   - Linux: /dev/ttyUSB0
4. **Upload:** Click the Upload button (→ arrow)
5. **Wait** for "Done uploading" message

### Step 5: Test Your Setup

After uploading:
1. LEDs should immediately start the sunrise sequence
2. Should see very dim red light within seconds
3. Over 30 minutes, transitions through red → orange → golden yellow
4. Reaches full brightness at 30 minutes

**If nothing happens, see Troubleshooting section below.**

---

## 📶 WiFi-Scheduled Sunrise (`SRM_Sunrise_Scheduled.ino`)

This sketch is a different setup path from the sketches above - it needs an ESP32 board, WiFi, and continuous power, and manages its own alarm schedule instead of relying on a smart outlet.

### What it adds

- **Independent alarm per day of the week** - e.g. Mon/Wed/Fri at 6:30, Sat/Sun at 7:30, other days off
- **Web page** at `http://sunrise-light.local/` (or the board's IP address) to view status and change settings without re-flashing
- **Editable sunrise duration** and **hold time** (how long the light stays on before auto-off after the ramp finishes)
- **Start Now button** to trigger a sunrise on demand, separate from the schedule
- **Settings persist** across power loss/reboot (stored in the ESP32's flash, not just RAM)

### Setup steps

1. **Board:** Tools → Board → **ESP32 Dev Module** (not Arduino Nano). See the ESP32 board-package install steps in Step 1 above if you haven't added ESP32 support to the Arduino IDE yet.

2. **WiFi credentials:** open the `SRM_Sunrise_Scheduled` folder - alongside the `.ino` file you'll find `wifi_secrets.h.example`. Copy it to `wifi_secrets.h` in that same folder, then edit it:
   ```cpp
   #define WIFI_SSID "your-network-name"
   #define WIFI_PASSWORD "your-network-password"
   ```
   `wifi_secrets.h` is gitignored so your real credentials never get committed if this repo is under version control. ESP32 only supports **2.4GHz** WiFi - if your router broadcasts separate 2.4GHz/5GHz networks, use the 2.4GHz one.

3. **Timezone:** in the sketch itself, set `TZ_STRING` to a POSIX TZ string for your location (examples for US Eastern/Pacific are in a comment right above it).

4. **Upload**, then open the Serial Monitor at **115200 baud**. It'll print WiFi connection progress and, once connected, the board's IP address.

5. **Wiring and power:** same LED wiring as the other sketches (`DATA_PIN` 18), but this variant needs to stay **continuously powered** - plug it into a wall adapter or always-on USB power, not the smart outlet. See [Hardware guide](../../docs/Arduino_Hardware.md) for details.

6. **Access the web page:** from a phone or computer on the same WiFi network, go to `http://sunrise-light.local/` (works on macOS/iOS/Android out of the box; Windows may need Bonjour installed) or the IP address printed in Serial Monitor. From there you can set each day's alarm time, the sunrise duration, the hold time, and hit **Start Now** to test the LED sequence immediately.

---

## ⚙️ Configuration Options

### Change Sunrise Duration

At the top of the sketch, modify:

```cpp
#define SUNRISE_MINUTES 30  // Change to 15, 20, 45, 60, etc.
```

Common durations:
- **15 minutes** - Quicker wake-up
- **30 minutes** - Default, gentle wake-up (recommended)
- **45 minutes** - Very gradual
- **60 minutes** - Ultra-gradual for deep sleepers

### Change LED Count

```cpp
#define NUM_LEDS 24  // Change to your actual LED count
```

**Important:** Count must match your physical LEDs exactly!

### Change Data Pin

```cpp
#define DATA_PIN 9  // Change to any available digital pin
```

Good pin choices:
- Arduino Nano: 2-13
- ESP32: Most GPIO pins (avoid input-only pins)

### Adjust Color Range

Find this section in the code:

```cpp
// Hue: 0 (red) to 45 (yellow-orange)
uint8_t hue = map(progress, 0, 255, 0, 45);
```

**Color chart (hue values 0-255):**
- 0 = Red
- 32 = Orange  
- 45 = Yellow-orange (default end)
- 64 = Yellow
- 96 = Green
- 160 = Blue
- 224 = Pink

**Examples:**
```cpp
// More red-orange, less yellow
uint8_t hue = map(progress, 0, 255, 0, 32);

// Extend into yellow range
uint8_t hue = map(progress, 0, 255, 0, 64);

// Start from deep purple to yellow
uint8_t hue = map(progress, 0, 255, 224, 64);
```

### Adjust Brightness Range

```cpp
// Current: 5% minimum to 100% maximum
uint8_t brightness = map(progress, 0, 255, 13, 255);
```

**Adjust minimum brightness:**
```cpp
// Dimmer start (2%)
uint8_t brightness = map(progress, 0, 255, 5, 255);

// Brighter start (10%)  
uint8_t brightness = map(progress, 0, 255, 26, 255);

// Very bright start (20%)
uint8_t brightness = map(progress, 0, 255, 51, 255);
```

**Adjust maximum brightness:**
```cpp
// Less intense ending (80% max)
uint8_t brightness = map(progress, 0, 255, 13, 204);

// Very dim throughout (good for night light)
uint8_t brightness = map(progress, 0, 255, 5, 128);
```

### Change Brightness Curve

**Current (Linear - Recommended):**
```cpp
uint8_t brightness = map(progress, 0, 255, 13, 255);
```

**Alternative curves:**

```cpp
// Quadratic (slow start, accelerates)
uint16_t brightSquared = (uint16_t)progress * progress;
uint8_t brightness = map(brightSquared / 255, 0, 255, 13, 255);

// Square root (fast start, slows down)
uint8_t brightness = map(sqrt(progress * 255), 0, 255, 13, 255);

// Smoothstep (slow-fast-slow, most natural)
float normalized = progress / 255.0;
float eased = normalized * normalized * (3.0 - 2.0 * normalized);
uint8_t brightness = map(eased * 255, 0, 255, 13, 255);
```

---

## 🔧 Troubleshooting

### Upload Issues

**"Port not found" or "Device not recognized"**
- Check USB cable (must be data cable, not power-only)
- Install CH340 drivers for Nano clones
- Try different USB port
- Check Device Manager (Windows) for port conflicts

**"Sketch too big"**
- Not common with Nano/ESP32
- If using ATtiny or small board, reduce `NUM_LEDS`

**"Error compiling"**
- Make sure FastLED library is installed
- Check syntax errors (missing semicolons, brackets)
- Verify board selection matches your hardware

### LED Issues

**No LEDs light up**
- ✅ Check wiring: Data pin, 5V, GND all connected
- ✅ Verify `NUM_LEDS` matches your physical count
- ✅ Confirm you're connected to INPUT end of LED strip (look for arrow)
- ✅ Test with simple code (see below)
- ✅ Check power supply is adequate

**Simple test code:**
```cpp
#include "FastLED.h"
#define NUM_LEDS 24
#define DATA_PIN 9
CRGB leds[NUM_LEDS];

void setup() { 
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
}

void loop() { 
  leds[0] = CRGB::Red;    // First LED red
  leds[1] = CRGB::Green;  // Second LED green
  leds[2] = CRGB::Blue;   // Third LED blue
  FastLED.show();
  delay(1000);
}
```

**Wrong colors (red shows as green, etc.)**
- Change color order in setup:
  ```cpp
  // Try these variations:
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);  // Default
  FastLED.addLeds<WS2812B, DATA_PIN, RGB>(leds, NUM_LEDS);  // Alternative
  FastLED.addLeds<WS2812B, DATA_PIN, BRG>(leds, NUM_LEDS);  // Another option
  ```

**Only first few LEDs work**
- Check `NUM_LEDS` value
- Verify power supply can handle all LEDs
- One bad LED in chain can break rest (bypass it)

**Flickering or random colors**
- Add 1000µF capacitor across power supply
- Shorten data wire (keep under 6 inches if possible)
- Add 330Ω resistor in series with data line
- Ensure common ground between Arduino and LEDs

### Timing Issues

**Sunrise too fast**
- Check `SUNRISE_MINUTES` value
- Verify you're using the updated code (not Test1/Test2)

**Sunrise too slow**
- Increase `SUNRISE_MINUTES` value
- Or decrease for faster sunrise

**Takes 2+ minutes for LEDs to appear**
- Make sure you're using the version with minimum brightness
- Look for: `map(progress, 0, 255, 13, 255)`
- NOT: `progress * progress / 255`

---

## 🔌 Power Considerations

### USB Power (24 LEDs) ✅
- Arduino Nano + 24 LEDs: ~800mA average
- Standard USB 2.0: 500mA (might brownout)
- USB 3.0 or phone charger: 900-2400mA (plenty)
- **Recommendation:** Use 1A+ USB adapter

### External Power (60+ LEDs)
For strips with 60+ LEDs, use external 5V power supply:

```
5V Power Supply (3A+)
├─ LED Strip 5V
├─ LED Strip GND ──┬── Arduino GND
└─ (don't connect to Arduino 5V)
                    │
              Arduino Nano
              (powered separately via USB)
```

**Important:** Always connect grounds together!

---

## 📚 Code Explanation

### How the Sunrise Works

**SRM_Sunrise_Smooth.ino (Blocking Version):**

```cpp
void loop() {
  const uint16_t totalSteps = 512;  // Number of color changes
  const uint16_t stepDelay = (SUNRISE_MINUTES * 60000UL) / totalSteps;
  
  for (uint16_t step = 0; step < totalSteps; step++) {
    // Calculate how far through sunrise (0-255)
    uint8_t progress = map(step, 0, totalSteps - 1, 0, 255);
    
    // Map progress to hue (red to yellow-orange)
    uint8_t hue = map(progress, 0, 255, 0, 45);
    
    // Map progress to saturation (pure to warm)
    uint8_t saturation = map(progress, 0, 255, 255, 180);
    
    // Map progress to brightness (5% to 100%)
    uint8_t brightness = map(progress, 0, 255, 13, 255);
    
    // Set all LEDs to calculated color
    fill_solid(leds, NUM_LEDS, CHSV(hue, saturation, brightness));
    FastLED.show();
    
    delay(stepDelay);  // Wait before next step (~3.5 seconds)
  }
  
  delay(3600000);  // Hold final color for 1 hour
}
```

**Key concepts:**
- **HSV color space:** Hue (color), Saturation (purity), Value (brightness)
- **Mapping:** Converts step number to color values
- **Blocking:** `delay()` pauses execution

---

## 🎓 Next Steps

### Add Features (Non-Blocking Version Only)

**Add a button to restart sunrise:**
```cpp
const int BUTTON_PIN = 2;

void setup() {
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
}

void loop() {
  if (digitalRead(BUTTON_PIN) == LOW) {
    currentStep = 0;  // Reset sunrise
    delay(200);       // Debounce
  }
  
  // ... rest of sunrise code ...
}
```

**Add serial output for debugging:**
```cpp
void setup() {
  Serial.begin(9600);
  // ... rest of setup ...
}

void loop() {
  // ... in your step increment section ...
  Serial.print("Step: ");
  Serial.print(currentStep);
  Serial.print(" / ");
  Serial.println(totalSteps);
}
```

### Smart Home Integration

See [SmartThings setup guide](../../docs/smartthings_setup.md) for:
- Automating with smart outlets
- Scheduling sunrise for weekdays only
- Auto-shutoff after sunrise completes

Prefer the board to manage its own schedule instead of a smart outlet? See [WiFi-Scheduled Sunrise](#-wifi-scheduled-sunrise-srm_sunrise_scheduledino) above - it covers per-day scheduling and auto-shutoff natively, no smart outlet required.

### Hardware Improvements

See [Hardware guide](../../docs/Arduino_Hardware.md) for:
- Enclosure options (3D printed, IKEA Fado lamp)
- External power supply for more LEDs
- Adding diffusers for better light distribution

---

## 📖 Additional Resources

- **FastLED Documentation:** [fastled.io](http://fastled.io/)
- **WS2812B Datasheet:** Technical specs for LEDs
- **Arduino Reference:** [arduino.cc/reference](https://www.arduino.cc/reference/en/)
- **Project Repository:** [Full documentation and updates](https://github.com/SteveMoodyData/sunrise-alarm-clock)

---

## 💡 Tips & Tricks

**Faster testing:**
- Set `SUNRISE_MINUTES` to 1 for quick tests
- Change back to 30 for production use

**Preserve settings:**
- Keep a copy of your customized sketch
- Document your changes in comments

**Multiple alarms:**
- Use multiple smart outlets on different schedules
- Or add RTC (real-time clock) module for standalone scheduling

**Sunset mode:**
- Reverse the color progression for bedtime
- Start at yellow, end at dim red
- Helps wind down before sleep

---

**Need help?** Open an [issue](https://github.com/SteveMoodyData/sunrise-alarm-clock/issues) or check [discussions](https://github.com/SteveMoodyData/sunrise-alarm-clock/discussions)!
