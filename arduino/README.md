# Arduino Implementation

This folder contains Arduino-based implementations of the sunrise alarm clock.

## 📂 Folder Structure

```
arduino/
└── standalone/          ← All Arduino sketches are here
    ├── README.md        ← Complete setup guide
    ├── SRM_Sunrise_Smooth.ino (RECOMMENDED)
    ├── SRM_Sunrise_NonBlocking.ino
    ├── SRM_Sunrise_Test1.ino
    └── SRM_Sunrise_Test2.ino
```

## 🚀 Get Started

👉 **[Go to standalone folder for complete setup guide](standalone/README.md)**

## Quick Links

- **Hardware Setup:** [Arduino + Hardware Guide](../docs/Arduino+Hardware.md)
- **Smart Home Integration:** [SmartThings Setup](../docs/smartthings_setup.md)
- **Troubleshooting:** [Common Issues](../docs/troubleshooting.md)
- **Back to Main:** [Project Overview](../README.md)

## Overview

### Why Choose Arduino?

**Pros:**
- ✅ Simple, focused functionality
- ✅ No WiFi required
- ✅ Fast boot time (~2 seconds)
- ✅ Low cost (~$3-8 for board)
- ✅ Easy to customize timing and colors
- ✅ Works with any Arduino-compatible board

**Cons:**
- ❌ Single purpose (just sunrise alarm)
- ❌ Requires re-uploading to change settings
- ❌ No web interface

**Best for:**
- Dedicated bedside alarm clock
- Simple, reliable wake-up light
- Users who want a "set it and forget it" solution
- First-time makers

### Hardware Requirements

**Minimum:**
- Arduino Nano or compatible (~$3-5)
- WS2812B LED ring/strip (~$5-12)
- USB cable for power
- Smart outlet for automation (optional)

**Steve's Build:**
- Arduino Nano clone (CH340)
- 24-pixel WS2812B LED ring
- Zigbee smart outlet + SmartThings
- 3D printed enclosures + IKEA Fado lamp

Total cost: $15-30

📋 **[Full hardware guide](../docs/Arduino+Hardware.md)**

### Features

- 🌅 **512-step smooth color transitions**
- 🎨 **Natural sunrise colors:** Deep red → Orange → Golden yellow
- ⏱️ **Configurable duration:** 5 to 60+ minutes
- 💡 **Visible from start:** 5% minimum brightness
- 🔧 **Easy customization:** All settings in code
- 🏠 **Smart home ready:** Works with any smart outlet

## Comparison: Smooth vs Non-Blocking

| Feature | Smooth (Recommended) | Non-Blocking |
|---------|---------------------|--------------|
| **Complexity** | ⭐⭐ Simple | ⭐⭐⭐ Moderate |
| **Sunrise Quality** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| **Responsiveness** | Blocked during sunrise | Always responsive |
| **Add features** | Difficult | Easy (buttons, display, etc.) |
| **Best for** | Most users | Advanced tinkerers |

**Recommendation:** Start with **Smooth** version. Switch to Non-Blocking only if you want to add buttons, sensors, or other interactive features.

## Quick Start (3 Steps)

1. **Install software:**
   - Arduino IDE
   - FastLED library
   - Board support (if needed)

2. **Upload sketch:**
   - Open `SRM_Sunrise_Smooth.ino`
   - Verify `NUM_LEDS` and `DATA_PIN` match your hardware
   - Upload to board

3. **Connect hardware:**
   - LED Data → GPIO 9
   - LED 5V → Power
   - LED GND → Ground

📖 **[Detailed instructions in standalone folder](standalone/README.md)**

## Example Code Snippet

```cpp
#include "FastLED.h"

#define NUM_LEDS 24
#define DATA_PIN 9
#define SUNRISE_MINUTES 30

CRGB leds[NUM_LEDS];

void setup() { 
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
}

void loop() {
  // 512 smooth steps from red to golden yellow
  // Takes exactly 30 minutes
  // See full code in standalone folder
}
```

## Common Customizations

**Change duration:**
```cpp
#define SUNRISE_MINUTES 15  // Instead of 30
```

**Change colors:**
```cpp
uint8_t hue = map(progress, 0, 255, 0, 64);  // More yellow
```

**Adjust brightness:**
```cpp
uint8_t brightness = map(progress, 0, 255, 26, 255);  // Brighter start
```

## Need Help?

- 📖 **[Complete setup guide](standalone/README.md)**
- 🔧 **[Hardware wiring](../docs/Arduino+Hardware.md)**
- 🐛 **[Troubleshooting](../docs/troubleshooting.md)**
- 💬 **[Ask questions](https://github.com/SteveMoodyData/sunrise-alarm-clock/discussions)**
- 🐞 **[Report issues](https://github.com/SteveMoodyData/sunrise-alarm-clock/issues)**

---

**Ready to build?** → **[Start here: Standalone folder](standalone/README.md)** →
