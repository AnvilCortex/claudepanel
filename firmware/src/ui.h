#pragma once
#include "lgfx_panel.h"
#include "model.h"

class Net;

enum class Screen { BOOT, PORTAL, STATUS, MENU, PERM, NOTIF };

class Ui {
public:
  Ui(LGFX& tft, State& st) : _tft(tft), _st(st) {}

  void begin();
  void attachNet(Net* n) { _net = n; }

  void showBoot(const char* msg);
  void showPortal(const char* ssid, const char* pass);
  void toStatus();
  void notifyPerm();
  void permCancelled();
  void notifyNew(int idx); // notification arrived; applies priority rules
  void toast(const char* msg);

  void onSteps(int16_t n);
  void onClick();
  void onLong();
  void tick(uint32_t now);

private:
  void render(uint32_t now);
  void renderStatus(uint32_t now);
  void renderMenu();
  void renderPerm(uint32_t now);
  void renderNotif(uint32_t now);
  void ringPct(float pct, uint32_t color, int r0 = 112, int r1 = 119);
  void wrapText(const char* text, int y, int lineH, int maxLines, int maxW);
  void dismissNotif(bool tellHub);
  // menu rows: [notifications?] + presets + Back
  int menuNotifRow() const { return _st.notifCount() > 0 ? 1 : 0; }
  int menuCount() const { return menuNotifRow() + _st.presetCount + 1; }

  LGFX& _tft;
  State& _st;
  Net* _net = nullptr;
  lgfx::LGFX_Sprite _spr{nullptr};
  lgfx::LovyanGFX* _g = nullptr;
  bool _sprOk = false;

  Screen _scr = Screen::BOOT;
  int _menuIdx = 0;
  int _permIdx = 1; // 0 Allow, 1 Pass, 2 Deny — default Pass
  int _notifIdx = -1; // slot shown on NOTIF screen
  int _actIdx = -1;   // -1 browsing stack, >=0 selecting an action
  float _spin = 0;
  uint32_t _lastRender = 0;
  char _toast[36] = {0};
  uint32_t _toastUntil = 0;
};
