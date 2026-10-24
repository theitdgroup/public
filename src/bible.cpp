#include "bible.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <LittleFS.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include "settings.h"
#include "state.h"
#include "textutil.h"
#include <time.h>
#include <vector>

// The whole file lives in PSRAM; each line is split in place into "ref\0text\0"
static char* buf = nullptr;
static std::vector<BibleVerse> verses;
static bool loaded = false;

static void load() {
  loaded = true;
  File f = LittleFS.open("/bible/verses.txt", "r");
  if (!f) { log_w("no /bible/verses.txt"); return; }
  size_t len = f.size();
  buf = (char*)ps_malloc(len + 1);
  if (!buf) { f.close(); return; }
  len = f.read((uint8_t*)buf, len);
  f.close();
  buf[len] = 0;
  char* p = buf;
  while (*p) {
    char* nl = strchr(p, '\n');
    if (nl) *nl = 0;
    char* tab = strchr(p, '\t');
    if (tab) { *tab = 0; verses.push_back({p, tab + 1}); }
    if (!nl) break;
    p = nl + 1;
  }
  log_i("bible: %u verses", (unsigned)verses.size());
}

int bibleCount() {
  if (!loaded) load();
  return verses.size();
}

BibleVerse bibleGet(int i) {
  int n = bibleCount();
  if (!n) return {"", ""};
  return verses[((i % n) + n) % n];
}

int bibleToday() {
  time_t now = time(nullptr);
  if (now < 1700000000) return -1;
  struct tm t;
  localtime_r(&now, &t);
  return t.tm_year * 366 + t.tm_yday;
}

int bibleDailyIndex() {
  int n = bibleCount(), day = bibleToday();
  if (!n || day < 0) return -1;
  // Step through the list by a large prime so consecutive days jump around the Bible
  return (int)(((uint32_t)day * 7919UL) % n);
}

// ---------- online Verse of the Day ----------

static OnlineVerse online;            // guarded by StateLock
static uint32_t onlineVersion = 0;
static uint32_t nextFetch = 0;
static String fetchedVersion;          // translation of the verse we have

void bibleRefreshNow() { nextFetch = 0; }
uint32_t bibleOnlineVersion() { return onlineVersion; }

bool bibleOnline(OnlineVerse& out) {
  int day = bibleToday();
  StateLock l;
  if (online.day < 0 || online.day != day) return false;
  out = online;
  return true;
}

static bool fetchVotd(const String& version) {
  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.setTimeout(10000);
  http.setUserAgent("Mozilla/5.0 (ClockHub ESP32)");
  if (!http.begin(tls, "https://www.biblegateway.com/votd/get/?format=json&version=" + urlEncode(version))) return false;
  int code = http.GET();
  if (code != 200) { log_w("votd -> %d", code); http.end(); return false; }
  String body = http.getString();
  http.end();
  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;
  JsonObject v = doc["votd"];
  String text = cleanText(v["text"].as<String>());
  // The text comes wrapped in quotation marks; the screen doesn't need them
  text.trim();
  while (text.startsWith("\"")) text.remove(0, 1);
  while (text.endsWith("\"")) text.remove(text.length() - 1);
  text.trim();
  String ref = cleanText(v["display_ref"].as<String>());
  if (text.isEmpty() || ref.isEmpty()) return false;
  StateLock l;
  online.text = text;
  online.ref = ref;
  online.version = v["version_id"].as<String>();
  online.day = bibleToday();
  onlineVersion++;
  return true;
}

void bibleService() {
  if (!WiFi.isConnected() || bibleToday() < 0) return;
  String version;
  { CfgLock c; version = cfg.bibleVersion; }
  int day = bibleToday();
  bool have;
  { StateLock l; have = online.day == day; }
  if (have && version == fetchedVersion) return;              // already have today's verse
  if (version != fetchedVersion) nextFetch = 0;                // translation changed
  if ((int32_t)(nextFetch - millis()) > 0) return;
  nextFetch = millis() + 30 * 60000UL;                         // retry in 30 min if it fails
  if (fetchVotd(version)) fetchedVersion = version;
}
