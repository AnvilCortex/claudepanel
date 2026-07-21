#pragma once
#include <Arduino.h>
#include "model.h"

class Ui;

class Net {
public:
  Net(State& s) : st(s) {}

  void begin(Ui* uiPtr); // blocks until WiFi associates
  void loop(uint32_t now);

  void sendPermResult(const char* id, const char* result);
  void sendInject(const char* sid, const char* text);
  void handleMsg(char* payload, size_t len);

  State& st;
  Ui* ui = nullptr;
};
