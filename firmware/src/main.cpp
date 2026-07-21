#include <Arduino.h>
#include "pins.h"
#include "lgfx_panel.h"
#include "leds.h"
#include "encoder.h"
#include "model.h"
#include "ui.h"
#include "net.h"

LGFX tft;
State state;
Ui ui(tft, state);
Net net(state);
RotaryEncoder enc;
PushButton btn;

static void backlightInit() {
  ledcAttach(PIN_BACKLIGHT, 5000, 8); // core 3.x pin-based LEDC API
  ledcWrite(PIN_BACKLIGHT, 230);
}

static leds::Mode aggregateLedMode() {
  if (!state.leds) return leds::Mode::OFF;
  if (state.perm.active) return leds::Mode::PERM;
  bool working = false, notify = false, any = false;
  for (auto& s : state.sessions) {
    if (!s.used) continue;
    any = true;
    if (s.state == SessState::WORKING) working = true;
    if (s.state == SessState::NOTIFY || s.state == SessState::PERM) notify = true;
  }
  if (notify) return leds::Mode::NOTIFY;
  if (working) return leds::Mode::WORKING;
  return any ? leds::Mode::IDLE : leds::Mode::OFF;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  Serial.println("claudepanel " FW_VERSION);

  // power rails for panel + LED ring (vendor demo drives these before init)
  pinMode(PIN_PWR1, OUTPUT);
  digitalWrite(PIN_PWR1, HIGH);
  pinMode(PIN_PWR2, OUTPUT);
  digitalWrite(PIN_PWR2, HIGH);

  leds::init();
  tft.init();
  ui.begin();
  ui.attachNet(&net);
  ui.showBoot("connecting wifi...");
  delay(100);
  backlightInit();

  enc.begin(PIN_ENC_A, PIN_ENC_B);
  btn.begin(PIN_ENC_SW);

  net.begin(&ui); // blocks in captive portal on first boot
  ui.toStatus();
}

void loop() {
  uint32_t now = millis();
  net.loop(now);

  int16_t steps = enc.consumeSteps(state.encDiv);
  if (steps) ui.onSteps(steps);

  BtnEvent e = btn.poll(now);
  if (e == BtnEvent::CLICK) ui.onClick();
  else if (e == BtnEvent::LONG_PRESS) ui.onLong();

  leds::set(aggregateLedMode());
  leds::tick(now);
  state.pruneSessions(now);
  ui.tick(now);
  delay(2);
}
