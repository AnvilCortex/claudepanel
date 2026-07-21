#include "encoder.h"

RotaryEncoder* RotaryEncoder::_inst = nullptr;

static const int8_t QTAB[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

void IRAM_ATTR RotaryEncoder::isrTrampoline() {
  RotaryEncoder* e = _inst;
  if (!e) return;
  e->_qstate = ((e->_qstate << 2) | (digitalRead(e->_pa) << 1) | digitalRead(e->_pb)) & 0x0F;
  e->_count += QTAB[e->_qstate];
}

void RotaryEncoder::begin(uint8_t pinA, uint8_t pinB) {
  _pa = pinA;
  _pb = pinB;
  pinMode(_pa, INPUT_PULLUP);
  pinMode(_pb, INPUT_PULLUP);
  _inst = this;
  attachInterrupt(digitalPinToInterrupt(_pa), isrTrampoline, CHANGE);
  attachInterrupt(digitalPinToInterrupt(_pb), isrTrampoline, CHANGE);
}

int16_t RotaryEncoder::consumeSteps(uint8_t divisor) {
  if (divisor == 0) divisor = 1;
  int32_t total = _count; // single-word read, atomic enough for UI use
  int32_t delta = total - _consumed;
  int16_t steps = delta / (int32_t)divisor;
  _consumed += (int32_t)steps * divisor;
  return steps;
}
