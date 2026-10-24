#include "clockapps.h"
#include <algorithm>
#include <math.h>
#include <time.h>
#include "hal.h"
#include "player.h"
#include "settings.h"
#include "sports.h"
#include "state.h"

static AlertInfo alert;
static uint32_t alertId = 0, alertStartedMs = 0;
static int alertAlarm = -1;          // which alarm raised the current alert
static time_t snoozeAt = 0;
static int snoozeAlarm = -1;
static int lastMinuteChecked = -1;
static std::vector<uint32_t> reminded;   // events already announced
static uint32_t lastReminderCheck = 0;

struct PendingMsg { String from, text; bool photo; };
static std::vector<PendingMsg> inbox;
static SemaphoreHandle_t inboxLock = nullptr;

void appsPostMessage(const String& from, const String& text, bool photo) {
  if (!inboxLock) inboxLock = xSemaphoreCreateMutex();
  xSemaphoreTake(inboxLock, portMAX_DELAY);
  if (inbox.size() < 5) inbox.push_back({from, text, photo});
  xSemaphoreGive(inboxLock);
}

static uint32_t timerTotal = 5 * 60, timerLeftMs = 5 * 60 * 1000UL, timerLastMs = 0;
static bool timerRunning = false;

static uint32_t sleepEndMs = 0;
static int sleepFadeStep = -1, sleepVolume = 0;

static String fmtTime(int h, int m) {
  bool h24;
  { CfgLock c; h24 = cfg.h24; }
  char b[12];
  if (h24) snprintf(b, sizeof(b), "%02d:%02d", h, m);
  else snprintf(b, sizeof(b), "%d:%02d %s", h % 12 == 0 ? 12 : h % 12, m, h < 12 ? "AM" : "PM");
  return b;
}

void appsRaise(AlertKind kind, const String& title, const String& detail, bool sound, const char* url) {
  alert.kind = kind;
  alert.title = title;
  alert.detail = detail;
  alert.canSnooze = kind == AlertKind::Alarm;
  alert.photo = false;
  alert.id = ++alertId;
  alertStartedMs = millis();
  halPokeActivity();  // wakes the screen
  if (sound) {
    uint8_t vol;
    { CfgLock c; vol = cfg.chimeVolume; }
    // Alarms and timers ring until stopped; everything else chimes once
    bool loud = kind == AlertKind::Alarm || kind == AlertKind::Timer;
    playerSend(loud ? Cmd::AlertStart : Cmd::Chime, vol, url);  // empty url = chime
  }
}

AlertInfo appsCurrentAlert() { return alert; }

void appsAlertStop() {
  if (alert.kind == AlertKind::None) return;
  alert.kind = AlertKind::None;
  alertAlarm = -1;
  playerSend(Cmd::AlertStop);
}

void appsAlertSnooze() {
  if (alert.kind != AlertKind::Alarm) { appsAlertStop(); return; }
  uint8_t mins;
  { CfgLock c; mins = cfg.snoozeMin; }
  snoozeAt = time(nullptr) + mins * 60;
  snoozeAlarm = alertAlarm;
  appsAlertStop();
}

static void fireAlarm(int idx) {
  Alarm a;
  {
    CfgLock c;
    if (idx < 0 || idx >= (int)cfg.alarms.size()) return;
    a = cfg.alarms[idx];
  }
  String title = a.label.length() ? a.label : String("Alarm");
  if (a.sound == "radio") {
    playerSend(Cmd::PlayStation, a.station);
    appsRaise(AlertKind::Alarm, title, fmtTime(a.hour, a.minute), false);
  } else {
    appsRaise(AlertKind::Alarm, title, fmtTime(a.hour, a.minute), true, a.sound == "reveille" ? playerReveilleUrl() : "");
  }
  alertAlarm = idx;
}

TimerInfo appsTimer() {
  return {timerTotal, (timerLeftMs + 999) / 1000, timerRunning};
}

void timerAdjust(int delta) {
  if (timerRunning) return;
  int t = constrain((int)timerTotal + delta, 10, 24 * 3600);
  timerTotal = t;
  timerLeftMs = t * 1000UL;
}

void timerToggle() {
  if (!timerRunning && timerLeftMs == 0) timerLeftMs = timerTotal * 1000UL;
  timerRunning = !timerRunning;
  timerLastMs = millis();
}

void timerReset() {
  timerRunning = false;
  timerLeftMs = timerTotal * 1000UL;
}

int sleepMinutesLeft() {
  if (!sleepEndMs) return 0;
  int32_t left = (int32_t)(sleepEndMs - millis());
  return left <= 0 ? 0 : (left + 59999) / 60000;
}

int sleepCycle() {
  static const int steps[] = {0, 15, 30, 60, 90};
  int cur = sleepMinutesLeft(), next = 15;
  for (int i = 0; i < 5; i++) if (cur <= steps[i]) { next = steps[(i + 1) % 5]; break; }
  if (cur > 90) next = 0;
  sleepEndMs = next ? millis() + next * 60000UL : 0;
  if (sleepFadeStep >= 0) { playerSend(Cmd::Volume, sleepVolume); sleepFadeStep = -1; }
  return next;
}

bool nightActive() {
  bool on;
  uint16_t s, e;
  { CfgLock c; on = cfg.nightOn; s = cfg.nightStart; e = cfg.nightEnd; }
  if (!on || time(nullptr) < 1700000000) return false;
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  int m = t.tm_hour * 60 + t.tm_min;
  return s <= e ? (m >= s && m < e) : (m >= s || m < e);
}

String nextAlarmText() {
  time_t now = time(nullptr);
  if (now < 1700000000) return "";
  struct tm t;
  localtime_r(&now, &t);
  int nowMin = t.tm_wday * 1440 + t.tm_hour * 60 + t.tm_min;
  int best = INT32_MAX;
  Alarm bestA;
  CfgLock c;
  for (auto& a : cfg.alarms) {
    if (!a.on || !a.days) continue;
    for (int d = 0; d < 7; d++) {
      if (!(a.days & (1 << d))) continue;
      int m = d * 1440 + a.hour * 60 + a.minute;
      int diff = (m - nowMin + 7 * 1440) % (7 * 1440);
      if (diff == 0) diff = 7 * 1440;
      if (diff < best) { best = diff; bestA = a; }
    }
  }
  if (best == INT32_MAX) return "";
  static const char* days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  int day = (t.tm_wday + (t.tm_hour * 60 + t.tm_min + best) / 1440) % 7;
  String when = best < 1440 && day == t.tm_wday ? "Today" : best < 2 * 1440 && day == (t.tm_wday + 1) % 7 ? "Tomorrow" : days[day];
  return when + " " + fmtTime(bestA.hour, bestA.minute);
}

std::vector<String> countdownLines() {
  std::vector<String> out;
  time_t now = time(nullptr);
  if (now < 1700000000) return out;
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = 12; t.tm_min = t.tm_sec = 0;
  time_t today = mktime(&t);
  std::vector<Countdown> list;
  { CfgLock c; list = cfg.countdowns; }
  std::vector<std::pair<long, String>> items;
  for (auto& cd : list) {
    struct tm d = {};
    d.tm_year = cd.date.substring(0, 4).toInt() - 1900;
    d.tm_mon = cd.date.substring(5, 7).toInt() - 1;
    d.tm_mday = cd.date.substring(8, 10).toInt();
    d.tm_hour = 12;
    d.tm_isdst = -1;
    long days = lround(difftime(mktime(&d), today) / 86400.0);
    if (days < 0) continue;
    String name = cd.name.length() ? cd.name : cd.date;
    String line = days == 0 ? name + " is today!" : days == 1 ? name + " is tomorrow" : String(days) + " days to " + name;
    items.push_back({days, line});
  }
  std::sort(items.begin(), items.end(), [](auto& a, auto& b) { return a.first < b.first; });
  for (auto& i : items) out.push_back(i.second);
  return out;
}

void appsTick() {
  uint32_t ms = millis();

  // Countdown timer
  if (timerRunning) {
    uint32_t dt = ms - timerLastMs;
    timerLastMs = ms;
    timerLeftMs = dt >= timerLeftMs ? 0 : timerLeftMs - dt;
    if (timerLeftMs == 0) {
      timerRunning = false;
      appsRaise(AlertKind::Timer, "Time's up!", "Timer finished");
    }
  }

  // Alerts stop by themselves after 10 minutes
  if (alert.kind != AlertKind::None && ms - alertStartedMs > 10 * 60000UL) appsAlertStop();

  // Radio sleep timer: fade out over the last 10 seconds, then stop
  if (sleepEndMs) {
    int32_t left = (int32_t)(sleepEndMs - ms);
    if (left <= 10000 && sleepFadeStep < 0) {
      StateLock l;
      sleepVolume = player.volume;
      sleepFadeStep = 0;
    }
    if (sleepFadeStep >= 0) {
      int want = 10 - max(0, (int)(left / 1000));
      if (want > sleepFadeStep && left > 0) {
        sleepFadeStep = want;
        playerSend(Cmd::Volume, sleepVolume * (10 - want) / 10);
      }
      if (left <= 0) {
        playerSend(Cmd::Stop);
        playerSend(Cmd::Volume, sleepVolume);
        sleepEndMs = 0;
        sleepFadeStep = -1;
      }
    }
  }

  // Messages from the web page and sports results (only when nothing else is showing)
  if (alert.kind == AlertKind::None) {
    PendingMsg m;
    bool have = false;
    if (inboxLock && xSemaphoreTake(inboxLock, 0) == pdTRUE) {
      if (!inbox.empty()) { m = inbox.front(); inbox.erase(inbox.begin()); have = true; }
      xSemaphoreGive(inboxLock);
    }
    if (have) {
      appsRaise(AlertKind::Message, m.from.length() ? "From " + m.from : String("Message"), m.text);
      alert.photo = m.photo;
    } else {
      String t, d;
      if (sportsTakeAlert(t, d)) appsRaise(AlertKind::Reminder, t, d);
    }
  }

  // Calendar reminders, checked every 15 seconds
  time_t now = time(nullptr);
  if (alert.kind == AlertKind::None && ms - lastReminderCheck > 15000 && now > 1700000000) {
    lastReminderCheck = ms;
    uint8_t lead;
    { CfgLock c; lead = cfg.reminderMin; }
    String title, when;
    if (lead) {
      StateLock l;
      for (auto& e : events) {
        if (e.allDay || e.start < now || e.start - now > lead * 60) continue;
        uint32_t key = (uint32_t)e.start ^ (uint32_t)e.title.length() * 2654435761u;
        if (std::find(reminded.begin(), reminded.end(), key) != reminded.end()) continue;
        reminded.push_back(key);
        if (reminded.size() > 50) reminded.erase(reminded.begin());
        struct tm t;
        localtime_r(&e.start, &t);
        when = "At " + fmtTime(t.tm_hour, t.tm_min) + (e.where.length() ? " - " + e.where : String(""));
        title = e.title;
        break;
      }
    }
    if (title.length()) appsRaise(AlertKind::Reminder, title, when);
  }

  // Alarms: checked once per minute
  if (now < 1700000000) return;
  if (snoozeAt && now >= snoozeAt) {
    snoozeAt = 0;
    fireAlarm(snoozeAlarm);
  }
  struct tm t;
  localtime_r(&now, &t);
  int minute = t.tm_hour * 60 + t.tm_min;
  if (minute == lastMinuteChecked) return;
  lastMinuteChecked = minute;
  int fire = -1;
  {
    CfgLock c;
    for (size_t i = 0; i < cfg.alarms.size(); i++) {
      const Alarm& a = cfg.alarms[i];
      if (a.on && (a.days & (1 << t.tm_wday)) && a.hour == t.tm_hour && a.minute == t.tm_min) { fire = i; break; }
    }
  }
  if (fire >= 0) fireAlarm(fire);
}
