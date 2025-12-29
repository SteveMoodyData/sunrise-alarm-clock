# 🌅 Smart Sunrise Alarm Clock

A gradual, natural sunrise simulation using WS2812B LED strips and ESP32. Wake up gently to 30 minutes of smooth color transitions from deep red through orange to warm golden yellow.

![Sunrise Demo](docs/images/sunrise-demo.gif)
*30-minute color progression from dark red to golden yellow*

## ✨ Features

- **Natural Color Progression**: Deep red → orange → golden yellow mimicking a real sunrise
- **Ultra-Smooth Transitions**: 512 discrete steps for imperceptible color changes
- **Smart Home Integration**: Compatible with SmartThings, Home Assistant, or any smart outlet
- **Customizable Duration**: Easily adjust from 5 to 60+ minutes
- **Two Implementations**: Choose between Arduino standalone or WLED custom effect
- **Low Cost**: ~$15-20 in parts for a complete sunrise alarm

## 🎥 Demo

[Add video or GIF showing the sunrise in action]

## 🤔 Which Version Should I Choose?

### Arduino Standalone
**Best for:** Dedicated alarm clock device

**Pros:**
- ✅ Simple, focused functionality
- ✅ No WiFi required (optional)
- ✅ Fastest boot time
- ✅ Easiest to customize timing

**Cons:**
- ❌ Single purpose only
- ❌ Harder to change settings (requires re-upload)

👉 [Arduino Setup Guide](arduino/README.md)

---

### WLED Custom Effect
**Best for:** Multi-purpose lighting (alarm + regular use)

**Pros:**
- ✅ Full WLED features (party lights, effects, sync)
- ✅ Web interface for easy control
- ✅ Works as regular LED controller when not alarming
- ✅ OTA updates, mobile app support

**Cons:**
- ❌ More complex setup (compile custom firmware)
- ❌ WiFi required
- ❌ Slightly slower boot time

👉 [WLED Setup Guide](wled/README.md)

## 🛠️ Hardware Requirements

### Required Components
- **ESP32 Development Board** (~$5-8)
  - ESP32 DevKit, ESP32-WROOM, or similar
- **WS2812B LED Strip** (~$8-12)
  - Recommend 24-60 LEDs (1 meter at 60 LEDs/m)
  - 5V addressable RGB (NeoPixels)
- **5V Power Supply** (~$6-10)
  - Minimum 2A for 24 LEDs
  - 3A+ recommended for 60 LEDs
- **Smart Outlet** (optional, ~$10-15)
  - For automation via SmartThings, Alexa, Google Home, etc.

### Optional Components
- Diffuser material (white acrylic, frosted plastic)
- Enclosure or mounting hardware
- Capacitor (1000µF) for power smoothing
- Level shifter (if experiencing flickering)

**Total Cost:** $15-50 depending on components and smart outlet

📋 [Full parts list with links](docs/hardware.md)

## ⚡ Quick Start

### Arduino Version

1. **Clone the repository:**
   ```bash
   git clone https://github.com/yourusername/sunrise-alarm-clock.git
   cd sunrise-alarm-clock/arduino/standalone
   ```

2. **Install Arduino IDE and libraries:**
   - [Arduino IDE](https://www.arduino.cc/en/software)
   - ESP32 board support
   - FastLED library

3. **Upload the sketch:**
   - Open `SRM_Sunrise_NonBlocking.ino`
   - Select your ESP32 board
   - Upload

4. **Wire the hardware:**
   - LED Data → GPIO 9
   - LED 5V → Power supply +5V
   - LED GND → Common ground

📖 [Detailed Arduino guide](arduino/README.md)

---

### WLED Version

1. **Clone the repository:**
   ```bash
   git clone https://github.com/yourusername/sunrise-alarm-clock.git
   cd sunrise-alarm-clock
   ```

2. **Download WLED source:**
   ```bash
   git clone https://github.com/Aircoookie/WLED.git
   ```

3. **Add the custom effect:**
   - Copy code from `wled/sunrise_effect.cpp`
   - Follow installation instructions in `wled/installation.md`

4. **Compile and upload:**
   - Use PlatformIO (recommended) or Arduino IDE
   - Upload to ESP32

5. **Configure WLED:**
   - Create preset with "Sunrise 30min" effect
   - Set as boot preset

📖 [Detailed WLED guide](wled/installation.md)

## 🏠 Smart Home Integration

### SmartThings Setup
1. Add smart outlet to SmartThings
2. Create automation:
   - **Trigger:** Time (e.g., 7:00 AM on weekdays)
   - **Action:** Turn on outlet
   - **Optional:** Turn off after 35 minutes

### Home Assistant Setup
```yaml
automation:
  - alias: "Morning Sunrise Alarm"
    trigger:
      platform: time
      at: "07:00:00"
    condition:
      condition: time
      weekday:
        - mon
        - tue
        - wed
        - thu
        - fri
    action:
      service: switch.turn_on
      entity_id: switch.sunrise_alarm_outlet
```

📖 [Full smart home guide](docs/smartthings_setup.md)

## 🎨 Customization

### Change Duration
**Arduino:**
```cpp
#define SUNRISE_MINUTES 30  // Change to 15, 45, 60, etc.
```

**WLED:**
```cpp
const uint16_t sunriseDuration = 30;  // Change to desired minutes
```

### Adjust Color Range
```cpp
// Change the ending hue (0=red, 32=yellow-orange, 64=yellow, 96=green)
uint8_t hue = map(progress, 0, 255, 0, 32);  // Red to yellow-orange

// Change saturation (255=pure color, 180=warm, 0=white)
uint8_t sat = map(progress, 0, 255, 255, 180);  // Pure to warm
```

### Brightness Curve
```cpp
// Linear (current)
uint8_t val = progress;

// Slower start, faster end
uint8_t val = sqrt(progress * 255);

// Faster start, slower end (quadratic)
uint8_t val = (progress * progress) / 255;
```

## 📊 Comparison Table

| Feature | Arduino Standalone | WLED Custom Effect |
|---------|-------------------|-------------------|
| **Sunrise quality** | ⭐⭐⭐⭐⭐ | ⭐⭐⭐⭐⭐ |
| **Setup complexity** | ⭐⭐⭐⭐ (Easy) | ⭐⭐⭐ (Moderate) |
| **Multi-use** | ❌ No | ✅ Yes |
| **Web interface** | ❌ No | ✅ Yes |
| **Boot time** | ~2 seconds | ~15 seconds |
| **WiFi required** | ❌ No | ✅ Yes |
| **OTA updates** | ❌ No | ✅ Yes |
| **Smart home** | Via outlet only | Full integration |

## 📖 Documentation

- [Hardware Setup & Wiring](docs/hardware.md)
- [Arduino Implementation Details](arduino/README.md)
- [WLED Implementation Details](wled/README.md)
- [SmartThings Integration](docs/smartthings_setup.md)
- [Comparison: Arduino vs WLED](docs/comparison.md)
- [Troubleshooting Guide](docs/troubleshooting.md)

## 🐛 Troubleshooting

### LEDs not turning on
- Check power supply voltage (should be 5V)
- Verify wiring connections
- Confirm LED count in code matches actual strip

### Colors look wrong
- Check color order setting (GRB vs RGB)
- Verify FastLED/WLED LED type configuration
- Try different data pin if flickering

### Sunrise too fast/slow
- Verify `sunriseDuration` value
- Check millis() timing (Arduino) or effect loop (WLED)
- See troubleshooting guide for detailed fixes

📖 [Full troubleshooting guide](docs/troubleshooting.md)

## 🤝 Contributing

Contributions are welcome! Please feel free to submit a Pull Request. For major changes:

1. Fork the repository
2. Create a feature branch (`git checkout -b feature/AmazingFeature`)
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`)
4. Push to the branch (`git push origin feature/AmazingFeature`)
5. Open a Pull Request

## 📝 License

This project is licensed under the MIT License - see the [LICENSE](LICENSE) file for details.

## 🙏 Acknowledgments

- [FastLED Library](https://github.com/FastLED/FastLED) - Amazing LED control library
- [WLED Project](https://github.com/Aircoookie/WLED) - Fantastic ESP32 LED controller
- Inspired by commercial sunrise alarm clocks like Philips Wake-Up Light
- Thanks to the maker community for feedback and improvements

## 📬 Contact & Support

- **Issues:** [GitHub Issues](https://github.com/yourusername/sunrise-alarm-clock/issues)
- **Discussions:** [GitHub Discussions](https://github.com/yourusername/sunrise-alarm-clock/discussions)
- **Project Updates:** [Watch this repo for updates]

---

**⭐ If this project helped you wake up more gently, please star the repository!**

**💡 Have ideas for improvements? Open an issue or discussion!**

## 📸 Gallery

[Add photos of completed builds, different enclosures, installation examples]

## 🗺️ Roadmap

- [ ] Add sunset mode (reverse colors for bedtime)
- [ ] Web-based configuration tool
- [ ] Home Assistant custom component
- [ ] PCB design for cleaner builds
- [ ] 3D printable enclosures
- [ ] Pre-compiled WLED binaries
- [ ] Add audio integration (nature sounds)

---

Made with ☕ and 💡 by [Your Name]
