#pragma once
#include "lgfx_panel.h"
#include "model.h"

class Net;

enum class Screen { BOOT, PORTAL, STATUS, MENU, PERM };

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
  void ringPct(float pct, uint32_t color, int r0 = 112, int r1 = 119);
  void wrapText(const char* text, int y, int lineH, int maxLines, int maxW);
  int menuCount() const { return _st.presetCount + 1; } // + Back

  LGFX& _tft;
  State& _st;
  Net* _net = nullptr;
  lgfx::LGFX_Sprite _spr{nullptr};
  lgfx::LovyanGFX* _g = nullptr;
  bool _sprOk = false;

  Screen _scr = Screen::BOOT;
  int _menuIdx = 0;
  int _permIdx = 1; // 0 Allow, 1 Pass, 2 Deny — default Pass
  float _spin = 0;
  uint32_t _lastRender = 0;
  char _toast[36] = {0};
  uint32_t _toastUntil = 0;
};
