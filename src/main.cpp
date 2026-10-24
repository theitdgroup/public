// ClockHub: clock faces, music (internet radio / SD card) and news for the
// Waveshare ESP32-S3-Touch-AMOLED-1.75.
//
// Tasks:  loop() on core 1  -> LVGL UI (only task that touches LVGL and, after setup, I2C)
//         audio  core 0     -> decoder + radio/SD playback
//         net    core 0     -> Wi-Fi, NTP, news feeds, weather, sports
//         web    core 0     -> HTTP server for the web UI / face gallery
#include <Arduino.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include "hal.h"
#include "improv.h"
#include "net.h"
#include "player.h"
#include "settings.h"
#include "state.h"
#include "storage.h"
#include "ui.h"
#include "web.h"

// USB serial console (used by tools/set_wifi.py):
//   cfg {json}   merge settings, same format as POST /api/config
//   status       print network state
static void serialConsole() {
  static String line;
  while (Serial.available()) {
    int b = Serial.read();
    if (improvFeed((uint8_t)b)) continue;  // browser installer Wi-Fi setup
    char c = (char)b;
    if (c != '\n' && c != '\r') { if (line.length() < 2048) line += c; continue; }
    if (line.startsWith("cfg ")) {
      JsonDocument doc;
      if (deserializeJson(doc, line.substring(4))) {
        Serial.println("ERR bad json");
      } else {
        { CfgLock l; settingsFromJson(doc.as<JsonVariantConst>()); settingsSave(); }
        netReconnect();
        netApplyTime();
        uiRequestSettingsApply();
        Serial.println("OK saved");
      }
    } else if (line == "status") {
      StateLock l;
      Serial.printf("STATUS connected=%d ap=%d ssid=%s ip=%s\n", net.connected, net.apMode, net.ssid.c_str(), net.ip.c_str());
    }
    line = "";
  }
}

void setup() {
  // Everyday allocations of 256 bytes or more (text, JSON, lists) go to PSRAM, keeping
  // internal RAM free and unfragmented for HTTPS. (The TLS buffers themselves must stay
  // internal: the crypto hardware's DMA can't use PSRAM in this framework build.)
  heap_caps_malloc_extmem_enable(256);
  storageInit();  // USB drive interface must exist before USB enumerates
  Serial.begin(115200);
  delay(200);
  log_i("ClockHub starting, PSRAM %u bytes free", ESP.getFreePsram());

  if (!LittleFS.begin(true)) log_e("LittleFS mount failed");
  stateInit();
  settingsInit();
  playerMakeChime();
  playerMakeReveille();

  halInit();
  uiInit();
  playerInit();
  netInit();
  webInit();
}

void loop() {
  serialConsole();
  improvLoop();
  uiLoop();
  delay(5);
}
