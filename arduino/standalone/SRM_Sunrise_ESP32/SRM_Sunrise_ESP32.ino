#include <FastLED.h>



// How many leds in your strip?
#define NUM_LEDS 24

// How long do you want the sunrise simulation to last (in minutes)?
#define SUNRISE_MINUTES 30


// ESP-WROOM-32 dev boards have no D9 (that's a Nano-only silkscreen label).
// GPIO18 is a safe general-purpose output: not a strapping pin (0/2/12/15),
// not flash-connected (6-11), and not input-only (34-39).
#define DATA_PIN 18

// Define the array of leds
CRGB leds[NUM_LEDS];

void setup() {
  // ESP32 GPIOs drive 3.3V logic; WS2812B data expects ~5V (its threshold is
  // roughly 0.7*VDD). Many strips still read a 3.3V signal fine, especially
  // with a short data wire and the first LED close to the MCU, but if you see
  // flicker or wrong colors on LED 0, add a level shifter (e.g. 74AHCT125)
  // or a ~330-500 ohm resistor in series on the data line as a workaround.
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255); // Max brightness, we'll control brightness via HSV
}

void loop() {
  sunrise();
  FastLED.show();
}

void sunrise() {

  // Total number of steps in the sunrise (512 for smooth gradations)
  static const uint16_t totalSteps = 512;

  // Calculate interval between each step to achieve desired sunrise length
  // 30 minutes = 1,800,000 milliseconds / 512 steps = ~3,516 ms per step
  static const uint16_t interval = (SUNRISE_MINUTES * 60000UL) / totalSteps;

  // Current step in the sunrise progression
  static uint16_t currentStep = 0;

  // Only increment every interval milliseconds (non-blocking)
  EVERY_N_MILLISECONDS_I(sunriseTimer, interval) {
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
  // This makes the later yellows warmer and less pure
  uint8_t saturation = map(progress, 0, 255, 255, 180);

  // Brightness: Start at 0 (black), gradually increase to full brightness
  // Using a quadratic curve for more natural acceleration
  uint16_t brightSquared = progress * progress;
  uint8_t brightness = brightSquared / 255;

  // Set all LEDs to the same color (unified sunrise effect)
  fill_solid(leds, NUM_LEDS, CHSV(hue, saturation, brightness));
}
