#pragma once
#include <Arduino.h>
#include <Adafruit_NeoPixel.h>
#include <math.h>
#include "pins.h"

// 5-LED WS2812 ring around the bezel — ambient state indicator
namespace leds {

enum class Mode : uint8_t { OFF, IDLE, WORKING, PERM, NOTIFY };

inline Adafruit_NeoPixel ring(LED_COUNT, PIN_LED, NEO_GRB + NEO_KHZ800);
inline Mode mode = Mode::OFF;
inline uint32_t lastStep = 0;
inline uint16_t phase = 0;

inline void init() {
  pinMode(PIN_POWER_LIGHT, OUTPUT);
  digitalWrite(PIN_POWER_LIGHT, LOW); // factory demo boot state
  ring.begin();
  ring.setBrightness(45);
  ring.clear();
  ring.show();
}

inline void set(Mode m) {
  if (m != mode) {
    mode = m;
    phase = 0;
  }
}

inline void tick(uint32_t now) {
  if (now - lastStep < 70) return;
  lastStep = now;
  phase++;
  switch (mode) {
    case Mode::OFF:
      if (phase > 2) return; // already dark, stop re-pushing
      ring.clear();
      break;
    case Mode::IDLE: // faint green
      for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, ring.Color(0, 14, 3));
      break;
    case Mode::WORKING: { // orange chase
      ring.clear();
      ring.setPixelColor(phase % LED_COUNT, ring.Color(140, 60, 20));
      ring.setPixelColor((phase + LED_COUNT - 1) % LED_COUNT, ring.Color(35, 15, 5));
      break;
    }
    case Mode::PERM: { // red pulse
      uint8_t v = 30 + (uint8_t)((sinf(phase * 0.35f) + 1.0f) * 90.0f);
      for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, ring.Color(v, 0, 0));
      break;
    }
    case Mode::NOTIFY: { // blue pulse
      uint8_t v = 20 + (uint8_t)((sinf(phase * 0.25f) + 1.0f) * 55.0f);
      for (int i = 0; i < LED_COUNT; i++) ring.setPixelColor(i, ring.Color(0, v / 3, v));
      break;
    }
  }
  ring.show();
}

} // namespace leds
