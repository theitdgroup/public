#include "settings.h"
#include <LittleFS.h>

Settings cfg;
SemaphoreHandle_t cfgMutex;

static const char* kPath = "/config.json";

static void applyDefaults() {
  cfg.stations = {
    {"SomaFM Groove Salad", "http://ice1.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Indie Pop Rocks", "http://ice1.somafm.com/indiepop-128-mp3"},
    {"SomaFM Drone Zone", "http://ice1.somafm.com/dronezone-128-mp3"},
    {"Radio Paradise", "http://stream.radioparadise.com/mp3-128"},
    {"KEXP Seattle", "https://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
  };
  cfg.feeds = {
    {"BBC News", "https://feeds.bbci.co.uk/news/rss.xml", true},
    {"NPR", "https://feeds.npr.org/1001/rss.xml", true},
    {"The Guardian", "https://www.theguardian.com/world/rss", true},
    {"Ars Technica", "https://feeds.arstechnica.com/arstechnica/index", false},
    {"Hacker News", "https://hnrss.org/frontpage", false},
  };
  cfg.galleries = {"/gallery/index.json"};
}

static void readSecret(JsonVariantConst v, String& dst) {
  if (v.isNull()) return;
  String s = v.as<String>();
  if (s != kSecretMask) dst = s;
}

void settingsFromJson(JsonVariantConst j) {
  if (j["wifi"].is<JsonArrayConst>()) {
    std::vector<WifiNet> nets;
    for (JsonVariantConst w : j["wifi"].as<JsonArrayConst>()) {
      WifiNet n;
      n.ssid = w["ssid"].as<String>();
      if (n.ssid.isEmpty()) continue;
      // Keep the stored password when the UI sends the mask back for an unchanged network
      for (auto& old : cfg.wifi) if (old.ssid == n.ssid) n.pass = old.pass;
      readSecret(w["pass"], n.pass);
      nets.push_back(n);
    }
    cfg.wifi = nets;
  }
  if (j["hostname"].is<const char*>()) cfg.hostname = j["hostname"].as<String>();
  if (j["tz"].is<const char*>())       cfg.tz = j["tz"].as<String>();
  if (j["tzName"].is<const char*>())   cfg.tzName = j["tzName"].as<String>();
  if (j["ntp"].is<const char*>())      cfg.ntp = j["ntp"].as<String>();
  if (j["h24"].is<bool>())             cfg.h24 = j["h24"];
  if (j["brightness"].is<int>())       cfg.brightness = constrain(j["brightness"].as<int>(), 5, 255);
  if (j["dimBrightness"].is<int>())    cfg.dimBrightness = constrain(j["dimBrightness"].as<int>(), 0, 255);
  if (j["dimAfter"].is<int>())         cfg.dimAfter = j["dimAfter"];
  if (j["offAfter"].is<int>())         cfg.offAfter = j["offAfter"];
  if (j["pixelShift"].is<bool>())      cfg.pixelShift = j["pixelShift"];
  if (j["face"].is<const char*>())     cfg.face = j["face"].as<String>();
  if (j["volume"].is<int>())           cfg.volume = constrain(j["volume"].as<int>(), 0, 21);
  if (j["stations"].is<JsonArrayConst>()) {
    cfg.stations.clear();
    for (JsonVariantConst s : j["stations"].as<JsonArrayConst>())
      if (!s["url"].as<String>().isEmpty())
        cfg.stations.push_back({s["name"].as<String>(), s["url"].as<String>()});
  }
  if (j["feeds"].is<JsonArrayConst>()) {
    cfg.feeds.clear();
    for (JsonVariantConst f : j["feeds"].as<JsonArrayConst>())
      if (!f["url"].as<String>().isEmpty())
        cfg.feeds.push_back({f["name"].as<String>(), f["url"].as<String>(), f["enabled"] | true});
  }
  if (j["topics"].is<JsonArrayConst>()) {
    cfg.topics.clear();
    for (JsonVariantConst t : j["topics"].as<JsonArrayConst>())
      if (!t.as<String>().isEmpty()) cfg.topics.push_back(t.as<String>());
  }
  if (j["newsRefreshMin"].is<int>()) cfg.newsRefreshMin = max(5, j["newsRefreshMin"].as<int>());
  if (j["encoder"].is<JsonObjectConst>()) {
    JsonVariantConst e = j["encoder"];
    // Only the header GPIOs (and -1 = unused) are allowed, so a typo can't grab the display or SD pins
    auto pin = [](JsonVariantConst v, int8_t cur) -> int8_t {
      if (!v.is<int>()) return cur;
      int p = v.as<int>();
      return (p == -1 || p == 16 || p == 17 || p == 18) ? p : cur;
    };
    if (e["enabled"].is<bool>()) cfg.encoderEnabled = e["enabled"];
    cfg.encoderA = pin(e["a"], cfg.encoderA);
    cfg.encoderB = pin(e["b"], cfg.encoderB);
    cfg.encoderBtn = pin(e["button"], cfg.encoderBtn);
    if (e["reverse"].is<bool>()) cfg.encoderReverse = e["reverse"];
    if (e["stepsPerDetent"].is<int>()) cfg.encoderStepsPerDetent = constrain(e["stepsPerDetent"].as<int>(), 1, 4);
  }
  auto hm = [](JsonVariantConst v, uint16_t cur) -> uint16_t {
    String s = v.as<String>();
    int c = s.indexOf(':');
    if (c < 1) return cur;
    return constrain(s.substring(0, c).toInt(), 0, 23) * 60 + constrain(s.substring(c + 1).toInt(), 0, 59);
  };
  if (j["weather"].is<JsonObjectConst>()) {
    JsonVariantConst w = j["weather"];
    if (w["lat"].is<float>() && w["lon"].is<float>()) { cfg.lat = w["lat"]; cfg.lon = w["lon"]; }
    if (w["place"].is<const char*>()) cfg.place = w["place"].as<String>();
    if (w["units"].is<const char*>()) cfg.fahrenheit = w["units"].as<String>() != "C";
  }
  if (j["alarms"].is<JsonArrayConst>()) {
    cfg.alarms.clear();
    for (JsonVariantConst a : j["alarms"].as<JsonArrayConst>()) {
      Alarm al;
      uint16_t t = hm(a["time"], 7 * 60);
      al.hour = t / 60;
      al.minute = t % 60;
      al.days = 0;
      for (JsonVariantConst d : a["days"].as<JsonArrayConst>()) al.days |= 1 << (d.as<int>() % 7);
      al.on = a["on"] | true;
      al.sound = a["sound"] | "reveille";
      if (al.sound != "chime" && al.sound != "radio") al.sound = "reveille";
      al.station = a["station"] | 0;
      al.label = a["label"] | "";
      if (cfg.alarms.size() < 10) cfg.alarms.push_back(al);
    }
  }
  if (j["chimeVolume"].is<int>()) cfg.chimeVolume = constrain(j["chimeVolume"].as<int>(), 1, 21);
  if (j["snoozeMin"].is<int>()) cfg.snoozeMin = constrain(j["snoozeMin"].as<int>(), 1, 30);
  if (j["night"].is<JsonObjectConst>()) {
    JsonVariantConst n = j["night"];
    if (n["on"].is<bool>()) cfg.nightOn = n["on"];
    cfg.nightStart = hm(n["start"], cfg.nightStart);
    cfg.nightEnd = hm(n["end"], cfg.nightEnd);
    if (n["brightness"].is<int>()) cfg.nightBrightness = constrain(n["brightness"].as<int>(), 0, 255);
  }
  if (j["countdowns"].is<JsonArrayConst>()) {
    cfg.countdowns.clear();
    for (JsonVariantConst c : j["countdowns"].as<JsonArrayConst>())
      if (String(c["date"] | "").length() == 10 && cfg.countdowns.size() < 10)
        cfg.countdowns.push_back({c["name"] | "", c["date"] | ""});
  }
  if (j["calendars"].is<JsonArrayConst>()) {
    std::vector<CalendarSrc> list;
    for (JsonVariantConst c : j["calendars"].as<JsonArrayConst>()) {
      CalendarSrc cs{c["name"] | "", c["url"] | ""};
      if (cs.url == kSecretMask) {  // unchanged: keep the stored link
        for (auto& old : cfg.calendars) if (old.name == cs.name) cs.url = old.url;
      }
      if (cs.url.startsWith("webcal://")) cs.url = "https://" + cs.url.substring(9);
      if (cs.url.startsWith("http") && list.size() < 6) list.push_back(cs);
    }
    cfg.calendars = list;
  }
  if (j["reminderMin"].is<int>()) cfg.reminderMin = constrain(j["reminderMin"].as<int>(), 0, 120);
  if (j["teams"].is<JsonArrayConst>()) {
    cfg.teams.clear();
    for (JsonVariantConst t : j["teams"].as<JsonArrayConst>())
      if (cfg.teams.size() < 8) cfg.teams.push_back({t["sport"] | "", t["league"] | "", t["id"] | "", t["abbr"] | "", t["name"] | ""});
  }
  if (j["sportsAlerts"].is<bool>()) cfg.sportsAlerts = j["sportsAlerts"];
  if (j["screens"].is<JsonArrayConst>()) {
    cfg.screens.clear();
    for (JsonVariantConst s : j["screens"].as<JsonArrayConst>()) cfg.screens.push_back(s.as<String>());
  }
  if (j["photos"].is<JsonObjectConst>()) {
    JsonVariantConst p = j["photos"];
    if (p["seconds"].is<int>()) cfg.photoSeconds = constrain(p["seconds"].as<int>(), 2, 3600);
    if (p["shuffle"].is<bool>()) cfg.photoShuffle = p["shuffle"];
    if (p["frame"].is<bool>()) cfg.photoFrame = p["frame"];
  }
  if (j["bible"].is<JsonObjectConst>()) {
    JsonVariantConst b = j["bible"];
    if (b["mode"].is<const char*>()) cfg.bibleRandom = String(b["mode"].as<const char*>()) == "random";
    if (b["minutes"].is<int>()) cfg.bibleMinutes = constrain(b["minutes"].as<int>(), 1, 1440);
    if (b["version"].is<const char*>()) cfg.bibleVersion = b["version"].as<String>();
  }
  if (j["galleries"].is<JsonArrayConst>()) {
    cfg.galleries.clear();
    for (JsonVariantConst g : j["galleries"].as<JsonArrayConst>())
      if (!g.as<String>().isEmpty()) cfg.galleries.push_back(g.as<String>());
  }
  readSecret(j["adminPass"], cfg.adminPass);
}

void settingsToJson(JsonDocument& doc, bool redact) {
  auto secret = [&](const String& s) { return (redact && !s.isEmpty()) ? String(kSecretMask) : s; };
  JsonArray w = doc["wifi"].to<JsonArray>();
  for (auto& n : cfg.wifi) { JsonObject o = w.add<JsonObject>(); o["ssid"] = n.ssid; o["pass"] = secret(n.pass); }
  doc["hostname"] = cfg.hostname;
  doc["tz"] = cfg.tz;
  doc["tzName"] = cfg.tzName;
  doc["ntp"] = cfg.ntp;
  doc["h24"] = cfg.h24;
  doc["brightness"] = cfg.brightness;
  doc["dimBrightness"] = cfg.dimBrightness;
  doc["dimAfter"] = cfg.dimAfter;
  doc["offAfter"] = cfg.offAfter;
  doc["pixelShift"] = cfg.pixelShift;
  doc["face"] = cfg.face;
  doc["volume"] = cfg.volume;
  JsonArray st = doc["stations"].to<JsonArray>();
  for (auto& s : cfg.stations) { JsonObject o = st.add<JsonObject>(); o["name"] = s.name; o["url"] = s.url; }
  JsonArray fd = doc["feeds"].to<JsonArray>();
  for (auto& f : cfg.feeds) { JsonObject o = fd.add<JsonObject>(); o["name"] = f.name; o["url"] = f.url; o["enabled"] = f.enabled; }
  JsonArray tp = doc["topics"].to<JsonArray>();
  for (auto& t : cfg.topics) tp.add(t);
  doc["newsRefreshMin"] = cfg.newsRefreshMin;
  JsonObject en = doc["encoder"].to<JsonObject>();
  en["enabled"] = cfg.encoderEnabled;
  en["a"] = cfg.encoderA;
  en["b"] = cfg.encoderB;
  en["button"] = cfg.encoderBtn;
  en["reverse"] = cfg.encoderReverse;
  en["stepsPerDetent"] = cfg.encoderStepsPerDetent;
  auto hm = [](uint16_t m) { char b[6]; snprintf(b, sizeof(b), "%02u:%02u", m / 60, m % 60); return String(b); };
  JsonObject we = doc["weather"].to<JsonObject>();
  if (!isnan(cfg.lat)) { we["lat"] = cfg.lat; we["lon"] = cfg.lon; }
  we["place"] = cfg.place;
  we["units"] = cfg.fahrenheit ? "F" : "C";
  JsonArray al = doc["alarms"].to<JsonArray>();
  for (auto& a : cfg.alarms) {
    JsonObject o = al.add<JsonObject>();
    o["time"] = hm(a.hour * 60 + a.minute);
    JsonArray d = o["days"].to<JsonArray>();
    for (int i = 0; i < 7; i++) if (a.days & (1 << i)) d.add(i);
    o["on"] = a.on;
    o["sound"] = a.sound;
    o["station"] = a.station;
    o["label"] = a.label;
  }
  doc["chimeVolume"] = cfg.chimeVolume;
  doc["snoozeMin"] = cfg.snoozeMin;
  JsonObject ni = doc["night"].to<JsonObject>();
  ni["on"] = cfg.nightOn;
  ni["start"] = hm(cfg.nightStart);
  ni["end"] = hm(cfg.nightEnd);
  ni["brightness"] = cfg.nightBrightness;
  JsonArray cd = doc["countdowns"].to<JsonArray>();
  for (auto& c : cfg.countdowns) { JsonObject o = cd.add<JsonObject>(); o["name"] = c.name; o["date"] = c.date; }
  JsonArray cl = doc["calendars"].to<JsonArray>();
  for (auto& c : cfg.calendars) { JsonObject o = cl.add<JsonObject>(); o["name"] = c.name; o["url"] = secret(c.url); }
  doc["reminderMin"] = cfg.reminderMin;
  JsonArray tm = doc["teams"].to<JsonArray>();
  for (auto& t : cfg.teams) {
    JsonObject o = tm.add<JsonObject>();
    o["sport"] = t.sport; o["league"] = t.league; o["id"] = t.id; o["abbr"] = t.abbr; o["name"] = t.name;
  }
  doc["sportsAlerts"] = cfg.sportsAlerts;
  JsonArray sc = doc["screens"].to<JsonArray>();
  for (auto& s : cfg.screens) sc.add(s);
  JsonObject ph = doc["photos"].to<JsonObject>();
  ph["seconds"] = cfg.photoSeconds;
  ph["shuffle"] = cfg.photoShuffle;
  ph["frame"] = cfg.photoFrame;
  JsonObject bi = doc["bible"].to<JsonObject>();
  bi["mode"] = cfg.bibleRandom ? "random" : "daily";
  bi["minutes"] = cfg.bibleMinutes;
  bi["version"] = cfg.bibleVersion;
  JsonArray g = doc["galleries"].to<JsonArray>();
  for (auto& u : cfg.galleries) g.add(u);
  doc["adminPass"] = secret(cfg.adminPass);
}

bool settingsSave() {
  JsonDocument doc;
  settingsToJson(doc, false);
  File f = LittleFS.open(kPath, "w");
  if (!f) return false;
  serializeJsonPretty(doc, f);
  f.close();
  return true;
}

static volatile bool savePending = false;
void settingsSaveLater() { savePending = true; }
void settingsService() {
  if (!savePending) return;
  savePending = false;
  CfgLock c;
  settingsSave();
}

void settingsInit() {
  cfgMutex = xSemaphoreCreateRecursiveMutex();
  applyDefaults();
  File f = LittleFS.open(kPath, "r");
  if (!f) { settingsSave(); return; }
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) { log_e("config.json invalid: %s", err.c_str()); return; }
  settingsFromJson(doc.as<JsonVariantConst>());
}
