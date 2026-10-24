#include "web.h"
#include <ArduinoJson.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <Update.h>
#include <WebServer.h>
#include <WiFi.h>
#include <vector>
#include "faces.h"
#include "photos.h"
#include "hal.h"
#include "net.h"
#include "news.h"
#include "bible.h"
#include "player.h"
#include "settings.h"
#include "state.h"
#include "storage.h"
#include "textutil.h"
#include "ui.h"
#include "webdav.h"
#include "weather.h"
#include "calendar.h"
#include "clockapps.h"
#include "sports.h"
#include "encoder.h"

#include "version.h"

static WebServer server(80);
static DNSServer dns;
static bool dnsRunning = false;
static File uploadFile;
static String uploadError;

static bool passwordOk() {
  String pass;
  { CfgLock c; pass = cfg.adminPass; }
  return pass.isEmpty() || server.authenticate("admin", pass.c_str());
}

// Changes must carry X-ClockHub: browsers won't send a custom header cross-site
// without a CORS preflight (which we never approve), so other websites can't
// drive the device through the user's browser.
static bool csrfOk() {
  return server.method() == HTTP_GET || server.header("X-ClockHub") == "1";
}

static bool allowed() { return passwordOk() && csrfOk(); }

static bool guard() {
  if (!passwordOk()) { server.requestAuthentication(BASIC_AUTH, "ClockHub"); return false; }
  if (!csrfOk()) { server.send(403, "text/plain", "Missing X-ClockHub header"); return false; }
  return true;
}

static void sendJson(JsonDocument& doc, int code = 200) {
  String out;
  serializeJson(doc, out);
  server.send(code, "application/json", out);
}

static void sendOk() { server.send(200, "application/json", "{\"ok\":true}"); }
static void sendErr(int code, const String& msg) {
  JsonDocument d;
  d["ok"] = false;
  d["error"] = msg;
  sendJson(d, code);
}

static bool readBody(JsonDocument& doc) {
  if (!server.hasArg("plain")) { sendErr(400, "Missing JSON body"); return false; }
  DeserializationError e = deserializeJson(doc, server.arg("plain"));
  if (e) { sendErr(400, String("Invalid JSON: ") + e.c_str()); return false; }
  return true;
}

static String safeFileName(const String& in) {
  String n = in.substring(in.lastIndexOf('/') + 1);
  n = n.substring(n.lastIndexOf('\\') + 1);
  String out;
  for (char c : n) if (isalnum(c) || c == '.' || c == '-' || c == '_' || c == ' ' || c == '(' || c == ')') out += c;
  if (out.startsWith(".")) out = "_" + out;
  return out.substring(0, 96);
}

// ---------- handlers ----------

static void handleStatus() {
  if (!guard()) return;
  JsonDocument d;
  d["version"] = FW_VERSION;
  d["build"] = FW_BUILD;  // set by tools/stamp_build.py on every build
  { extern bool halCodecOk; d["soundChip"] = halCodecOk; }
  {
    uint32_t n, avg, mx;
    faceAnimStats(n, avg, mx);
    if (n) { JsonObject a = d["animation"].to<JsonObject>(); a["frames"] = n; a["avgMs"] = avg; a["maxMs"] = mx;
      extern volatile uint32_t uiRenderSamples, uiRenderUs, uiFlushUs;
      if (uiRenderSamples) { a["redraws"] = uiRenderSamples; a["drawMs"] = uiRenderUs / uiRenderSamples / 1000; a["sendMs"] = uiFlushUs / uiRenderSamples / 1000; } }
  }
  d["uptime"] = millis() / 1000;
  {
    OnlineVerse v;
    if (bibleOnline(v)) d["verseOfDay"] = v.ref + " (" + v.version + ")";
  }
  {
    static const char* const why[] = {"unknown", "power on", "external pin", "software restart", "crash", "interrupt watchdog",
                                      "task watchdog", "other watchdog", "deep sleep", "brownout (power dip)", "SDIO", "USB", "JTAG", "eFuse", "power glitch", "CPU lockup"};
    int r = (int)esp_reset_reason();
    d["resetReason"] = r >= 0 && r < (int)(sizeof(why) / sizeof(why[0])) ? why[r] : "unknown";
  }
  d["heap"] = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  d["heapBlock"] = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  d["psram"] = ESP.getFreePsram();
  d["time"] = (long)time(nullptr);
  {
    StateLock l;
    JsonObject n = d["net"].to<JsonObject>();
    n["connected"] = net.connected;
    n["ap"] = net.apMode;
    n["ip"] = net.ip;
    n["ssid"] = net.ssid;
    n["rssi"] = net.rssi;
    n["timeSynced"] = net.timeSynced;
    n["wifiError"] = net.wifiError;
    JsonObject p = d["player"].to<JsonObject>();
    p["source"] = sourceName(player.source);
    p["playing"] = player.playing;
    p["busy"] = player.busy;
    p["title"] = player.title;
    p["artist"] = player.artist;
    p["album"] = player.album;
    p["station"] = player.station;
    p["error"] = player.error;
    p["volume"] = player.volume;
    p["posMs"] = player.posMs;
    p["durMs"] = player.durMs;
    p["stationIdx"] = player.stationIdx;
    p["trackIdx"] = player.trackIdx;
    p["trackCount"] = player.trackCount;
    p["shuffle"] = player.shuffle;
    p["sd"] = player.sdMounted;
    p["usbDrive"] = storageUsbActive();
    JsonObject pw = d["power"].to<JsonObject>();
    pw["battery"] = power.battery;
    pw["charging"] = power.charging;
    pw["percent"] = power.percent;
    d["newsCount"] = news.size();
    d["newsStatus"] = newsStatus;
  }
  { CfgLock c; d["face"] = cfg.face; }
  long kt, kp; int kl;
  encoderDebug(kt, kl, kp);
  d["knob"]["transitions"] = kt;
  d["knob"]["pins"] = kl;
  d["knob"]["presses"] = kp;
  d["screen"] = uiTile;
  sendJson(d);
}

static void handleGetConfig() {
  if (!guard()) return;
  JsonDocument d;
  { CfgLock c; settingsToJson(d, true); }
  sendJson(d);
}

static void handlePostConfig() {
  if (!guard()) return;
  JsonDocument in;
  if (!readBody(in)) return;
  bool wifiChanged = !in["wifi"].isNull() || !in["hostname"].isNull();
  bool newsChanged = !in["feeds"].isNull() || !in["topics"].isNull();
  bool faceChanged = !in["face"].isNull();
  {
    CfgLock c;
    settingsFromJson(in.as<JsonVariantConst>());
    if (!settingsSave()) { sendErr(500, "Could not save settings"); return; }
  }
  netApplyTime();
  if (wifiChanged) netReconnect();
  if (newsChanged) newsRefreshNow();
  if (!in["weather"].isNull()) weatherRefreshNow();
  if (!in["calendars"].isNull()) calendarRefreshNow();
  if (!in["teams"].isNull()) sportsRefreshNow();
  if (faceChanged) uiRequestFaceReload();
  if (!in["volume"].isNull()) playerSend(Cmd::Volume, in["volume"].as<int>());
  uiRequestSettingsApply();
  halPokeActivity();
  sendOk();
}

static void handleFaces() {
  if (!guard()) return;
  JsonDocument d;
  String active;
  { CfgLock c; active = cfg.face; }
  JsonArray a = d.to<JsonArray>();
  for (auto& f : faceList()) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = f.id;
    o["name"] = f.name;
    o["author"] = f.author;
    o["active"] = f.id == active;
  }
  sendJson(d);
}

static void handleSaveFace() {
  if (!guard()) return;
  JsonDocument face;
  if (!readBody(face)) return;
  String err;
  if (!faceValidate(face, err)) { sendErr(400, err); return; }
  String id = faceSafeId(server.hasArg("id") ? server.arg("id") : face["id"] | face["name"].as<String>());
  if (id.isEmpty()) { sendErr(400, "Face needs an id or name"); return; }
  face["id"] = id;
  LittleFS.mkdir("/faces");
  File f = LittleFS.open("/faces/" + id + ".json", "w");
  if (!f) { sendErr(500, "Could not write face (flash full?)"); return; }
  serializeJson(face, f);
  f.close();
  String active;
  { CfgLock c; active = cfg.face; }
  if (server.arg("activate") == "1" || id == active) {
    { CfgLock c; cfg.face = id; settingsSave(); }
    uiRequestFaceReload();
    halPokeActivity();
  }
  JsonDocument d;
  d["ok"] = true;
  d["id"] = id;
  sendJson(d);
}

static void handleDeleteFace() {
  if (!guard()) return;
  String id = faceSafeId(server.arg("id"));
  if (id.isEmpty() || !LittleFS.remove("/faces/" + id + ".json")) { sendErr(404, "No such face"); return; }
  // Imported faces keep their images as /faces/assets/<id>_*
  std::vector<String> owned;
  File dir = LittleFS.open("/faces/assets");
  while (dir) {
    File f = dir.openNextFile();
    if (!f) break;
    String n = f.name();
    f.close();
    if (n.startsWith(id + "_")) owned.push_back("/faces/assets/" + n);
  }
  for (auto& n : owned) LittleFS.remove(n);
  uiRequestFaceReload();  // falls back to the first face if the active one was removed
  sendOk();
}

static void handleActivateFace() {
  if (!guard()) return;
  String id = faceSafeId(server.arg("id"));
  if (!LittleFS.exists("/faces/" + id + ".json")) { sendErr(404, "No such face"); return; }
  { CfgLock c; cfg.face = id; settingsSave(); }
  uiRequestFaceReload();
  halPokeActivity();
  sendOk();
}

static void handlePlayer() {
  if (!guard()) return;
  JsonDocument in;
  if (!readBody(in)) return;
  String cmd = in["cmd"] | "";
  int arg = in["arg"] | 0;
  if (cmd == "source") {
    String s = in["arg"] | "";
    playerSend(Cmd::SetSource, (int)(s == "radio" ? Source::Radio : s == "sd" ? Source::SD : Source::None));
  } else if (cmd == "station") playerSend(Cmd::PlayStation, arg);
  else if (cmd == "track") playerSend(Cmd::PlayTrack, arg);
  else if (cmd == "url") playerSend(Cmd::PlayUrl, 0, in["url"] | "");
  else if (cmd == "toggle") playerSend(Cmd::Toggle);
  else if (cmd == "next") playerSend(Cmd::Next);
  else if (cmd == "prev") playerSend(Cmd::Prev);
  else if (cmd == "stop") playerSend(Cmd::Stop);
  else if (cmd == "volume") playerSend(Cmd::Volume, arg);
  else if (cmd == "shuffle") playerSend(Cmd::Shuffle, arg);
  else if (cmd == "rescan") playerSend(Cmd::Rescan);
  else if (cmd == "alert") playerSend(Cmd::AlertStart, arg ? arg : 12, String(in["sound"] | "") == "reveille" ? playerReveilleUrl() : "");
  else if (cmd == "alertstop") playerSend(Cmd::AlertStop);
  else { sendErr(400, "Unknown command"); return; }
  sendOk();
}

static void handleTracks() {
  if (!guard()) return;
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (auto& t : playerTracks()) a.add(t);
  sendJson(d);
}

static void handleNews() {
  if (!guard()) return;
  if (server.method() == HTTP_POST) { newsRefreshNow(); sendOk(); return; }
  JsonDocument d;
  JsonArray a = d["items"].to<JsonArray>();
  StateLock l;
  d["status"] = newsStatus;
  for (auto& n : news) {
    JsonObject o = a.add<JsonObject>();
    o["source"] = n.source;
    o["title"] = n.title;
    o["summary"] = n.summary;
    o["ts"] = (long)n.ts;
  }
  sendJson(d);
}

// Generic upload: target decides where the bytes go
enum class UpTarget { FaceAsset, Music, WebUi, StaticDir, Photo, MessagePhoto };

// Only the web page and gallery folders can be written this way (never config.json)
static String staticDir() {
  String d = server.arg("dir");
  while (d.endsWith("/")) d.remove(d.length() - 1);
  if (d.indexOf("..") >= 0 || !(d == "/www" || d.startsWith("/www/") || d == "/gallery" || d.startsWith("/gallery/") || d == "/sounds" || d == "/bible")) return "";
  return d;
}

static void mkdirs(const String& dir, fs::FS& fs = LittleFS) {
  for (int i = 1; i <= (int)dir.length(); i++)
    if (i == (int)dir.length() || dir[i] == '/') fs.mkdir(dir.substring(0, i));
}

// ---------- face gallery on the SD card ----------
// The gallery (every face you could install, ~7 MB) lives on the SD card when there is one,
// which keeps the 8 MB internal flash free for installed faces, settings and sounds.

static bool galleryOnSd(const String& dir) { return dir.startsWith("/gallery") && storageAvailable(); }

class GalleryHandler : public RequestHandler {
 public:
  bool canHandle(WebServer&, HTTPMethod m, const String& uri) override {
    return m == HTTP_GET && uri.startsWith("/gallery/") && uri.indexOf("..") < 0;
  }
  bool handle(WebServer& s, HTTPMethod, const String& uri) override {
    File f;
    if (storageAvailable() && SD_MMC.exists(uri)) f = SD_MMC.open(uri, FILE_READ);
    if (!f && LittleFS.exists(uri)) f = LittleFS.open(uri, "r");
    if (!f || f.isDirectory()) { s.send(404, "text/plain", "Not found"); return true; }
    String type = uri.endsWith(".json") ? "application/json" : uri.endsWith(".png") ? "image/png"
                : uri.endsWith(".jpg") ? "image/jpeg" : "application/octet-stream";
    s.sendHeader("Cache-Control", "no-cache");
    s.streamFile(f, type);
    f.close();
    return true;
  }
};

// Copy dir (and below) from flash to the card; true if every file arrived intact
static bool copyTree(const String& dir) {
  File d = LittleFS.open(dir);
  if (!d || !d.isDirectory()) return false;
  SD_MMC.mkdir(dir);
  static uint8_t buf[4096];
  bool ok = true;
  for (File f = d.openNextFile(); f && ok; f = d.openNextFile()) {
    String path = dir + "/" + String(f.name());
    if (f.isDirectory()) { f.close(); ok = copyTree(path); continue; }
    File out = SD_MMC.open(path, FILE_WRITE);
    if (!out) { f.close(); ok = false; break; }
    size_t n, total = 0;
    while ((n = f.read(buf, sizeof(buf))) > 0) { total += out.write(buf, n); vTaskDelay(1); }
    ok = total == f.size();
    out.close();
    f.close();
  }
  d.close();
  return ok;
}

static void removeTree(const String& dir) {
  File d = LittleFS.open(dir);
  if (!d) return;
  std::vector<String> files, dirs;
  for (File f = d.openNextFile(); f; f = d.openNextFile()) {
    (f.isDirectory() ? dirs : files).push_back(dir + "/" + String(f.name()));
    f.close();
  }
  d.close();
  for (auto& f : files) LittleFS.remove(f);
  for (auto& s : dirs) removeTree(s);
  LittleFS.rmdir(dir);
}

// One-time move after an install or update: flash /gallery -> card /gallery, then free the flash
static void galleryMigrateTask(void*) {
  for (int i = 0; i < 40 && !storageAvailable(); i++) vTaskDelay(pdMS_TO_TICKS(500));  // card is mounted by the audio task
  if (storageAvailable() && LittleFS.exists("/gallery/index.json")) {
    log_i("moving the face gallery to the SD card");
    if (copyTree("/gallery")) {
      removeTree("/gallery");
      log_i("gallery moved; flash used %u of %u", (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
    } else {
      log_w("gallery copy to SD failed; keeping it on flash");
    }
  }
  vTaskDelete(nullptr);
}

static void uploadInto(UpTarget target) {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadError = "";
    if (!allowed()) { uploadError = "Not authorised"; return; }
    String name = safeFileName(up.filename);
    if (name.isEmpty()) { uploadError = "Bad file name"; return; }
    if (target == UpTarget::FaceAsset) {
      LittleFS.mkdir("/faces");
      LittleFS.mkdir("/faces/assets");
      uploadFile = LittleFS.open("/faces/assets/" + name, "w");
    } else if (target == UpTarget::WebUi) {
      uploadFile = LittleFS.open("/www/" + name, "w");
    } else if (target == UpTarget::MessagePhoto) {
      uploadFile = LittleFS.open("/msg.jpg", "w");
    } else if (target == UpTarget::Photo) {
      bool sd = server.arg("to") == "sd";
      if (sd && !storageAvailable()) { uploadError = "No SD card"; return; }
      fs::FS& fs = sd ? (fs::FS&)SD_MMC : (fs::FS&)LittleFS;
      fs.mkdir("/photos");
      if (!sd && LittleFS.totalBytes() - LittleFS.usedBytes() < 200 * 1024) { uploadError = "The watch's memory is full. Delete some photos or faces, or use an SD card."; return; }
      uploadFile = fs.open("/photos/" + name, FILE_WRITE);
    } else if (target == UpTarget::StaticDir) {
      String dir = staticDir();
      if (dir.isEmpty()) { uploadError = "dir must be under /www or /gallery, or /sounds"; return; }
      if (galleryOnSd(dir)) {
        mkdirs(dir, SD_MMC);
        uploadFile = SD_MMC.open(dir + "/" + name, FILE_WRITE);
      } else {
        mkdirs(dir);
        uploadFile = LittleFS.open(dir + "/" + name, "w");
      }
    } else {
      if (!storageAvailable()) { uploadError = storageUsbActive() ? "SD card is in USB drive mode" : "No SD card"; return; }
      SD_MMC.mkdir("/music");
      uploadFile = SD_MMC.open("/music/" + name, FILE_WRITE);
    }
    if (!uploadFile) uploadError = "Could not create file";
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadFile && uploadFile.write(up.buf, up.currentSize) != up.currentSize) uploadError = "Storage full";
  } else if (up.status == UPLOAD_FILE_END || up.status == UPLOAD_FILE_ABORTED) {
    if (uploadFile) uploadFile.close();
    if (up.status == UPLOAD_FILE_ABORTED) uploadError = "Upload aborted";
  }
}

static void uploadDone() {
  if (!guard()) return;
  if (uploadError.length()) sendErr(500, uploadError);
  else sendOk();
}

static void handleOtaUpload() {
  HTTPUpload& up = server.upload();
  if (up.status == UPLOAD_FILE_START) {
    uploadError = "";
    if (!allowed()) { uploadError = "Not authorised"; return; }
    if (!Update.begin(UPDATE_SIZE_UNKNOWN, up.name == "fs" ? U_SPIFFS : U_FLASH)) uploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_WRITE) {
    if (uploadError.isEmpty() && Update.write(up.buf, up.currentSize) != up.currentSize) uploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_END) {
    if (uploadError.isEmpty() && !Update.end(true)) uploadError = Update.errorString();
  } else if (up.status == UPLOAD_FILE_ABORTED) {
    if (Update.isRunning()) Update.abort();
    uploadError = "Upload aborted";
  }
}

static void handleOtaDone() {
  if (!guard()) return;
  if (uploadError.length()) { sendErr(500, uploadError); return; }
  sendOk();
  delay(500);
  ESP.restart();
}

static void handleWifiScan() {
  if (!guard()) return;
  int n = WiFi.scanNetworks(false, false);
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : a) if (o["ssid"] == ssid) dup = true;
    if (dup) continue;  // same network on several access points
    JsonObject o = a.add<JsonObject>();
    o["ssid"] = ssid;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
  }
  WiFi.scanDelete();
  sendJson(d);
}

static void handleUsbDrive() {
  if (!guard()) return;
  JsonDocument in;
  if (!readBody(in)) return;
  if (in["on"] | false) {
    String err;
    if (!storageUsbStart(&err)) { sendErr(409, err); return; }
  } else {
    storageUsbStop();
  }
  sendOk();
}

static void handleKnobTest() {
  if (!guard()) return;
  int up, down;
  encoderWiringTest(up, down);
  JsonDocument d;
  d["pullup"] = up;
  d["pulldown"] = down;
  sendJson(d);
}

static void handlePhotos() {
  if (!guard()) return;
  if (server.method() == HTTP_DELETE) {
    if (!photosDelete(server.arg("sd") == "1", server.arg("name"))) { sendErr(404, "No such photo"); return; }
    sendOk();
    return;
  }
  JsonDocument d;
  JsonArray a = d["photos"].to<JsonArray>();
  for (auto& p : photosList()) {
    JsonObject o = a.add<JsonObject>();
    o["name"] = p.name;
    o["sd"] = p.sd;
    o["size"] = p.size;
  }
  JsonObject fl = d["flash"].to<JsonObject>();
  fl["total"] = LittleFS.totalBytes();
  fl["used"] = LittleFS.usedBytes();
  if (storageAvailable()) {
    JsonObject sd = d["sd"].to<JsonObject>();
    sd["total"] = (double)SD_MMC.totalBytes();
    sd["used"] = (double)SD_MMC.usedBytes();
  }
  sendJson(d);
}

// Photos on the SD card (flash photos are served statically from /photos/)
static void handlePhotoFile() {
  if (!guard()) return;
  String n = server.arg("name");
  if (!storageAvailable() || n.indexOf('/') >= 0 || n.indexOf("..") >= 0) { server.send(404, "text/plain", "Not found"); return; }
  File f = SD_MMC.open("/photos/" + n, FILE_READ);
  if (!f) { server.send(404, "text/plain", "Not found"); return; }
  server.streamFile(f, "image/jpeg");
  f.close();
}

// Message from the web page; an optional photo is uploaded first to /api/message/photo
static void handleMessage() {
  if (!guard()) return;
  JsonDocument in;
  if (!readBody(in)) return;
  String text = cleanText(in["text"] | "", 300);
  String from = cleanText(in["from"] | "", 30);
  bool photo = (in["photo"] | false) && LittleFS.exists("/msg.jpg");
  if (text.isEmpty() && !photo) { sendErr(400, "Type a message"); return; }
  appsPostMessage(from, text, photo);
  sendOk();
}

static void handleTeams() {
  if (!guard()) return;
  String sport = server.arg("sport"), league = server.arg("league");
  for (char c : sport + league) if (!isalnum(c) && c != '-' && c != '.') { sendErr(400, "Bad league"); return; }
  String list = sportsTeamList(sport, league);
  if (list.isEmpty()) { sendErr(502, "Couldn't get the team list (" + sportsLastError() + ")"); return; }
  server.send(200, "application/json", list);
}

// What the watch currently knows: weather, upcoming events, scores
static void handleToday() {
  if (!guard()) return;
  JsonDocument d;
  StateLock l;
  JsonObject w = d["weather"].to<JsonObject>();
  w["valid"] = weather.valid;
  w["temp"] = weather.temp;
  w["code"] = weather.code;
  w["text"] = weatherText(weather.code);
  w["error"] = weather.error;
  w["updated"] = (long)weather.updated;
  JsonArray ev = d["events"].to<JsonArray>();
  for (auto& e : events) {
    JsonObject o = ev.add<JsonObject>();
    o["start"] = (long)e.start;
    o["allDay"] = e.allDay;
    o["title"] = e.title;
    o["cal"] = e.cal;
  }
  d["calendarStatus"] = calendarStatus;
  JsonArray gs = d["games"].to<JsonArray>();
  for (auto& g : games) {
    JsonObject o = gs.add<JsonObject>();
    o["league"] = g.league; o["away"] = g.away; o["home"] = g.home;
    o["awayScore"] = g.awayScore; o["homeScore"] = g.homeScore; o["state"] = g.state; o["detail"] = g.detail;
  }
  sendJson(d);
}

static void handleReboot() {
  if (!guard()) return;
  sendOk();
  delay(300);
  ESP.restart();
}

static void handleNotFound() {
  // Captive portal: send phones on the setup hotspot to the web UI
  if (WiFi.getMode() & WIFI_AP && server.hostHeader() != WiFi.softAPIP().toString()) {
    server.sendHeader("Location", "http://" + WiFi.softAPIP().toString() + "/", true);
    server.send(302, "text/plain", "");
    return;
  }
  server.send(404, "text/plain", "Not found");
}

static void webTask(void*) {
  while (WiFi.getMode() == WIFI_OFF) vTaskDelay(pdMS_TO_TICKS(200));

  const char* headers[1 + kDavHeaderCount] = {"X-ClockHub"};
  for (size_t i = 0; i < kDavHeaderCount; i++) headers[i + 1] = kDavHeaders[i];
  server.collectHeaders(headers, 1 + kDavHeaderCount);
  // Network drive first, so the static file handler doesn't swallow /dav
  server.addHandler(webdavHandler(passwordOk));
  server.on("/", HTTP_GET, [] {
    if (!guard()) return;
    File f = LittleFS.open("/www/index.html", "r");
    if (!f) { server.send(500, "text/plain", "Web UI missing: run 'pio run -t uploadfs'"); return; }
    server.streamFile(f, "text/html");
    f.close();
  });
  // Alarm/timer chime, fetched by the audio task over loopback (no password: it's just a sound)
  for (const char* path : {"/sounds/chime.wav", "/sounds/reveille.wav", "/sounds/reveille.mp3"}) {
    server.on(path, HTTP_GET, [path] {
      File f = LittleFS.open(path, "r");
      if (!f) { server.send(404, "text/plain", "Not found"); return; }
      server.streamFile(f, String(path).endsWith(".mp3") ? "audio/mpeg" : "audio/wav");
      f.close();
    });
  }
  // A face's own sound clip (/faces/assets/<f>), labelled as audio so the player accepts it
  // (the static file handler would send .mp3 as application/octet-stream)
  server.on("/sounds/face", HTTP_GET, [] {
    String f = server.arg("f");
    if (f.isEmpty() || f.indexOf('/') >= 0 || f.indexOf("..") >= 0) { server.send(400, "text/plain", "Bad name"); return; }
    File file = LittleFS.open("/faces/assets/" + f, "r");
    if (!file) { server.send(404, "text/plain", "Not found"); return; }
    server.streamFile(file, f.endsWith(".wav") ? "audio/wav" : "audio/mpeg");
    file.close();
  });
  server.on("/api/status", HTTP_GET, handleStatus);
  server.on("/api/config", HTTP_GET, handleGetConfig);
  server.on("/api/config", HTTP_POST, handlePostConfig);
  server.on("/api/faces", HTTP_GET, handleFaces);
  server.on("/api/faces", HTTP_POST, handleSaveFace);
  server.on("/api/faces", HTTP_DELETE, handleDeleteFace);
  server.on("/api/faces/activate", HTTP_POST, handleActivateFace);
  server.on("/api/faces/asset", HTTP_POST, uploadDone, [] { uploadInto(UpTarget::FaceAsset); });
  server.on("/api/player", HTTP_POST, handlePlayer);
  server.on("/api/tracks", HTTP_GET, handleTracks);
  server.on("/api/music", HTTP_POST, [] { uploadDone(); playerSend(Cmd::Rescan); }, [] { uploadInto(UpTarget::Music); });
  server.on("/api/news", HTTP_ANY, handleNews);
  server.on("/api/ota", HTTP_POST, handleOtaDone, handleOtaUpload);
  server.on("/api/reboot", HTTP_POST, handleReboot);
  server.on("/api/knobtest", HTTP_GET, handleKnobTest);
  server.on("/api/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/api/usbdrive", HTTP_POST, handleUsbDrive);
  server.on("/api/photos", HTTP_GET, handlePhotos);
  server.on("/api/photos", HTTP_DELETE, handlePhotos);
  server.on("/api/photos", HTTP_POST, [] { uploadDone(); if (uploadError.isEmpty()) photosChanged(); }, [] { uploadInto(UpTarget::Photo); });
  server.on("/api/photo", HTTP_GET, handlePhotoFile);
  server.on("/api/message", HTTP_POST, handleMessage);
  server.on("/api/message/photo", HTTP_POST, uploadDone, [] { uploadInto(UpTarget::MessagePhoto); });
  server.on("/api/espn/teams", HTTP_GET, handleTeams);
  server.on("/api/today", HTTP_GET, handleToday);
  // Update web page files without re-flashing the file system (keeps settings and faces)
  server.on("/api/www", HTTP_POST, uploadDone, [] { uploadInto(UpTarget::WebUi); });
  // Update gallery/web files in place: POST /api/static?dir=/gallery/faces/assets
  server.on("/api/static", HTTP_POST, uploadDone, [] { uploadInto(UpTarget::StaticDir); });
  // Delete one gallery/web file: DELETE /api/static?path=/gallery/faces/assets/x.bin (from the card and from flash)
  server.on("/api/static", HTTP_DELETE, [] {
    if (!guard()) return;
    String path = server.arg("path");
    int slash = path.lastIndexOf('/');
    String dir = slash > 0 ? path.substring(0, slash) : "";
    if (path.indexOf("..") >= 0 || dir.isEmpty() ||
        !(dir == "/www" || dir.startsWith("/gallery") || dir == "/sounds" || dir == "/bible")) { sendErr(400, "Bad path"); return; }
    bool gone = false;
    if (storageAvailable() && SD_MMC.exists(path)) gone |= SD_MMC.remove(path);
    if (LittleFS.exists(path)) gone |= LittleFS.remove(path);
    if (!gone) { sendErr(404, "No such file"); return; }
    sendOk();
  });
  server.serveStatic("/faces/", LittleFS, "/faces/", "no-cache");
  server.addHandler(new GalleryHandler());
  server.serveStatic("/photos/", LittleFS, "/photos/", "max-age=86400");
  server.serveStatic("/", LittleFS, "/www/", "max-age=300");
  server.onNotFound(handleNotFound);
  server.begin();
  log_i("Web server started");
  xTaskCreatePinnedToCore(galleryMigrateTask, "gallery", 6144, nullptr, 0, nullptr, 1);

  for (;;) {
    bool ap = WiFi.getMode() & WIFI_AP;
    if (ap && !dnsRunning) { dnsRunning = dns.start(53, "*", WiFi.softAPIP()); }
    if (!ap && dnsRunning) { dns.stop(); dnsRunning = false; }
    if (dnsRunning) dns.processNextRequest();
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(2));
  }
}

void webInit() {
  // Core 1: the audio task (core 0, higher priority) busy-waits for the reply when it
  // fetches the alarm sounds from this server over loopback, so we must not share its core
  xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, nullptr, 1);
}
