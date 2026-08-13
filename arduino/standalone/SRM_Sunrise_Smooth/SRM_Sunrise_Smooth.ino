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
  FastLED.setBrightness(255); // Max brightness, we'll control brightness via HSV
}

void loop() { 
  
  // Total number of steps in the sunrise
  // Using 512 steps gives us smooth gradations over 30 minutes
  const uint16_t totalSteps = 512;
  
  // Calculate delay between each step to achieve desired sunrise length
  // 30 minutes = 1,800,000 milliseconds / 512 steps = ~3,516 ms per step
  const uint16_t stepDelay = (SUNRISE_MINUTES * 60000UL) / totalSteps;
  
  // Sunrise color progression
  for (uint16_t step = 0; step < totalSteps; step++) {
    
    // Map step to 0-255 range for easier calculations
    uint8_t progress = map(step, 0, totalSteps - 1, 0, 255);
    
    // HSV Color mapping:
    // Hue: Start at 0 (red), gradually shift to 32 (orange), then to 45 (yellow-orange)
    // We'll map 0-255 progress to hue 0-45
    uint8_t hue = map(progress, 0, 255, 0, 45);
    
    // Saturation: Start fully saturated (255), gradually reduce to add white
    // This makes the later yellows warmer and less pure
    uint8_t saturation = map(progress, 0, 255, 255, 180);
    
    // Brightness: Start at 0 (black), gradually increase to full brightness
    // Using a quadratic curve for more natural acceleration
    // progress^2 / 255 gives us exponential feel
    uint16_t brightSquared = progress * progress;
    uint8_t brightness = brightSquared / 255;
    
    // Set all LEDs to the same color (unified sunrise effect)
    fill_solid(leds, NUM_LEDS, CHSV(hue, saturation, brightness));
    FastLED.show();
    
    delay(stepDelay);
  }
  
  // Hold at final warm color for 1 hour before looping
  delay(3600000);
}
