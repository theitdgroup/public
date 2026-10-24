// Persistent configuration, stored as /config.json on LittleFS.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>

struct WifiNet  { String ssid, pass; };
struct Station  { String name, url; };
struct Feed     { String name, url; bool enabled = true; };
struct Alarm {
  uint8_t hour = 7, minute = 0;
  uint8_t days = 0x3E;    // bit 0 = Sunday ... bit 6 = Saturday (default Mon-Fri)
  bool on = true;
  String sound = "reveille";  // "chime", "reveille" or "radio" (play a station)
  int16_t station = 0;
  String label;
};
struct Countdown { String name, date; };  // date as YYYY-MM-DD
struct CalendarSrc { String name, url; };  // private iCal (.ics) link
struct Team { String sport, league, id, abbr, name; };  // ESPN identifiers

struct Settings {
  std::vector<WifiNet> wifi;
  String hostname = "clockhub";
  String tz = "UTC0";          // POSIX TZ string
  String tzName = "UTC";       // IANA name, only for display in the web UI
  String ntp = "pool.ntp.org";
  bool   h24 = true;

  uint8_t  brightness = 200;   // 0-255
  uint8_t  dimBrightness = 40;
  uint16_t dimAfter = 60;      // seconds idle before dimming, 0 = never
  uint16_t offAfter = 0;       // seconds idle before screen off, 0 = never
  bool     pixelShift = true;  // nudge the clock face to limit AMOLED burn-in

  String  face = "classic";    // active face id (file /faces/<id>.json)
  uint8_t volume = 12;         // 0-21

  std::vector<Station> stations;
  std::vector<Feed> feeds;
  std::vector<String> topics;  // turned into Google News RSS searches
  uint16_t newsRefreshMin = 20;


  // KY-040 rotary encoder on the expansion header
  bool    encoderEnabled = true;
  int8_t  encoderA = 18, encoderB = 17, encoderBtn = 16;  // CLK, DT, SW (as wired on these builds)
  bool    encoderReverse = false;
  uint8_t encoderStepsPerDetent = 4;

  // Photo slideshow
  uint16_t photoSeconds = 8;   // time per photo
  bool     photoShuffle = false;
  bool     photoFrame = false; // when idle, show the slideshow instead of the clock

  // Bible screen
  bool     bibleRandom = false;  // false = verse of the day, true = a new random verse every bibleMinutes
  uint16_t bibleMinutes = 30;
  String   bibleVersion = "KJV";  // Bible Gateway translation code for Verse of the Day

  // Weather (Open-Meteo, no account needed)
  float lat = NAN, lon = NAN;
  String place;
  bool fahrenheit = true;

  // Alarms, timers, night mode, countdowns
  std::vector<Alarm> alarms;
  uint8_t chimeVolume = 15;    // 0-21
  uint8_t snoozeMin = 9;
  bool nightOn = false;
  uint16_t nightStart = 22 * 60, nightEnd = 7 * 60;  // minutes after midnight
  uint8_t nightBrightness = 12;
  std::vector<Countdown> countdowns;

  // Calendar and sports
  std::vector<CalendarSrc> calendars;
  uint8_t reminderMin = 10;    // chime this long before an event, 0 = off
  std::vector<Team> teams;
  bool sportsAlerts = true;    // pop-up when a followed game starts / ends

  // Screens shown on the watch, in order (ids: clock weather today music news photos timer settings)
  std::vector<String> screens;

  std::vector<String> galleries;  // face gallery catalog URLs
  String adminPass;               // optional HTTP basic-auth password for the web UI
};

extern Settings cfg;
extern SemaphoreHandle_t cfgMutex;  // recursive; hold via CfgLock when touching cfg from any task

struct CfgLock {
  CfgLock()  { xSemaphoreTakeRecursive(cfgMutex, portMAX_DELAY); }
  ~CfgLock() { xSemaphoreGiveRecursive(cfgMutex); }
};

void settingsInit();                 // mounts LittleFS (must already be mounted) and loads
bool settingsSave();
void settingsSaveLater();          // from tasks whose stack is in PSRAM (no flash writes there)
void settingsService();            // UI task: performs a pending save
void settingsToJson(JsonDocument& doc, bool redactSecrets);
void settingsFromJson(JsonVariantConst j);  // merges; "********" leaves a secret unchanged

static const char* const kSecretMask = "********";
