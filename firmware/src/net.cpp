#include "net.h"
#include "ui.h"
#include "pins.h"
#include <WiFi.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <ArduinoJson.h>
#if __has_include("wifi_secrets.h")
#include "wifi_secrets.h"
#endif
#ifndef WIFI_SSID
#include <WiFiManager.h>
#endif

// The device dials OUT to the bridge over plain TCP (newline-delimited
// JSON). The earlier WebSocket-server approach wedged: the links2004 server
// stopped reading inbound frames after the handshake while writes kept
// working. A client-mode WiFiClient read loop we own avoids all of it.
#ifndef BRIDGE_HOST
#define BRIDGE_HOST "10.10.1.135"
#endif
#ifndef BRIDGE_PORT
#define BRIDGE_PORT 8788
#endif

static WiFiClient link_;
static Net* self = nullptr;
// last driver disconnect reason, reported in the heartbeat. Key codes:
// 2 auth expire, 4 assoc expire, 8 we left, 15 4-way handshake timeout,
// 200 beacon timeout (RF/power), 201 no AP found, 205 connection fail
static volatile uint8_t wifiDiscReason = 0;
static volatile uint16_t wifiDiscCount = 0;
static const char* AP_SSID = "ClaudePanel";
static const char* AP_PASS = "claudepanel";
static uint32_t lastConnAttempt = 0;
static char rxBuf[2048]; // graph/list notify payloads run ~2KB
static size_t rxLen = 0;

#ifndef DEVICE_NAME
#define DEVICE_NAME "claudepanel"
#endif

static void sendJson(JsonDocument& d) {
  if (!link_.connected()) return;
  String out;
  serializeJson(d, out);
  out += '\n';
  link_.print(out);
}

static void sendDbg(const char* fmt, ...) {
  char buf[150];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  Serial.println(buf);
  JsonDocument d;
  d["t"] = "dbg";
  d["m"] = buf;
  sendJson(d);
}

static void cpStr(char* dst, size_t n, JsonVariantConst v) {
  if (v.is<const char*>()) strlcpy(dst, v.as<const char*>(), n);
}

// RF survey: what does this antenna actually hear, and how loud?
static void dumpScan() {
  int n = WiFi.scanNetworks();
  Serial.printf("scan: %d networks\n", n);
  for (int i = 0; i < n && i < 15; i++)
    Serial.printf("  %-26s ch%2d %4ddBm %s\n", WiFi.SSID(i).c_str(),
                  WiFi.channel(i), WiFi.RSSI(i), WiFi.BSSIDstr(i).c_str());
  WiFi.scanDelete();
}

static SessState parseState(const char* s) {
  if (!strcmp(s, "working")) return SessState::WORKING;
  if (!strcmp(s, "perm")) return SessState::PERM;
  if (!strcmp(s, "notify")) return SessState::NOTIFY;
  if (!strcmp(s, "ended")) return SessState::ENDED;
  return SessState::IDLE;
}

void Net::begin(Ui* uiPtr) {
  self = this;
  ui = uiPtr;

  WiFi.mode(WIFI_STA);
  WiFi.onEvent([](WiFiEvent_t, WiFiEventInfo_t info) {
    wifiDiscReason = info.wifi_sta_disconnected.reason;
    wifiDiscCount = wifiDiscCount + 1;
    Serial.printf("wifi disconnect rsn=%u rssi=%d\n",
                  (unsigned)wifiDiscReason, (int)WiFi.RSSI());
  }, WiFiEvent_t::ARDUINO_EVENT_WIFI_STA_DISCONNECTED);
  // default WIFI_FAST_SCAN latches onto the first BSSID that answers —
  // with multiple APs broadcasting the SSID it regularly picks a -90dBm
  // one while a strong one sits nearby. Always scan all and take strongest.
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setSortMethod(WIFI_CONNECT_AP_BY_SIGNAL);

#ifdef WIFI_SSID
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t started = millis();
  int attempt = 1;
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    if (millis() - started > 25000) { // re-kick association
      attempt++;
      char msg[40];
      snprintf(msg, sizeof(msg), "wifi retry %d...", attempt);
      ui->showBoot(msg);
      WiFi.disconnect();
      delay(500);
      dumpScan();
      WiFi.begin(WIFI_SSID, WIFI_PASS);
      started = millis();
    }
  }
#else
  WiFiManager wm;
  wm.setConfigPortalTimeout(600);
  wm.setAPCallback([](WiFiManager*) {
    if (self && self->ui) self->ui->showPortal(AP_SSID, AP_PASS);
  });
  if (!wm.autoConnect(AP_SSID, AP_PASS)) {
    ui->showBoot("WiFi failed, rebooting");
    delay(3000);
    ESP.restart();
  }
#endif
  // must run after association or it silently fails to apply on this core;
  // modem sleep can stall inbound traffic on some APs
  WiFi.setSleep(false);
  strlcpy(st.ip, WiFi.localIP().toString().c_str(), sizeof(st.ip));
  Serial.printf("WiFi connected: %s -> bridge %s:%d\n", st.ip, BRIDGE_HOST, BRIDGE_PORT);

  MDNS.begin("claudepanel");
  ArduinoOTA.setHostname("claudepanel");
  ArduinoOTA.setPassword("claudepanel");
  ArduinoOTA.onStart([]() {
    if (self && self->ui) self->ui->showBoot("OTA update...");
  });
  ArduinoOTA.begin();
}

void Net::loop(uint32_t now) {
  ArduinoOTA.handle();

  // WiFi watchdog: setAutoReconnect alone has failed to re-associate on this
  // AP (device sat offline 30+ min). Re-kick association every 30s; if still
  // down after 5 min, reboot — the boot retry path always gets back on.
  static uint32_t wifiLostAt = 0, lastKick = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiLostAt) {
      wifiLostAt = now ? now : 1;
      st.ip[0] = 0;
      st.dirty = true;
      Serial.println("wifi lost");
    }
    if (now - wifiLostAt > 5UL * 60UL * 1000UL) {
      if (ui) ui->showBoot("wifi lost, rebooting");
      delay(500);
      ESP.restart();
    }
    if (now - lastKick >= 30000) {
      lastKick = now;
      WiFi.disconnect();
      delay(100);
#ifdef WIFI_SSID
      WiFi.begin(WIFI_SSID, WIFI_PASS);
#else
      WiFi.begin(); // stored NVS creds from the portal
#endif
    }
  } else {
    if (wifiLostAt) {
      wifiLostAt = 0;
      strlcpy(st.ip, WiFi.localIP().toString().c_str(), sizeof(st.ip));
      st.dirty = true;
      Serial.println("wifi recovered");
    }
    // the ESP32 never roams by itself: if it camped on a weak BSSID,
    // kick it off so the by-signal reconnect picks the strongest one.
    // Rate-limited so an all-APs-are-weak spot can't flap-loop.
    static uint32_t weakSince = 0, lastRoam = 0;
    int rssiNow = WiFi.RSSI();
    if (rssiNow > -85) {
      weakSince = 0;
    } else if (!weakSince) {
      weakSince = now ? now : 1;
    } else if (now - weakSince > 60000 && now - lastRoam > 300000) {
      lastRoam = now;
      weakSince = 0;
      Serial.printf("weak link (%ddBm), re-roaming\n", rssiNow);
      WiFi.disconnect();
      delay(100);
#ifdef WIFI_SSID
      WiFi.begin(WIFI_SSID, WIFI_PASS);
#else
      WiFi.begin();
#endif
    }
  }

  if (!link_.connected()) {
    if (st.bridgeConnected) {
      st.bridgeConnected = false;
      st.dirty = true;
      link_.stop();
      Serial.println("bridge link down");
    }
    if (now - lastConnAttempt >= 3000) {
      lastConnAttempt = now;
      if (link_.connect(BRIDGE_HOST, BRIDGE_PORT, 1500)) {
        link_.setNoDelay(true);
        st.bridgeConnected = true;
        st.dirty = true;
        rxLen = 0;
        JsonDocument d;
        d["t"] = "hello";
        d["fw"] = FW_VERSION;
        d["name"] = DEVICE_NAME;
        d["rst"] = (int)esp_reset_reason();
        d["up"] = now / 1000;
        sendJson(d);
        Serial.println("bridge link up");
      }
    }
    return;
  }

  while (link_.available()) {
    char c = link_.read();
    if (c == '\n') {
      rxBuf[rxLen] = 0;
      if (rxLen) handleMsg(rxBuf, rxLen);
      rxLen = 0;
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = (char)c;
    } else {
      rxLen = 0; // overlong line, drop it
    }
  }

  static uint32_t lastHb = 0;
  if (now - lastHb >= 10000) {
    lastHb = now;
    sendDbg("hb sessions=%d focused=%d heap=%u minheap=%u up=%lus rssi=%d drops=%u rsn=%u",
            st.sessionCount(), st.focused, (unsigned)ESP.getFreeHeap(),
            (unsigned)ESP.getMinFreeHeap(), (unsigned long)(now / 1000),
            (int)WiFi.RSSI(), (unsigned)wifiDiscCount, (unsigned)wifiDiscReason);
  }
}

void Net::handleMsg(char* payload, size_t len) {
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload, len);
  if (err) {
    sendDbg("parse error: %s (len=%u)", err.c_str(), (unsigned)len);
    return;
  }
  const char* t = doc["t"] | "";

  if (!strcmp(t, "status")) {
    const char* sid = doc["sid"] | "";
    if (!*sid) return;
    Session* s = st.upsertSession(sid);
    if (!s) return;
    cpStr(s->title, sizeof(s->title), doc["title"]);
    cpStr(s->model, sizeof(s->model), doc["model"]);
    cpStr(s->tool, sizeof(s->tool), doc["tool"]);
    if (doc["state"].is<const char*>()) s->state = parseState(doc["state"]);
    s->ctxPct = doc["ctx"] | s->ctxPct;
    s->cost = doc["cost"] | s->cost;
    s->lastSeen = millis();
    st.dirty = true;

  } else if (!strcmp(t, "remove")) {
    st.removeSession(doc["sid"] | "");

  } else if (!strcmp(t, "perm")) {
    PermRequest& p = st.perm;
    cpStr(p.id, sizeof(p.id), doc["id"]);
    cpStr(p.sid, sizeof(p.sid), doc["sid"]);
    cpStr(p.tool, sizeof(p.tool), doc["tool"]);
    cpStr(p.detail, sizeof(p.detail), doc["detail"]);
    uint32_t timeoutS = doc["timeout"] | 60;
    p.totalMs = timeoutS * 1000UL;
    p.expiresAt = millis() + p.totalMs;
    p.active = true;
    if (ui) ui->notifyPerm();

  } else if (!strcmp(t, "perm_cancel")) {
    if (st.perm.active && !strcmp(st.perm.id, doc["id"] | "")) {
      st.perm.active = false;
      if (ui) ui->permCancelled();
    }

  } else if (!strcmp(t, "presets")) {
    uint8_t n = 0;
    for (JsonObjectConst it : doc["items"].as<JsonArrayConst>()) {
      if (n >= MAX_PRESETS) break;
      cpStr(st.presets[n].label, sizeof(st.presets[n].label), it["label"]);
      cpStr(st.presets[n].text, sizeof(st.presets[n].text), it["text"]);
      n++;
    }
    st.presetCount = n;
    st.dirty = true;

  } else if (!strcmp(t, "notify")) {
    const char* id = doc["id"] | "";
    if (!*id) return;
    const char* vs = doc["view"] | "card";
    NotifView v = !strcmp(vs, "gauge") ? NotifView::GAUGE
                : !strcmp(vs, "list")  ? NotifView::LIST
                : !strcmp(vs, "graph") ? NotifView::GRAPH
                : NotifView::CARD;
    uint32_t now = millis();
    int idx = st.allocNotif(id, doc["topic"] | "", v, now);
    if (idx < 0) { sendDbg("notify dropped (stack full): %s", id); return; }
    Notification& nf = st.notifs[idx];
    nf = Notification();
    nf.used = true;
    nf.view = v;
    nf.rcvdAt = now;
    strlcpy(nf.id, id, sizeof(nf.id));
    cpStr(nf.topic, sizeof(nf.topic), doc["topic"]);
    int pr = doc["priority"] | 3;
    nf.priority = pr < 1 ? 1 : pr > 5 ? 5 : pr;
    uint32_t ttl = doc["ttl"] | 1800;
    nf.expiresAt = ttl ? now + ttl * 1000UL : 0;
    cpStr(nf.title, sizeof(nf.title), doc["title"]);
    cpStr(nf.body, sizeof(nf.body), doc["body"]);
    cpStr(nf.icon, sizeof(nf.icon), doc["icon"]);
    cpStr(nf.unit, sizeof(nf.unit), doc["unit"]);
    if (doc["label"].is<const char*>() && !nf.title[0])
      cpStr(nf.title, sizeof(nf.title), doc["label"]);
    if (doc["color"].is<const char*>()) {
      const char* c = doc["color"].as<const char*>();
      if (*c == '#') c++;
      nf.color = (uint32_t)strtoul(c, nullptr, 16);
    }
    nf.value = doc["value"] | 0.0f;
    nf.vmax = doc["max"] | 100.0f;
    if (nf.vmax <= 0) nf.vmax = 100;
    for (JsonObjectConst it : doc["items"].as<JsonArrayConst>()) {
      if (nf.nItems >= MAX_NOTIF_ITEMS) break;
      cpStr(nf.itemL[nf.nItems], sizeof(nf.itemL[0]), it["label"]);
      if (it["value"].is<const char*>())
        cpStr(nf.itemV[nf.nItems], sizeof(nf.itemV[0]), it["value"]);
      else if (it["value"].is<float>())
        snprintf(nf.itemV[nf.nItems], sizeof(nf.itemV[0]), "%g", it["value"].as<float>());
      nf.nItems++;
    }
    for (JsonObjectConst a : doc["actions"].as<JsonArrayConst>()) {
      if (nf.nActs >= MAX_NOTIF_ACTIONS) break;
      cpStr(nf.acts[nf.nActs], sizeof(nf.acts[0]), a["label"]);
      nf.nActs++;
    }
    for (JsonVariantConst p : doc["points"].as<JsonArrayConst>()) {
      if (nf.nPts >= MAX_NOTIF_POINTS) break;
      nf.pts[nf.nPts++] = p.as<float>();
    }
    st.dirty = true;
    if (ui) ui->notifyNew(idx);

  } else if (!strcmp(t, "notify_del")) {
    int idx = st.findNotif(doc["id"] | "");
    if (idx >= 0) {
      st.notifs[idx].used = false;
      st.dirty = true;
    }

  } else if (!strcmp(t, "toast")) {
    if (ui && doc["m"].is<const char*>()) ui->toast(doc["m"].as<const char*>());

  } else if (!strcmp(t, "cfg")) {
    st.encDiv = doc["enc_div"] | st.encDiv;
    st.beep = doc["beep"] | st.beep;
    st.leds = doc["leds"] | st.leds;
    st.dirty = true;
  }
}

void Net::sendNotifAction(const char* id, int action, bool dismiss) {
  JsonDocument d;
  d["t"] = "notify_action";
  d["id"] = id;
  if (dismiss) d["dismiss"] = true;
  else d["action"] = action;
  sendJson(d);
}

void Net::sendPermResult(const char* id, const char* result) {
  JsonDocument d;
  d["t"] = "perm_result";
  d["id"] = id;
  d["result"] = result;
  sendJson(d);
}

void Net::sendInject(const char* sid, const char* text) {
  JsonDocument d;
  d["t"] = "inject";
  d["sid"] = sid;
  d["text"] = text;
  sendJson(d);
}
