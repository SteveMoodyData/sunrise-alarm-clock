#include "FastLED.h"

// How many leds in your strip?
#define NUM_LEDS 24

// How long do you want the4 sunrise simulation to last? 
#define MINUTES 5
#define DELAY_NUM (MINUTES * 60000)/(NUM_LEDS*8) // 30 MINUTES * 60000 = 1,800,000ms / NUM_LEDS*8 for 8 color shifts = 24*8 = 192  
                                                 // 30 minutes = 1800 seconds = 1,800,000 ms. 24 LEDs and 8 color shifts. 1,800,000/(24*8) = 1,800,000/192 = 9375ms per LED 

// For led chips like Neopixels, which have a data line, ground, and power, you just
// need to define DATA_PIN.  For led chipsets that are SPI based (four wires - data, clock,
// ground, and power), like the LPD8806 define both DATA_PIN and CLOCK_PIN
#define DATA_PIN 9
//#define CLOCK_PIN 13

// Define the array of leds
CRGB leds[NUM_LEDS];

void setup() { 
      // Uncomment/edit one of the following lines for your leds arrangement.
      // FastLED.addLeds<TM1803, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<TM1804, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<TM1809, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<WS2811, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<WS2812, DATA_PIN, RGB>(leds, NUM_LEDS);
       FastLED.addLeds<WS2812B, DATA_PIN, GRB>(leds, NUM_LEDS);
  	  // FastLED.addLeds<NEOPIXEL, DATA_PIN>(leds, NUM_LEDS);
      // FastLED.addLeds<APA104, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<UCS1903, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<UCS1903B, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<GW6205, DATA_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<GW6205_400, DATA_PIN, RGB>(leds, NUM_LEDS);
      
      // FastLED.addLeds<WS2801, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<SM16716, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<LPD8806, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<P9813, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<APA102, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<DOTSTAR, RGB>(leds, NUM_LEDS);

      // FastLED.addLeds<WS2801, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<SM16716, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<LPD8806, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<P9813, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<APA102, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
      // FastLED.addLeds<DOTSTAR, DATA_PIN, CLOCK_PIN, RGB>(leds, NUM_LEDS);
}

void loop() { 


  // total sunrise length, in minutes
  static const uint8_t sunriseLength = MINUTES;

  // how often (in seconds) should the heat color increase?
  // for the default of 30 minutes, this should be about every 7 seconds
  // 7 seconds x 256 gradient steps = 1,792 seconds = ~30 minutes

  static const float DELAY_NUM = ((float)(sunriseLength * 60) / 255)*1000;




    // Fade in red getting brighter 

//      fill_solid(leds, NUM_LEDS, CHSV( HUE_RED, 240, 50)); // start solid 
//          FastLED.show();
//        delay(DELAY_NUM);
    
    
      for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_RED, 255, 75);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_RED, 255, 75);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }

     for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_RED, 255, 100);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_RED, 255, 100);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }

    for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_RED, 255, 155);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_RED, 255, 155);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }  

    // then fade to Orange 

    for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_ORANGE, 255, 200);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_ORANGE, 255, 100);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }

    for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_ORANGE, 255, 155);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_ORANGE, 255, 150);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }

    for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_ORANGE, 255, 155);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_ORANGE, 255, 200);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }
    
    for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CHSV( HUE_ORANGE, 255, 255);
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CHSV( HUE_ORANGE, 255, 250);
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }

    for (uint8_t i = 0; i < NUM_LEDS/2; i++) {
      leds[i] = CRGB::Orange;
      FastLED.show();  // update the display
      delay(DELAY_NUM);
      leds[NUM_LEDS -1 - i] = CRGB::Orange;
       FastLED.show();  // update the display
    delay(DELAY_NUM);
    }

    
    fill_solid(leds, NUM_LEDS,  CRGB::Orange); // Set all to final color for same time as MUNUTES (30 minutes of light by default)
    FastLED.show();
    delay(DELAY_NUM * 500);
    
}
