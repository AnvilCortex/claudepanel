#pragma once
#include <Arduino.h>

#define MAX_SESSIONS 6
#define MAX_PRESETS  10

// session states as sent by the bridge
enum class SessState : uint8_t { IDLE, WORKING, PERM, NOTIFY, ENDED };

struct Session {
  bool used = false;
  char sid[44] = {0};
  char title[22] = {0};   // usually cwd basename
  char model[18] = {0};
  char tool[64] = {0};    // "Bash: npm test" style activity line
  SessState state = SessState::IDLE;
  uint8_t ctxPct = 0;     // context window usage 0-100
  float cost = 0;
  uint32_t lastSeen = 0;
};

struct PermRequest {
  bool active = false;
  char id[44] = {0};
  char sid[44] = {0};
  char tool[26] = {0};
  char detail[110] = {0};
  uint32_t expiresAt = 0;
  uint32_t totalMs = 60000;
};

struct Preset {
  char label[22] = {0};
  char text[140] = {0};
};

struct State {
  Session sessions[MAX_SESSIONS];
  int focused = -1;

  PermRequest perm;

  Preset presets[MAX_PRESETS];
  uint8_t presetCount = 0;

  uint8_t encDiv = 4;      // encoder transitions per UI step (4 = one detent)
  bool beep = false;       // chirp on permission request
  bool leds = false;       // WS2812 ring animations
  bool bridgeConnected = false;
  char ip[18] = {0};

  bool dirty = true;

  int sessionCount() const {
    int n = 0;
    for (auto& s : sessions) n += s.used;
    return n;
  }

  int findSession(const char* sid) {
    for (int i = 0; i < MAX_SESSIONS; i++)
      if (sessions[i].used && strcmp(sessions[i].sid, sid) == 0) return i;
    return -1;
  }

  Session* upsertSession(const char* sid) {
    int i = findSession(sid);
    if (i < 0) {
      for (int j = 0; j < MAX_SESSIONS; j++)
        if (!sessions[j].used) { i = j; break; }
      if (i < 0) return nullptr;
      sessions[i] = Session();
      sessions[i].used = true;
      strlcpy(sessions[i].sid, sid, sizeof(sessions[i].sid));
      if (focused < 0) focused = i;
    }
    return &sessions[i];
  }

  void removeSession(const char* sid) {
    int i = findSession(sid);
    if (i < 0) return;
    sessions[i].used = false;
    if (focused == i) focused = firstUsed();
    dirty = true;
  }

  int firstUsed() const {
    for (int i = 0; i < MAX_SESSIONS; i++)
      if (sessions[i].used) return i;
    return -1;
  }

  // move focus by n used slots (wraps)
  void moveFocus(int n) {
    int count = sessionCount();
    if (count == 0) { focused = -1; return; }
    int order[MAX_SESSIONS], m = 0, cur = 0;
    for (int i = 0; i < MAX_SESSIONS; i++)
      if (sessions[i].used) {
        if (i == focused) cur = m;
        order[m++] = i;
      }
    focused = order[((cur + n) % m + m) % m];
    dirty = true;
  }

  void pruneSessions(uint32_t now) {
    for (auto& s : sessions) {
      if (!s.used) continue;
      int32_t age = (int32_t)(now - s.lastSeen);
      if (age < 0) continue; // lastSeen stamped after caller's now snapshot
      if (age > (int32_t)(45UL * 60UL * 1000UL) || (s.state == SessState::ENDED && age > 15000)) {
        s.used = false;
        dirty = true;
      }
    }
    if (focused >= 0 && !sessions[focused].used) { focused = firstUsed(); dirty = true; }
  }
};
