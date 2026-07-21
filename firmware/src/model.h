#pragma once
#include <Arduino.h>

#define MAX_SESSIONS 6
#define MAX_PRESETS  10
#define MAX_NOTIFS        8
#define MAX_NOTIF_ACTIONS 3
#define MAX_NOTIF_ITEMS   6
#define MAX_NOTIF_POINTS  60

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

// ntfy-sourced notification. view selects the renderer; unused fields
// stay zeroed. Template views (non-CARD) replace by topic, cards stack.
enum class NotifView : uint8_t { CARD, GAUGE, LIST, GRAPH };

struct Notification {
  bool used = false;
  char id[26] = {0};
  char topic[22] = {0};
  NotifView view = NotifView::CARD;
  uint8_t priority = 3;   // ntfy 1-5
  uint32_t expiresAt = 0; // millis deadline, 0 = never
  uint32_t rcvdAt = 0;
  char title[42] = {0};
  char body[160] = {0};
  char icon[16] = {0};
  char unit[10] = {0};
  uint32_t color = 0;     // 0 = accent default
  float value = 0, vmax = 100;
  uint8_t nItems = 0;
  char itemL[MAX_NOTIF_ITEMS][18] = {{0}};
  char itemV[MAX_NOTIF_ITEMS][14] = {{0}};
  uint8_t nActs = 0;
  char acts[MAX_NOTIF_ACTIONS][16] = {{0}};
  uint8_t nPts = 0;
  float pts[MAX_NOTIF_POINTS] = {0};
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

  Notification notifs[MAX_NOTIFS];

  int notifCount() const {
    int n = 0;
    for (auto& x : notifs) n += x.used;
    return n;
  }

  int findNotif(const char* id) {
    for (int i = 0; i < MAX_NOTIFS; i++)
      if (notifs[i].used && strcmp(notifs[i].id, id) == 0) return i;
    return -1;
  }

  // template views act as a per-topic dashboard slot; cards stack.
  // Returns slot index, evicting expired-then-oldest-lowest-priority
  // when full. -1 only if every slot outranks the newcomer.
  int allocNotif(const char* id, const char* topic, NotifView v, uint32_t now,
                 uint8_t pr) {
    int i = findNotif(id);
    if (i >= 0) return i;
    if (v != NotifView::CARD)
      for (int j = 0; j < MAX_NOTIFS; j++)
        if (notifs[j].used && notifs[j].view == v && !strcmp(notifs[j].topic, topic))
          return j;
    for (int j = 0; j < MAX_NOTIFS; j++)
      if (!notifs[j].used) return j;
    int victim = -1;
    for (int j = 0; j < MAX_NOTIFS; j++) {
      if (notifs[j].expiresAt && (int32_t)(now - notifs[j].expiresAt) >= 0) return j;
      if (notifs[j].priority > pr) continue;  // never evict a higher-priority slot
      if (victim < 0 || notifs[j].priority < notifs[victim].priority ||
          (notifs[j].priority == notifs[victim].priority &&
           (int32_t)(notifs[j].rcvdAt - notifs[victim].rcvdAt) < 0))
        victim = j;
    }
    return victim;
  }

  // next used notification slot in direction dir (wraps); -1 when empty
  int stepNotif(int from, int dir) const {
    if (notifCount() == 0) return -1;
    int i = from;
    for (int k = 0; k < MAX_NOTIFS; k++) {
      i = (i + dir + MAX_NOTIFS) % MAX_NOTIFS;
      if (notifs[i].used) return i;
    }
    return -1;
  }

  void pruneNotifs(uint32_t now) {
    for (auto& x : notifs) {
      if (!x.used || !x.expiresAt) continue;
      if ((int32_t)(now - x.expiresAt) >= 0) {
        x.used = false;
        dirty = true;
      }
    }
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
