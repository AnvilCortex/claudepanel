#pragma once
#include <Arduino.h>

// Quadrature encoder with transition accumulator. consumeSteps() divides the
// raw count by a configurable divisor (a detent = 4 transitions; tunable at
// runtime via the bridge cfg msg). ISR lives in encoder.cpp — Xtensa rejects
// IRAM_ATTR bodies inlined in headers ("l32r literal placed after use").
class RotaryEncoder {
public:
  void begin(uint8_t pinA, uint8_t pinB);

  // returns whole UI steps since last call (positive = clockwise)
  int16_t consumeSteps(uint8_t divisor);

  static void isrTrampoline();
  static RotaryEncoder* _inst;

  volatile int32_t _count = 0;
  volatile uint8_t _qstate = 0;
  int32_t _consumed = 0;
  uint8_t _pa = 0, _pb = 0;
};

// debounced push button with click / long-press detection (active low)
enum class BtnEvent { NONE, CLICK, LONG_PRESS };

class PushButton {
public:
  void begin(uint8_t pin) {
    _pin = pin;
    pinMode(_pin, INPUT_PULLUP);
  }

  BtnEvent poll(uint32_t now) {
    bool raw = digitalRead(_pin) == LOW;
    if (raw != _lastRaw) {
      _lastRaw = raw;
      _lastEdge = now;
    }
    if (now - _lastEdge < 20) return BtnEvent::NONE; // debounce

    if (raw && !_stable) { // press
      _stable = true;
      _pressAt = now;
      _longFired = false;
    } else if (raw && _stable && !_longFired && now - _pressAt >= 550) {
      _longFired = true;
      return BtnEvent::LONG_PRESS;
    } else if (!raw && _stable) { // release
      _stable = false;
      if (!_longFired && now - _pressAt < 550) return BtnEvent::CLICK;
    }
    return BtnEvent::NONE;
  }

private:
  uint8_t _pin = 0;
  bool _lastRaw = false, _stable = false, _longFired = false;
  uint32_t _lastEdge = 0, _pressAt = 0;
};
