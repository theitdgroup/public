#include "net.h"
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include "bible.h"
#include "news.h"
#include "weather.h"
#include "calendar.h"
#include "sports.h"
#include "settings.h"
#include "state.h"

static volatile bool reconnectRequested = false;
static volatile bool rtcSyncPending = false;
static bool mdnsStarted = false;

static void onTimeSync(struct timeval*) {
  rtcSyncPending = true;
  StateLock l;
  net.timeSynced = true;
  net.version++;
}

bool netTakeRtcSync() {
  if (!rtcSyncPending) return false;
  rtcSyncPending = false;
  return true;
}

void netApplyTime() {
  String tz, ntp;
  { CfgLock c; tz = cfg.tz; ntp = cfg.ntp; }
  setenv("TZ", tz.c_str(), 1);
  tzset();
  if (WiFi.isConnected()) configTzTime(tz.c_str(), ntp.c_str(), "time.google.com", "time.cloudflare.com");
}

void netReconnect() { reconnectRequested = true; }

static void startAP() {
  if (WiFi.getMode() & WIFI_AP) return;
  String name = "ClockHub-" + String((uint32_t)(ESP.getEfuseMac() >> 24) & 0xFFFF, HEX);
  name.toUpperCase();
  name.replace("CLOCKHUB", "ClockHub");
  WiFi.mode(WIFI_AP_STA);
  WiFi.softAP(name.c_str(), "clockhub");
  StateLock l;
  net.apMode = true;
  net.apName = name;
  net.ip = WiFi.softAPIP().toString();
  net.version++;
  log_i("Setup AP %s started", name.c_str());
}

static void stopAP() {
  if (!(WiFi.getMode() & WIFI_AP)) return;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  StateLock l;
  net.apMode = false;
  net.version++;
}

static void onConnected() {
  String host;
  { CfgLock c; host = cfg.hostname; }
  if (!mdnsStarted) {
    mdnsStarted = MDNS.begin(host.c_str());
    if (mdnsStarted) MDNS.addService("http", "tcp", 80);
  }
  netApplyTime();
  StateLock l;
  net.connected = true;
  net.ip = WiFi.localIP().toString();
  net.ssid = WiFi.SSID();
  net.rssi = WiFi.RSSI();
  net.version++;
}

static void setWifiError(const String& e) {
  StateLock l;
  if (net.wifiError != e) { net.wifiError = e; net.version++; }
}

// Join the first saved network (in list order) that is in range. The web page and
// installer put the newest network first, so a freshly added network always wins.
static bool connectSaved() {
  std::vector<WifiNet> nets;
  { CfgLock c; nets = cfg.wifi; }
  if (nets.empty()) return false;
  int found = WiFi.scanNetworks();
  String lastErr = "None of the saved networks are in range";
  for (auto& n : nets) {
    bool visible = false;
    for (int i = 0; i < found && !visible; i++) visible = WiFi.SSID(i) == n.ssid;
    if (!visible) continue;
    WiFi.begin(n.ssid.c_str(), n.pass.c_str());
    uint32_t start = millis();
    wl_status_t st;
    while ((st = WiFi.status()) != WL_CONNECTED && millis() - start < 15000) {
      if (st == WL_CONNECT_FAILED) break;
      vTaskDelay(pdMS_TO_TICKS(250));
    }
    if (WiFi.status() == WL_CONNECTED) {
      WiFi.scanDelete();
      setWifiError("");
      return true;
    }
    lastErr = "Couldn't join \"" + n.ssid + "\" - check the password";
    WiFi.disconnect();
  }
  WiFi.scanDelete();
  setWifiError(lastErr);
  return false;
}

static void netTask(void*) {
  String host;
  { CfgLock c; host = cfg.hostname; }
  WiFi.persistent(false);  // settings live in config.json; also keeps this task off flash
  WiFi.setHostname(host.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // lower latency for streaming
  sntp_set_time_sync_notification_cb(onTimeSync);
  netApplyTime();        // timezone even before Wi-Fi so the RTC time displays correctly

  size_t nets;
  { CfgLock c; nets = cfg.wifi.size(); }
  uint32_t disconnectedSince = millis();
  bool wasConnected = false;
  uint32_t lastRssi = 0, lastAttempt = 0, connectedAt = 0;

  for (;;) {
    if (reconnectRequested) {
      reconnectRequested = false;
      { CfgLock c; nets = cfg.wifi.size(); }
      WiFi.disconnect();
      for (int i = 0; i < 30 && WiFi.isConnected(); i++) vTaskDelay(pdMS_TO_TICKS(100));
      wasConnected = false;
      lastAttempt = 0;  // try the new settings right away, even with the setup hotspot on
      disconnectedSince = millis();
      StateLock l;
      net.connected = false;
      net.version++;
    }

    bool apOn = WiFi.getMode() & WIFI_AP;
    bool connected = WiFi.isConnected();
    // Scanning disturbs clients on the setup hotspot, so retry less often while it is up
    if (!connected && nets > 0 && (lastAttempt == 0 || millis() - lastAttempt > (apOn ? 60000u : 10000u))) {
      connected = connectSaved();
      lastAttempt = millis();
    }

    if (connected && !wasConnected) {
      onConnected();
      connectedAt = millis();
    }
    if (!connected && wasConnected) {
      disconnectedSince = millis();
      StateLock l;
      net.connected = false;
      net.version++;
    }
    wasConnected = connected;

    // No usable network for 30 s: open the setup hotspot
    if (!connected && (nets == 0 || millis() - disconnectedSince > 30000)) startAP();
    // Keep the hotspot for 2 minutes after joining Wi-Fi so the phone on it can read the new address
    if (connected && apOn && millis() - connectedAt > 120000) stopAP();

    if (connected) {
      if (millis() - lastRssi > 10000) {
        lastRssi = millis();
        StateLock l;
        net.rssi = WiFi.RSSI();
        net.version++;
      }
      weatherService();
      sportsService();
      calendarService();
      newsService();
      bibleService();
    }
    vTaskDelay(pdMS_TO_TICKS(connected ? 100 : 1000));
  }
}

void netInit() {
  // Start the radio here, on the main task (internal-RAM stack). On a brand-new board
  // the first start writes RF calibration to flash, which must not happen on the
  // network task because its stack lives in PSRAM (flash writes disable the PSRAM cache).
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  // Stack in PSRAM: internal RAM is reserved for TLS. This task must not write flash.
  xTaskCreatePinnedToCoreWithCaps(netTask, "net", 16384, nullptr, 1, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}
