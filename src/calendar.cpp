#include "calendar.h"
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <algorithm>
#include <set>
#include "settings.h"
#include "state.h"
#include "textutil.h"

static uint32_t nextRefresh = 0;
static const long kWindow = 14 * 86400L;

void calendarRefreshNow() { nextRefresh = 0; }

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return (int64_t)era * 146097 + (int64_t)(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
}

// "20261008T153000Z" (UTC), "20261008T153000" (local), "20261008" (all-day, local midnight)
static time_t parseIcsTime(const String& v, bool& allDay) {
  if (v.length() < 8) return 0;
  int y = v.substring(0, 4).toInt(), mo = v.substring(4, 6).toInt(), d = v.substring(6, 8).toInt();
  allDay = v.length() == 8;
  int h = 0, mi = 0, s = 0;
  if (!allDay && v.length() >= 15) { h = v.substring(9, 11).toInt(); mi = v.substring(11, 13).toInt(); s = v.substring(13, 15).toInt(); }
  if (v.endsWith("Z")) return (time_t)(daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + s);
  // Floating or TZID times: assume the calendar's zone matches the watch's
  struct tm t = {};
  t.tm_year = y - 1900; t.tm_mon = mo - 1; t.tm_mday = d; t.tm_hour = h; t.tm_min = mi; t.tm_sec = s; t.tm_isdst = -1;
  return mktime(&t);
}

static String param(const String& rule, const char* key) {
  String k = String(key) + "=";
  int i = rule.indexOf(k);
  if (i < 0) return "";
  int e = rule.indexOf(';', i);
  return rule.substring(i + k.length(), e < 0 ? rule.length() : e);
}

struct RawEvent {
  String uid, title, where, rrule, recurrenceId;
  time_t start = 0, end = 0;
  bool allDay = false;
  std::vector<time_t> exdates;
};

static void addOccurrence(std::vector<CalEvent>& out, const RawEvent& e, time_t start, const String& cal) {
  CalEvent c;
  c.start = start;
  c.end = start + (e.end > e.start ? e.end - e.start : (e.allDay ? 86400 : 3600));
  c.allDay = e.allDay;
  c.title = e.title;
  c.where = e.where;
  c.cal = cal;
  out.push_back(c);
}

// Expand a (possibly repeating) event into occurrences inside [from, to)
static void expand(const RawEvent& e, time_t from, time_t to, const std::set<time_t>& overridden, std::vector<CalEvent>& out, const String& cal) {
  time_t dur = e.end > e.start ? e.end - e.start : 0;
  auto keep = [&](time_t s) {
    if (s + dur < from || s >= to) return;
    if (overridden.count(s)) return;
    for (time_t x : e.exdates) if (x == s) return;
    addOccurrence(out, e, s, cal);
  };
  if (e.rrule.isEmpty()) { keep(e.start); return; }

  String freq = param(e.rrule, "FREQ");
  int interval = max(1, (int)param(e.rrule, "INTERVAL").toInt());
  int count = param(e.rrule, "COUNT").toInt();
  String untilS = param(e.rrule, "UNTIL");
  bool dummy;
  time_t until = untilS.length() ? parseIcsTime(untilS, dummy) + (untilS.length() == 8 ? 86399 : 0) : 0;
  String byday = param(e.rrule, "BYDAY");
  static const char* dn[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

  struct tm base;
  localtime_r(&e.start, &base);
  int n = 0;
  for (int step = 0; step < 4000; step++) {
    struct tm t = base;
    t.tm_isdst = -1;
    if (freq == "DAILY") t.tm_mday += step * interval;
    else if (freq == "WEEKLY") t.tm_mday += step * interval * 7;
    else if (freq == "MONTHLY") t.tm_mon += step * interval;
    else if (freq == "YEARLY") t.tm_year += step * interval;
    else { keep(e.start); return; }
    time_t s = mktime(&t);
    if ((until && s > until) || s >= to) break;

    std::vector<time_t> cands;
    if (freq == "WEEKLY" && byday.length()) {
      // every listed weekday in this week (week starts on the DTSTART weekday)
      for (int d = 0; d < 7; d++) {
        struct tm w = t;
        w.tm_mday += d;
        w.tm_isdst = -1;
        time_t ws = mktime(&w);
        if (byday.indexOf(dn[w.tm_wday]) >= 0) cands.push_back(ws);
      }
    } else if (freq == "MONTHLY" && byday.length() >= 3) {
      // e.g. 2TU = second Tuesday, -1FR = last Friday
      int nth = byday.substring(0, byday.length() - 2).toInt();
      String wd = byday.substring(byday.length() - 2);
      int target = 0;
      for (int k = 0; k < 7; k++) if (wd == dn[k]) target = k;
      struct tm m = t;
      m.tm_mday = 1; m.tm_isdst = -1;
      mktime(&m);
      int first = (target - m.tm_wday + 7) % 7 + 1;
      int day = nth > 0 ? first + (nth - 1) * 7 : 0;
      if (nth < 0) {
        struct tm l = t; l.tm_mon += 1; l.tm_mday = 0; l.tm_isdst = -1; mktime(&l);  // last day of month
        day = l.tm_mday - ((l.tm_wday - target + 7) % 7);
      }
      m = t; m.tm_mday = day; m.tm_isdst = -1;
      time_t ms = mktime(&m);
      if (m.tm_mon == t.tm_mon) cands.push_back(ms);
    } else {
      cands.push_back(s);
    }
    for (time_t c : cands) {
      if (c < e.start) continue;
      if (until && c > until) break;
      if (count && n >= count) return;
      n++;
      keep(c);
    }
    if (count && n >= count) return;
  }
}

// Stream an .ics file line by line, collecting events that touch the window
static bool readCalendar(const CalendarSrc& src, time_t from, time_t to, std::vector<CalEvent>& out) {
  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(15000);
  if (!http.begin(tls, src.url)) return false;
  if (http.GET() != 200) { http.end(); return false; }
  WiFiClient* s = http.getStreamPtr();

  std::vector<RawEvent> raws;
  RawEvent cur;
  bool inEvent = false;
  String line, pending;
  uint32_t lineBytes = 0;
  uint32_t last = millis();
  auto handle = [&](const String& ln) {
    if (ln == "BEGIN:VEVENT") { cur = RawEvent(); inEvent = true; return; }
    if (ln == "END:VEVENT") {
      inEvent = false;
      // Skip one-off events far outside the window early to save memory
      if (cur.rrule.isEmpty() && (cur.start > to || (cur.end ? cur.end : cur.start + 86400) < from)) return;
      if (raws.size() < 400) raws.push_back(cur);
      return;
    }
    if (!inEvent) return;
    int colon = ln.indexOf(':');
    if (colon < 0) return;
    String key = ln.substring(0, colon), val = ln.substring(colon + 1);
    String name = key.substring(0, key.indexOf(';') < 0 ? key.length() : key.indexOf(';'));
    bool ad;
    if (name == "DTSTART") { cur.start = parseIcsTime(val, ad); cur.allDay = ad; }
    else if (name == "DTEND") cur.end = parseIcsTime(val, ad);
    else if (name == "SUMMARY") cur.title = cleanText(val, 80);
    else if (name == "LOCATION") cur.where = cleanText(val, 60);
    else if (name == "RRULE") cur.rrule = val;
    else if (name == "UID") cur.uid = val;
    else if (name == "RECURRENCE-ID") cur.recurrenceId = val;
    else if (name == "EXDATE") {
      int p = 0;
      while (p < (int)val.length()) {
        int c = val.indexOf(',', p);
        String one = val.substring(p, c < 0 ? val.length() : c);
        cur.exdates.push_back(parseIcsTime(one, ad));
        if (c < 0) break;
        p = c + 1;
      }
    }
  };
  // RFC 5545 folds long lines: a line starting with a space continues the previous one
  while (millis() - last < 15000) {
    if (!s->available()) {
      if (!s->connected()) break;
      delay(5);
      continue;
    }
    char c = s->read();
    last = millis();
    if ((++lineBytes & 1023) == 0) vTaskDelay(1);  // let the idle task run (watchdog)
    if (c == '\r') continue;
    if (c != '\n') { line += c; continue; }
    if (line.startsWith(" ") || line.startsWith("\t")) { pending += line.substring(1); line = ""; continue; }
    if (pending.length()) handle(pending);
    pending = line;
    line = "";
  }
  if (pending.length()) handle(pending);
  http.end();

  // Modified instances of repeating events replace the original occurrence
  std::set<time_t> overridden;
  for (auto& r : raws) if (r.recurrenceId.length()) { bool ad; overridden.insert(parseIcsTime(r.recurrenceId, ad)); }
  for (auto& r : raws) expand(r, from, to, r.recurrenceId.length() ? std::set<time_t>() : overridden, out, src.name);
  return true;
}

void calendarService() {
  if (!WiFi.isConnected() || time(nullptr) < 1700000000) return;
  if ((int32_t)(nextRefresh - millis()) > 0) return;
  nextRefresh = millis() + 15 * 60000UL;
  std::vector<CalendarSrc> list;
  { CfgLock c; list = cfg.calendars; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);
  t.tm_hour = t.tm_min = t.tm_sec = 0;
  t.tm_isdst = -1;
  time_t from = mktime(&t);  // start of today
  std::vector<CalEvent> all;
  int ok = 0;
  for (auto& c : list) { if (readCalendar(c, from, from + kWindow, all)) ok++; vTaskDelay(1); }
  std::sort(all.begin(), all.end(), [](const CalEvent& a, const CalEvent& b) { return a.start < b.start; });
  if (all.size() > 60) all.resize(60);
  StateLock l;
  if (ok || list.empty()) events = all;
  calendarStatus = list.empty() ? "" : ok == 0 ? "Couldn't read your calendar" : "";
  eventsVersion++;
}
