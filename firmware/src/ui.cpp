#include "ui.h"
#include "net.h"
#include "pins.h"
#include <math.h>

static constexpr uint32_t C_BG     = 0x000000;
static constexpr uint32_t C_ACCENT = 0xD97757; // claude orange
static constexpr uint32_t C_TEXT   = 0xEDEAE4;
static constexpr uint32_t C_DIM    = 0x8A857C;
static constexpr uint32_t C_RINGBG = 0x2A2622;
static constexpr uint32_t C_GREEN  = 0x4CAF50;
static constexpr uint32_t C_AMBER  = 0xFFB300;
static constexpr uint32_t C_RED    = 0xE5484D;
static constexpr uint32_t C_BLUE   = 0x4A90D9;

static const char* PERM_OPTS[3] = {"Allow", "Pass", "Deny"};
static char portalSsid[26] = {0};
static char portalPass[26] = {0};
static char bootMsg[40] = {0};

void Ui::begin() {
  _spr.setColorDepth(8);
  _sprOk = _spr.createSprite(240, 240) != nullptr;
  _g = _sprOk ? (lgfx::LovyanGFX*)&_spr : (lgfx::LovyanGFX*)&_tft;
  if (!_sprOk) Serial.println("ui: sprite alloc failed, drawing direct");
  _g->setTextDatum(textdatum_t::middle_center);
}

void Ui::showBoot(const char* msg) {
  strlcpy(bootMsg, msg, sizeof(bootMsg));
  _scr = Screen::BOOT;
  _st.dirty = true;
  render(millis());
}

void Ui::showPortal(const char* ssid, const char* pass) {
  strlcpy(portalSsid, ssid, sizeof(portalSsid));
  strlcpy(portalPass, pass, sizeof(portalPass));
  _scr = Screen::PORTAL;
  _st.dirty = true;
  render(millis());
}

void Ui::toStatus() {
  _scr = Screen::STATUS;
  _st.dirty = true;
}

void Ui::notifyPerm() {
  _permIdx = 1; // Pass
  _scr = Screen::PERM;
  _st.dirty = true;
}

void Ui::permCancelled() {
  if (_scr == Screen::PERM) toStatus();
  toast("resolved in terminal");
}

void Ui::notifyNew(int idx) {
  Notification& n = _st.notifs[idx];
  if (_scr == Screen::BOOT || _scr == Screen::PORTAL) return;
  if (_scr == Screen::PERM) return; // permission prompts always win
  if (n.priority >= 5 ||
      (n.priority == 4 && (_scr == Screen::STATUS || _scr == Screen::NOTIF))) {
    _notifIdx = idx;
    _actIdx = -1;
    _scr = Screen::NOTIF;
  }
  _st.dirty = true;
}

void Ui::dismissNotif(bool tellHub) {
  if (_notifIdx < 0 || !_st.notifs[_notifIdx].used) return;
  if (tellHub && _net) _net->sendNotifAction(_st.notifs[_notifIdx].id, 0, true);
  _st.notifs[_notifIdx].used = false;
  _actIdx = -1;
  _notifIdx = _st.stepNotif(_notifIdx, 1);
  if (_notifIdx < 0) toStatus();
  _st.dirty = true;
}

void Ui::toast(const char* msg) {
  strlcpy(_toast, msg, sizeof(_toast));
  _toastUntil = millis() + 1800;
  _st.dirty = true;
}

void Ui::onSteps(int16_t n) {
  switch (_scr) {
    case Screen::STATUS:
      _st.moveFocus(n);
      break;
    case Screen::MENU: {
      int i = _menuIdx + n;
      if (i < 0) i = 0;
      if (i > menuCount() - 1) i = menuCount() - 1;
      _menuIdx = i;
      _st.dirty = true;
      break;
    }
    case Screen::PERM: {
      int i = _permIdx + n;
      if (i < 0) i = 0;
      if (i > 2) i = 2;
      _permIdx = i;
      _st.dirty = true;
      break;
    }
    case Screen::NOTIF: {
      if (_notifIdx < 0) break;
      if (_actIdx >= 0) {
        int i = _actIdx + n;
        int hi = _st.notifs[_notifIdx].nActs - 1;
        if (i < 0) i = 0;
        if (i > hi) i = hi;
        _actIdx = i;
      } else {
        int dir = n > 0 ? 1 : -1;
        for (int k = 0; k < abs(n); k++) _notifIdx = _st.stepNotif(_notifIdx, dir);
      }
      _st.dirty = true;
      break;
    }
    default:
      break;
  }
}

void Ui::onClick() {
  switch (_scr) {
    case Screen::STATUS:
      _menuIdx = 0;
      _scr = Screen::MENU;
      _st.dirty = true;
      break;

    case Screen::MENU: {
      int nr = menuNotifRow();
      if (_menuIdx >= menuCount() - 1) { // Back
        toStatus();
      } else if (nr && _menuIdx == 0) {  // notifications
        _notifIdx = _st.stepNotif(MAX_NOTIFS - 1, 1);
        _actIdx = -1;
        if (_notifIdx >= 0) {
          _scr = Screen::NOTIF;
          _st.dirty = true;
        } else {
          toStatus();
        }
      } else if (_st.focused < 0) {
        toast("no session");
      } else if (!_st.bridgeConnected) {
        toast("bridge down");
      } else {
        if (_net) _net->sendInject(_st.sessions[_st.focused].sid, _st.presets[_menuIdx - nr].text);
        toStatus();
        toast("sent");
      }
      break;
    }

    case Screen::PERM: {
      const char* result = _permIdx == 0 ? "allow" : _permIdx == 2 ? "deny" : "pass";
      if (_net) _net->sendPermResult(_st.perm.id, result);
      _st.perm.active = false;
      toStatus();
      toast(_permIdx == 0 ? "allowed" : _permIdx == 2 ? "denied" : "passed");
      break;
    }
    case Screen::NOTIF: {
      if (_notifIdx < 0 || !_st.notifs[_notifIdx].used) {
        toStatus();
        break;
      }
      Notification& nf = _st.notifs[_notifIdx];
      if (_actIdx < 0) {
        if (nf.nActs > 0) {
          _actIdx = 0;
          _st.dirty = true;
        } else {
          dismissNotif(true);
        }
      } else {
        if (_net) _net->sendNotifAction(nf.id, _actIdx, false);
        _actIdx = -1;
        toast("action sent");
      }
      break;
    }
    default:
      break;
  }
}

void Ui::onLong() {
  switch (_scr) {
    case Screen::MENU:
      toStatus();
      break;
    case Screen::NOTIF:
      if (_actIdx >= 0) {
        _actIdx = -1;
        _st.dirty = true;
      } else {
        dismissNotif(true);
      }
      break;
    case Screen::PERM:
      if (_net) _net->sendPermResult(_st.perm.id, "pass");
      _st.perm.active = false;
      toStatus();
      toast("passed");
      break;
    case Screen::STATUS:
      toast(_st.ip[0] ? _st.ip : "no wifi");
      break;
    default:
      break;
  }
}

void Ui::tick(uint32_t now) {
  if (_st.perm.active && (int32_t)(now - _st.perm.expiresAt) >= 0) {
    _st.perm.active = false;
    if (_scr == Screen::PERM) toStatus();
  }
  if (_toastUntil && now >= _toastUntil) {
    _toastUntil = 0;
    _st.dirty = true;
  }
  if (_scr == Screen::NOTIF && (_notifIdx < 0 || !_st.notifs[_notifIdx].used)) {
    _notifIdx = _notifIdx < 0 ? _st.stepNotif(MAX_NOTIFS - 1, 1)
                              : _st.stepNotif(_notifIdx, 1);
    _actIdx = -1;
    if (_notifIdx < 0) toStatus();
    _st.dirty = true;
  }

  bool animating = false;
  if (_scr == Screen::STATUS && _st.focused >= 0 &&
      _st.sessions[_st.focused].state == SessState::WORKING)
    animating = true;
  if (_scr == Screen::PERM) animating = true;
  if (_scr == Screen::NOTIF && _notifIdx >= 0 && _st.notifs[_notifIdx].expiresAt)
    animating = true;

  if (_st.dirty || (animating && now - _lastRender >= 66)) {
    _spin += 16;
    if (_spin >= 360) _spin -= 360;
    render(now);
  }
}

// ---- rendering ----

void Ui::ringPct(float pct, uint32_t color, int r0, int r1) {
  _g->fillArc(120, 120, r0, r1, 0, 360, C_RINGBG);
  if (pct <= 0) return;
  if (pct > 1) pct = 1;
  float a1 = 270 + pct * 360.0f;
  if (a1 <= 360) {
    _g->fillArc(120, 120, r0, r1, 270, a1, color);
  } else {
    _g->fillArc(120, 120, r0, r1, 270, 360, color);
    _g->fillArc(120, 120, r0, r1, 0, a1 - 360, color);
  }
}

void Ui::wrapText(const char* text, int y, int lineH, int maxLines, int maxW) {
  char line[64] = {0};
  char word[48];
  int li = 0;
  const char* p = text;
  while (*p && li < maxLines) {
    // grab next word
    int wi = 0;
    while (*p && *p != ' ' && wi < (int)sizeof(word) - 1) word[wi++] = *p++;
    word[wi] = 0;
    while (*p == ' ') p++;

    char candidate[80];
    if (line[0]) snprintf(candidate, sizeof(candidate), "%s %s", line, word);
    else strlcpy(candidate, word, sizeof(candidate));

    if (_g->textWidth(candidate) <= maxW) {
      strlcpy(line, candidate, sizeof(line));
    } else {
      if (line[0]) {
        _g->drawString(line, 120, y + li * lineH);
        li++;
      }
      strlcpy(line, word, sizeof(line));
      // hard-truncate a single overlong word
      while (_g->textWidth(line) > maxW && strlen(line) > 1) line[strlen(line) - 1] = 0;
    }
  }
  if (line[0] && li < maxLines) _g->drawString(line, 120, y + li * lineH);
}

void Ui::render(uint32_t now) {
  _g->fillScreen(C_BG);
  switch (_scr) {
    case Screen::BOOT: {
      _g->setFont(&fonts::FreeSansBold12pt7b);
      _g->setTextColor(C_ACCENT);
      _g->drawString("ClaudePanel", 120, 100);
      _g->setFont(&fonts::Font2);
      _g->setTextColor(C_DIM);
      _g->drawString(bootMsg, 120, 140);
      break;
    }
    case Screen::PORTAL: {
      _g->setFont(&fonts::FreeSansBold9pt7b);
      _g->setTextColor(C_ACCENT);
      _g->drawString("WiFi SETUP", 120, 52);
      _g->setFont(&fonts::Font2);
      _g->setTextColor(C_DIM);
      _g->drawString("join network:", 120, 86);
      _g->setFont(&fonts::FreeSansBold9pt7b);
      _g->setTextColor(C_TEXT);
      _g->drawString(portalSsid, 120, 112);
      _g->setFont(&fonts::Font2);
      _g->setTextColor(C_DIM);
      char buf[40];
      snprintf(buf, sizeof(buf), "password: %s", portalPass);
      _g->drawString(buf, 120, 138);
      _g->drawString("then open 192.168.4.1", 120, 168);
      break;
    }
    case Screen::STATUS:
      renderStatus(now);
      break;
    case Screen::MENU:
      renderMenu();
      break;
    case Screen::PERM:
      renderPerm(now);
      break;
    case Screen::NOTIF:
      renderNotif(now);
      break;
  }

  // toast overlay
  if (_toastUntil && now < _toastUntil && _scr != Screen::BOOT && _scr != Screen::PORTAL) {
    _g->setFont(&fonts::Font2);
    int w = _g->textWidth(_toast) + 24;
    if (w > 200) w = 200;
    _g->fillRoundRect(120 - w / 2, 206, w, 24, 12, C_RINGBG);
    _g->setTextColor(C_TEXT);
    _g->drawString(_toast, 120, 218);
  }

  if (_sprOk) _spr.pushSprite(&_tft, 0, 0);
  _lastRender = now;
  _st.dirty = false;
}

void Ui::renderStatus(uint32_t now) {
  Session* s = _st.focused >= 0 ? &_st.sessions[_st.focused] : nullptr;

  // bridge indicator
  _g->fillCircle(120, 16, 3, _st.bridgeConnected ? C_GREEN : C_RED);

  // pending notification pill
  int nn = _st.notifCount();
  if (nn > 0) {
    char nb[6];
    snprintf(nb, sizeof(nb), "%d", nn);
    _g->fillRoundRect(136, 8, 22, 16, 8, C_RINGBG);
    _g->setFont(&fonts::Font2);
    _g->setTextColor(C_AMBER);
    _g->drawString(nb, 147, 16);
  }

  if (!s) {
    _g->setFont(&fonts::FreeSansBold9pt7b);
    _g->setTextColor(C_TEXT);
    _g->drawString("no sessions", 120, 104);
    _g->setFont(&fonts::Font2);
    _g->setTextColor(C_DIM);
    _g->drawString(_st.bridgeConnected ? "bridge connected" : "waiting for bridge", 120, 130);
    if (_st.ip[0]) _g->drawString(_st.ip, 120, 154);
    return;
  }

  // context usage ring
  uint32_t rc = s->ctxPct < 60 ? C_GREEN : s->ctxPct < 85 ? C_AMBER : C_RED;
  ringPct(s->ctxPct / 100.0f, rc);

  // spinner while working
  if (s->state == SessState::WORKING) {
    float a0 = _spin, a1 = _spin + 70;
    if (a1 <= 360) {
      _g->fillArc(120, 120, 102, 107, a0, a1, C_ACCENT);
    } else {
      _g->fillArc(120, 120, 102, 107, a0, 360, C_ACCENT);
      _g->fillArc(120, 120, 102, 107, 0, a1 - 360, C_ACCENT);
    }
  }

  const char* label;
  uint32_t lc;
  switch (s->state) {
    case SessState::WORKING: label = "WORKING";    lc = C_ACCENT; break;
    case SessState::PERM:    label = "PERMISSION"; lc = C_RED;    break;
    case SessState::NOTIFY:  label = "ATTENTION";  lc = C_BLUE;   break;
    case SessState::ENDED:   label = "DONE";       lc = C_DIM;    break;
    default:                 label = "READY";      lc = C_GREEN;  break;
  }
  _g->setFont(&fonts::FreeSansBold9pt7b);
  _g->setTextColor(lc);
  _g->drawString(label, 120, 48);

  _g->setFont(&fonts::FreeSansBold12pt7b);
  _g->setTextColor(C_TEXT);
  _g->drawString(s->title, 120, 82);

  _g->setFont(&fonts::Font2);
  _g->setTextColor(C_DIM);
  _g->drawString(s->model, 120, 106);

  if (s->tool[0]) {
    _g->setTextColor(C_TEXT);
    wrapText(s->tool, 134, 18, 2, 186);
  }

  char buf[36];
  if (s->cost > 0)
    snprintf(buf, sizeof(buf), "$%.2f   ctx %u%%", s->cost, s->ctxPct);
  else
    snprintf(buf, sizeof(buf), "ctx %u%%", s->ctxPct);
  _g->setTextColor(C_DIM);
  _g->drawString(buf, 120, 176);

  // session dots
  int n = _st.sessionCount();
  if (n > 1) {
    int drawn = 0;
    int x0 = 120 - (n - 1) * 7;
    for (int i = 0; i < MAX_SESSIONS; i++) {
      if (!_st.sessions[i].used) continue;
      if (i == _st.focused) _g->fillCircle(x0 + drawn * 14, 198, 4, C_ACCENT);
      else _g->fillCircle(x0 + drawn * 14, 198, 2, C_DIM);
      drawn++;
    }
  }
}

void Ui::renderMenu() {
  _g->setFont(&fonts::FreeSansBold9pt7b);
  _g->setTextColor(C_ACCENT);
  _g->drawString("PRESETS", 120, 40);

  int total = menuCount();
  int first = _menuIdx - 1;
  if (first < 0) first = 0;
  if (first > total - 3) first = total - 3;
  if (first < 0) first = 0;

  int nr = menuNotifRow();
  char nbuf[20];
  for (int row = 0; row < 3; row++) {
    int i = first + row;
    if (i >= total) break;
    int y = 92 + row * 36;
    const char* label;
    if (i >= total - 1) {
      label = "< back";
    } else if (nr && i == 0) {
      snprintf(nbuf, sizeof(nbuf), "notifs (%d)", _st.notifCount());
      label = nbuf;
    } else {
      label = _st.presets[i - nr].label;
    }
    _g->setFont(&fonts::FreeSansBold9pt7b);
    if (i == _menuIdx) {
      _g->fillRoundRect(28, y - 16, 184, 32, 16, C_ACCENT);
      _g->setTextColor(C_BG);
    } else {
      _g->setTextColor(C_DIM);
    }
    _g->drawString(label, 120, y);
  }

  _g->setFont(&fonts::Font2);
  _g->setTextColor(C_DIM);
  _g->drawString(_st.focused >= 0 ? _st.sessions[_st.focused].title : "no session", 120, 210);
}

void Ui::renderPerm(uint32_t now) {
  PermRequest& p = _st.perm;
  float remain = p.active ? (float)(int32_t)(p.expiresAt - now) / (float)p.totalMs : 0;
  if (remain < 0) remain = 0;
  ringPct(remain, C_RED);

  _g->setFont(&fonts::FreeSansBold9pt7b);
  _g->setTextColor(C_RED);
  _g->drawString("PERMISSION", 120, 46);

  _g->setFont(&fonts::FreeSansBold12pt7b);
  _g->setTextColor(C_TEXT);
  _g->drawString(p.tool, 120, 76);

  _g->setFont(&fonts::Font2);
  _g->setTextColor(C_DIM);
  wrapText(p.detail, 102, 18, 3, 184);

  for (int i = 0; i < 3; i++) {
    int xc = 56 + i * 64;
    _g->setFont(&fonts::Font2);
    if (i == _permIdx) {
      uint32_t bg = i == 0 ? C_GREEN : i == 2 ? C_RED : C_DIM;
      _g->fillRoundRect(xc - 29, 165, 58, 26, 13, bg);
      _g->setTextColor(C_BG);
    } else {
      _g->setTextColor(C_DIM);
    }
    _g->drawString(PERM_OPTS[i], xc, 178);
  }
}

void Ui::renderNotif(uint32_t now) {
  if (_notifIdx < 0 || !_st.notifs[_notifIdx].used) return;
  Notification& n = _st.notifs[_notifIdx];
  uint32_t ac = n.color ? n.color : (n.priority >= 4 ? C_RED : C_ACCENT);

  // ring: gauge value for GAUGE, remaining TTL (dim) otherwise
  if (n.view == NotifView::GAUGE) {
    float f = n.value / n.vmax;
    ringPct(f < 0 ? 0 : f, ac);
  } else if (n.expiresAt) {
    float total = (float)(n.expiresAt - n.rcvdAt);
    float rem = (float)(int32_t)(n.expiresAt - now);
    if (total > 0 && rem > 0) ringPct(rem / total, C_RINGBG, 116, 119);
  }

  // topic header
  _g->setFont(&fonts::Font2);
  _g->setTextColor(C_DIM);
  _g->drawString(n.topic[0] ? n.topic : "notification", 120, 28);

  char buf[48];
  switch (n.view) {
    case NotifView::CARD: {
      _g->setFont(&fonts::FreeSansBold12pt7b);
      _g->setTextColor(ac);
      wrapText(n.title, 58, 26, 2, 188);
      _g->setFont(&fonts::Font2);
      _g->setTextColor(C_TEXT);
      wrapText(n.body, 112, 18, 3, 196);
      break;
    }
    case NotifView::GAUGE: {
      _g->setFont(&fonts::FreeSansBold9pt7b);
      _g->setTextColor(C_DIM);
      _g->drawString(n.title, 120, 66);
      if (n.value == (int)n.value && fabsf(n.value) < 100000)
        snprintf(buf, sizeof(buf), "%d", (int)n.value);
      else
        snprintf(buf, sizeof(buf), "%.1f", n.value);
      _g->setFont(&fonts::FreeSansBold24pt7b);
      _g->setTextColor(C_TEXT);
      _g->drawString(buf, 120, 118);
      if (n.unit[0]) {
        _g->setFont(&fonts::FreeSansBold9pt7b);
        _g->setTextColor(C_DIM);
        _g->drawString(n.unit, 120, 152);
      }
      break;
    }
    case NotifView::LIST: {
      _g->setFont(&fonts::FreeSansBold9pt7b);
      _g->setTextColor(ac);
      _g->drawString(n.title, 120, 50);
      _g->setFont(&fonts::Font2);
      for (int i = 0; i < n.nItems; i++) {
        int y = 76 + i * 22;
        _g->setTextDatum(textdatum_t::middle_left);
        _g->setTextColor(C_DIM);
        _g->drawString(n.itemL[i], 46, y);
        _g->setTextDatum(textdatum_t::middle_right);
        _g->setTextColor(C_TEXT);
        _g->drawString(n.itemV[i], 196, y);
      }
      _g->setTextDatum(textdatum_t::middle_center);
      break;
    }
    case NotifView::GRAPH: {
      _g->setFont(&fonts::FreeSansBold9pt7b);
      _g->setTextColor(ac);
      _g->drawString(n.title, 120, 50);
      if (n.nPts >= 2) {
        float lo = n.pts[0], hi = n.pts[0];
        for (int i = 1; i < n.nPts; i++) {
          if (n.pts[i] < lo) lo = n.pts[i];
          if (n.pts[i] > hi) hi = n.pts[i];
        }
        if (hi - lo < 1e-6f) { hi += 1; lo -= 1; }
        const int x0 = 36, x1 = 204, y0 = 68, y1 = 150;
        for (int i = 1; i < n.nPts; i++) {
          int xa = x0 + (x1 - x0) * (i - 1) / (n.nPts - 1);
          int xb = x0 + (x1 - x0) * i / (n.nPts - 1);
          int ya = y1 - (int)((y1 - y0) * (n.pts[i - 1] - lo) / (hi - lo));
          int yb = y1 - (int)((y1 - y0) * (n.pts[i] - lo) / (hi - lo));
          _g->drawLine(xa, ya, xb, yb, ac);
          _g->drawLine(xa, ya + 1, xb, yb + 1, ac);
        }
        int xl = x1;
        int yl = y1 - (int)((y1 - y0) * (n.pts[n.nPts - 1] - lo) / (hi - lo));
        _g->fillCircle(xl, yl, 3, C_TEXT);
        float last = n.pts[n.nPts - 1];
        if (last == (int)last && fabsf(last) < 100000)
          snprintf(buf, sizeof(buf), "%d%s", (int)last, n.unit);
        else
          snprintf(buf, sizeof(buf), "%.1f%s", last, n.unit);
        _g->setFont(&fonts::FreeSansBold12pt7b);
        _g->setTextColor(C_TEXT);
        _g->drawString(buf, 120, 176);
      }
      break;
    }
  }

  // action chips when selecting
  if (_actIdx >= 0 && n.nActs > 0) {
    _g->setFont(&fonts::Font2);
    for (int i = 0; i < n.nActs; i++) {
      int xc = 120 + (int)((i - (n.nActs - 1) / 2.0f) * 66);
      if (i == _actIdx) {
        _g->fillRoundRect(xc - 30, 186, 60, 24, 12, ac);
        _g->setTextColor(C_BG);
      } else {
        _g->setTextColor(C_DIM);
      }
      _g->drawString(n.acts[i], xc, 198);
    }
  } else {
    _g->setFont(&fonts::Font2);
    _g->setTextColor(C_DIM);
    int total = _st.notifCount();
    if (n.nActs > 0) {
      _g->drawString("click: actions", 120, 192);
    }
    if (total > 1) {
      int pos = 0, seen = 0;
      for (int i = 0; i < MAX_NOTIFS; i++) {
        if (!_st.notifs[i].used) continue;
        seen++;
        if (i == _notifIdx) pos = seen;
      }
      snprintf(buf, sizeof(buf), "%d/%d", pos, total);
      _g->drawString(buf, 120, 212);
    }
  }
}
