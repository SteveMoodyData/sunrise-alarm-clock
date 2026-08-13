#include "FastLED.h"

// How many leds in your strip?
#define NUM_LEDS 24

// How long do you want the sunrise simulation to last (in minutes)?
#define SUNRISE_MINUTES 30

// For led chips like Neopixels, which have a data line, ground, and power, you just
// need to define DATA_PIN.  For led chipsets that are SPI based (four wires - data, clock,
// ground, and power), like the LPD8806 define both DATA_PIN and CLOCK_PIN
#define DATA_PIN 9
//#define CLOCK_PIN 13

// Define the array of leds
CRGB leds[NUM_LEDS];

void setup() { 
  FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  FastLED.setBrightness(255);
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
