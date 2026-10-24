#include "sports.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <map>
#include <vector>
#include "settings.h"
#include "state.h"
#include "textutil.h"

static uint32_t nextRefresh = 0;
static String lastErr;
static std::map<String, int> lastState;          // team key -> last seen game state
static std::vector<std::pair<String, String>> alerts;
static SemaphoreHandle_t alertLock = nullptr;
static void lockInit() { if (!alertLock) alertLock = xSemaphoreCreateMutex(); }

void sportsRefreshNow() { nextRefresh = 0; }

bool sportsTakeAlert(String& title, String& detail) {
  lockInit();
  xSemaphoreTake(alertLock, portMAX_DELAY);
  bool any = !alerts.empty();
  if (any) { title = alerts.front().first; detail = alerts.front().second; alerts.erase(alerts.begin()); }
  xSemaphoreGive(alertLock);
  return any;
}

static bool getJson(const String& url, JsonDocument& doc, JsonDocument& filter) {
  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(15000);
  if (!http.begin(tls, url)) { lastErr = "bad url"; return false; }
  int code = http.GET();
  if (code != 200) { lastErr = "HTTP " + String(code); http.end(); return false; }
  // Download the whole reply (100-400 KB, lands in PSRAM) before parsing: parsing
  // straight from the TLS stream stops early when data arrives in bursts
  WiFiClient* st = http.getStreamPtr();
  int len = http.getSize();
  char* buf = (char*)ps_malloc(len > 0 ? len + 1 : 2048 * 1024);
  if (!buf) { lastErr = "no memory"; http.end(); return false; }
  size_t cap = len > 0 ? len : 2048 * 1024, got = 0;
  uint32_t last = millis();
  while (got < cap && millis() - last < 15000) {
    int n = st->read((uint8_t*)buf + got, min<size_t>(4096, cap - got));
    if (n > 0) { got += n; last = millis(); vTaskDelay(1); }  // let the idle task run (watchdog)
    else if (!st->connected() && !st->available()) break;
    else delay(2);
  }
  http.end();
  // ESPN responses nest deeply even where the filter skips them
  DeserializationError e = deserializeJson(doc, buf, got, DeserializationOption::Filter(filter), DeserializationOption::NestingLimit(40));
  free(buf);
  if (e) lastErr = String("JSON ") + e.c_str() + " after " + String(got) + " bytes";
  return !e;
}

static const char* leagueName(const String& l) {
  if (l == "nfl") return "NFL";
  if (l == "college-football") return "NCAAF";
  if (l == "nba") return "NBA";
  if (l == "wnba") return "WNBA";
  if (l == "mens-college-basketball") return "NCAAM";
  if (l == "womens-college-basketball") return "NCAAW";
  if (l == "mlb") return "MLB";
  if (l == "nhl") return "NHL";
  if (l == "usa.1") return "MLS";
  if (l == "womens-college-volleyball") return "Volleyball";
  return "";
}

void sportsService() {
  if (!WiFi.isConnected() || time(nullptr) < 1700000000) return;
  if ((int32_t)(nextRefresh - millis()) > 0) return;
  std::vector<Team> teams;
  bool notify;
  { CfgLock c; teams = cfg.teams; notify = cfg.sportsAlerts; }
  lockInit();
  if (teams.empty()) {
    nextRefresh = millis() + 15 * 60000UL;
    StateLock l;
    if (!games.empty()) { games.clear(); gamesVersion++; }
    return;
  }

  JsonDocument filter;
  JsonObject ev = filter["events"].add<JsonObject>();
  ev["date"] = true;
  ev["status"]["type"]["state"] = true;
  ev["status"]["type"]["shortDetail"] = true;
  JsonObject comp = ev["competitions"].add<JsonObject>();
  JsonObject cp = comp["competitors"].add<JsonObject>();
  cp["homeAway"] = true;
  cp["score"] = true;
  cp["team"]["id"] = true;
  cp["team"]["abbreviation"] = true;

  std::vector<Game> found;
  bool live = false;
  // One scoreboard request per league, shared by all followed teams in it
  std::map<String, std::vector<Team>> byLeague;
  for (auto& t : teams) byLeague[t.sport + "/" + t.league].push_back(t);
  for (auto& kv : byLeague) {
    JsonDocument doc;
    if (!getJson("https://site.api.espn.com/apis/site/v2/sports/" + kv.first + "/scoreboard", doc, filter)) continue;
    for (auto& team : kv.second) {
      for (JsonObject e : doc["events"].as<JsonArray>()) {
        JsonArray cs = e["competitions"][0]["competitors"];
        bool ours = false;
        for (JsonObject c : cs) if (c["team"]["id"].as<String>() == team.id) ours = true;
        if (!ours) continue;
        Game g;
        g.league = leagueName(team.league);
        g.teamAbbr = team.abbr;
        for (JsonObject c : cs) {
          bool home = c["homeAway"] == "home";
          (home ? g.home : g.away) = c["team"]["abbreviation"].as<String>();
          (home ? g.homeScore : g.awayScore) = atoi(c["score"] | "0");
        }
        String st = e["status"]["type"]["state"] | "pre";
        g.state = st == "in" ? 1 : st == "post" ? 2 : 0;
        g.detail = asciiFold(e["status"]["type"]["shortDetail"] | "");
        g.start = parseFeedDate(e["date"] | "");
        if (g.state == 1) live = true;
        found.push_back(g);

        // Alerts on state changes (not on the first look after boot)
        String key = team.sport + team.league + team.id;
        auto it = lastState.find(key);
        if (notify && it != lastState.end() && it->second != g.state) {
          String score = g.away + " " + String(g.awayScore) + "  -  " + String(g.homeScore) + " " + g.home;
          xSemaphoreTake(alertLock, portMAX_DELAY);
          if (g.state == 1) alerts.push_back({team.abbr + " game is starting", g.away + " at " + g.home});
          else if (g.state == 2) alerts.push_back({"Final: " + team.abbr, score});
          xSemaphoreGive(alertLock);
        }
        lastState[key] = g.state;
        break;
      }
    }
    vTaskDelay(1);
  }
  nextRefresh = millis() + (live ? 60000UL : 15 * 60000UL);
  StateLock l;
  games = found;
  gamesVersion++;
}

static String teamListImpl(const String& sport, const String& league) {
  JsonDocument filter;
  JsonObject t = filter["sports"][0]["leagues"][0]["teams"][0]["team"].to<JsonObject>();
  t["id"] = true;
  t["abbreviation"] = true;
  t["displayName"] = true;
  JsonDocument doc;
  if (!getJson("https://site.api.espn.com/apis/site/v2/sports/" + sport + "/" + league + "/teams?limit=1000", doc, filter)) return "";
  JsonDocument out;
  JsonArray a = out.to<JsonArray>();
  for (JsonObject x : doc["sports"][0]["leagues"][0]["teams"].as<JsonArray>()) {
    JsonObject o = a.add<JsonObject>();
    o["id"] = x["team"]["id"];
    o["abbr"] = x["team"]["abbreviation"];
    o["name"] = x["team"]["displayName"];
  }
  String s;
  serializeJson(out, s);
  return s;
}

// The web task's stack is too small for a TLS handshake, so fetch on a helper
// task whose stack lives in PSRAM and wait for it.
struct TeamJob { String sport, league, result; SemaphoreHandle_t done; };

static void teamJobTask(void* p) {
  TeamJob* j = (TeamJob*)p;
  j->result = teamListImpl(j->sport, j->league);
  xSemaphoreGive(j->done);
  for (;;) vTaskDelay(portMAX_DELAY);  // deleted by the caller
}

String sportsLastError() { return lastErr; }

String sportsTeamList(const String& sport, const String& league) {
  TeamJob job{sport, league, "", xSemaphoreCreateBinary()};
  TaskHandle_t h = nullptr;
  if (xTaskCreatePinnedToCoreWithCaps(teamJobTask, "teams", 16384, &job, 1, &h, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
    vSemaphoreDelete(job.done);
    lastErr = "no task";
    return "";
  }
  bool ok = xSemaphoreTake(job.done, pdMS_TO_TICKS(120000)) == pdTRUE;
  vTaskDeleteWithCaps(h);
  vSemaphoreDelete(job.done);
  return ok ? job.result : String();
}
