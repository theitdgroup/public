// Runtime state shared between tasks. LVGL is only touched by the UI task;
// other tasks publish here and the UI polls the version counters.
#pragma once
#include <Arduino.h>
#include <vector>

enum class Source : uint8_t { None = 0, Radio, SD };
const char* sourceName(Source s);

struct PlayerState {
  Source source = Source::None;
  bool playing = false;
  bool busy = false;          // connecting / buffering
  String title, artist, album, station, error;
  uint32_t posMs = 0, durMs = 0;
  uint8_t volume = 12;        // 0-21
  int stationIdx = -1;
  int trackIdx = -1, trackCount = 0;
  bool shuffle = false;
  bool sdMounted = false;
  uint32_t version = 0;
};

struct NewsItem {
  String source, title, summary;
  time_t ts = 0;
};

struct NetState {
  bool connected = false;
  bool apMode = false;
  bool timeSynced = false;
  String ip, ssid, apName;
  String wifiError;         // why the last join attempt failed, "" if fine
  int rssi = 0;
  uint32_t version = 0;
};

struct DayForecast {
  int code = 0;
  float hi = 0, lo = 0;
  int rain = 0;     // % chance
  int wday = 0;     // 0 = Sunday
};

struct WeatherState {
  bool valid = false;
  float temp = 0, feels = 0, wind = 0;
  int code = 0, humidity = 0;
  bool day = true;
  DayForecast days[6];
  int ndays = 0;
  String error;
  time_t updated = 0;
  uint32_t version = 0;
};

struct CalEvent {
  time_t start = 0, end = 0;
  bool allDay = false;
  String title, where, cal;
};

struct Game {
  String league, teamAbbr;       // the followed team
  String home, away;             // abbreviations
  int homeScore = 0, awayScore = 0;
  int state = 0;                 // 0 scheduled, 1 in progress, 2 final
  String detail;                 // "Q4 2:31", "Final", "Sun 1:25 PM"
  time_t start = 0;
};

struct PowerState {
  bool present = false;  // PMU found
  bool battery = false;
  bool charging = false;
  bool usb = false;
  int percent = -1;
};

extern PlayerState player;
extern std::vector<NewsItem> news;
extern uint32_t newsVersion;
extern String newsStatus;
extern NetState net;
extern WeatherState weather;
extern std::vector<CalEvent> events;
extern uint32_t eventsVersion;
extern String calendarStatus;
extern std::vector<Game> games;
extern uint32_t gamesVersion;
extern PowerState power;   // written by UI task only
extern SemaphoreHandle_t stateMutex;

struct StateLock {
  StateLock()  { xSemaphoreTakeRecursive(stateMutex, portMAX_DELAY); }
  ~StateLock() { xSemaphoreGiveRecursive(stateMutex); }
};

void stateInit();
