#include "improv.h"
#include <WiFi.h>
#include <vector>
#include "net.h"
#include "settings.h"
#include "state.h"

#include "version.h"

namespace {

enum : uint8_t { TYPE_STATE = 1, TYPE_ERROR = 2, TYPE_RPC = 3, TYPE_RESULT = 4 };
enum : uint8_t { STATE_READY = 2, STATE_PROVISIONING = 3, STATE_PROVISIONED = 4 };
enum : uint8_t { ERR_NONE = 0, ERR_INVALID = 1, ERR_UNKNOWN_CMD = 2, ERR_CONNECT = 3 };
enum : uint8_t { CMD_WIFI = 1, CMD_STATE = 2, CMD_INFO = 3, CMD_SCAN = 4 };

const char kHeader[] = "IMPROV";
std::vector<uint8_t> rx;
bool pending = false;
String pendingSsid;
uint32_t pendingSince = 0;

void send(uint8_t type, const std::vector<uint8_t>& data) {
  std::vector<uint8_t> p(kHeader, kHeader + 6);
  p.push_back(1);  // protocol version
  p.push_back(type);
  p.push_back(data.size());
  p.insert(p.end(), data.begin(), data.end());
  uint8_t sum = 0;
  for (uint8_t b : p) sum += b;
  p.push_back(sum);
  p.push_back('\n');
  Serial.write(p.data(), p.size());
  Serial.flush();
}

void sendResult(uint8_t cmd, const std::vector<String>& strings) {
  std::vector<uint8_t> body;
  for (auto& s : strings) {
    body.push_back(s.length());
    body.insert(body.end(), s.c_str(), s.c_str() + s.length());
  }
  std::vector<uint8_t> d{cmd, (uint8_t)body.size()};
  d.insert(d.end(), body.begin(), body.end());
  send(TYPE_RESULT, d);
}

String deviceUrl() {
  StateLock l;
  return "http://" + net.ip + "/";
}

void handleRpc(const uint8_t* d, size_t len) {
  if (len < 2 || d[1] != len - 2) { send(TYPE_ERROR, {ERR_INVALID}); return; }
  uint8_t cmd = d[0];
  const uint8_t* arg = d + 2;
  size_t argLen = len - 2;
  switch (cmd) {
    case CMD_WIFI: {
      if (argLen < 1 || arg[0] + 2u > argLen) { send(TYPE_ERROR, {ERR_INVALID}); return; }
      String ssid((const char*)arg + 1, arg[0]);
      size_t pi = 1 + arg[0];
      String pass((const char*)arg + pi + 1, arg[pi]);
      {
        CfgLock c;
        std::vector<WifiNet> nets{{ssid, pass}};
        for (auto& n : cfg.wifi) if (n.ssid != ssid) nets.push_back(n);
        cfg.wifi = nets;
        settingsSave();
      }
      netReconnect();
      send(TYPE_ERROR, {ERR_NONE});
      send(TYPE_STATE, {STATE_PROVISIONING});
      pending = true;
      pendingSsid = ssid;
      pendingSince = millis();
      break;
    }
    case CMD_STATE: {
      bool connected;
      { StateLock l; connected = net.connected; }
      send(TYPE_STATE, {connected ? STATE_PROVISIONED : STATE_READY});
      if (connected) sendResult(CMD_STATE, {deviceUrl()});
      break;
    }
    case CMD_INFO: {
      String host;
      { CfgLock c; host = cfg.hostname; }
      sendResult(CMD_INFO, {"ClockHub", FW_VERSION, "ESP32-S3", host});
      break;
    }
    case CMD_SCAN: {
      int n = WiFi.scanNetworks();
      for (int i = 0; i < n; i++) {
        if (WiFi.SSID(i).isEmpty()) continue;
        sendResult(CMD_SCAN, {WiFi.SSID(i), String(WiFi.RSSI(i)), WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "NO" : "YES"});
      }
      WiFi.scanDelete();
      sendResult(CMD_SCAN, {});
      break;
    }
    default:
      send(TYPE_ERROR, {ERR_UNKNOWN_CMD});
  }
}

}  // namespace

bool improvFeed(uint8_t b) {
  size_t pos = rx.size();
  if (pos < 6) {
    if (b != (uint8_t)kHeader[pos]) {
      bool wasPacket = pos > 0;
      rx.clear();
      if (b == (uint8_t)kHeader[0]) { rx.push_back(b); return true; }
      return wasPacket;  // bytes of a broken header are dropped, not echoed to the console
    }
    rx.push_back(b);
    return true;
  }
  rx.push_back(b);
  if (rx.size() < 9) return true;
  size_t total = 9 + rx[8] + 1;  // header(6) ver type len + data + checksum
  if (rx.size() < total) return true;
  uint8_t sum = 0;
  for (size_t i = 0; i < total - 1; i++) sum += rx[i];
  if (sum == rx[total - 1] && rx[6] == 1 && rx[7] == TYPE_RPC) handleRpc(rx.data() + 9, rx[8]);
  else if (sum != rx[total - 1]) send(TYPE_ERROR, {ERR_INVALID});
  rx.clear();
  return true;
}

void improvLoop() {
  if (!pending) return;
  bool connected;
  // Ignore the old connection until the network task has switched to the new one
  { StateLock l; connected = net.connected && net.ssid == pendingSsid && millis() - pendingSince > 2000; }
  if (connected) {
    pending = false;
    send(TYPE_STATE, {STATE_PROVISIONED});
    sendResult(CMD_WIFI, {deviceUrl()});
  } else if (millis() - pendingSince > 30000) {
    pending = false;
    send(TYPE_ERROR, {ERR_CONNECT});
    send(TYPE_STATE, {STATE_READY});
  }
}
