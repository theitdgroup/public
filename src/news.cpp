#include "news.h"
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include <algorithm>
#include "settings.h"
#include "state.h"
#include "textutil.h"

static const size_t kMaxBody = 400 * 1024;
static const size_t kPerFeed = 15;
static const size_t kMaxItems = 60;
static uint32_t nextRefresh = 0;

void newsRefreshNow() { nextRefresh = 0; }

// Content of the first <tag ...>...</tag> in [from, to)
static String tagContent(const String& xml, int from, int to, const char* tag) {
  String open = String("<") + tag;
  int p = from;
  while (true) {
    p = xml.indexOf(open, p);
    if (p < 0 || p >= to) return "";
    char next = xml[p + open.length()];
    if (next == '>' || next == ' ' || next == '/' || next == '\n' || next == '\t') break;
    p += open.length();  // matched a longer tag name, e.g. <titleX>
  }
  int gt = xml.indexOf('>', p);
  if (gt < 0 || gt >= to) return "";
  if (xml[gt - 1] == '/') return "";  // self-closing
  int close = xml.indexOf(String("</") + tag + ">", gt);
  if (close < 0 || close > to) return "";
  return xml.substring(gt + 1, close);
}

static bool fetch(const String& url, String& body) {
  NetworkClientSecure tls;
  tls.setInsecure();
  WiFiClient plain;
  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, read until close
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(10000);
  http.setUserAgent("Mozilla/5.0 (ClockHub ESP32) RSS");
  bool ok = url.startsWith("https") ? http.begin(tls, url) : http.begin(plain, url);
  if (!ok) return false;
  int code = http.GET();
  if (code != 200) {
    log_w("feed %s -> %d", url.c_str(), code);
    http.end();
    return false;
  }
  WiFiClient* s = http.getStreamPtr();
  body = "";
  body.reserve(64 * 1024);
  char buf[1024];
  uint32_t last = millis();
  while (body.length() < kMaxBody && millis() - last < 8000) {
    int n = s->read((uint8_t*)buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; body.concat(buf, n); last = millis(); vTaskDelay(1); }  // let the idle task run (watchdog)
    else if (!s->connected() && !s->available()) break;
    else delay(5);
  }
  http.end();
  return body.length() > 0;
}

static void parse(const String& xml, const String& source, bool splitSource, std::vector<NewsItem>& out) {
  bool atom = xml.indexOf("<feed") >= 0 && xml.indexOf("<entry") >= 0;
  const char* itemTag = atom ? "<entry" : "<item";
  const char* itemEnd = atom ? "</entry>" : "</item>";
  int pos = 0;
  size_t count = 0;
  while (count < kPerFeed) {
    int start = xml.indexOf(itemTag, pos);
    if (start < 0) break;
    int end = xml.indexOf(itemEnd, start);
    if (end < 0) break;
    pos = end;

    NewsItem it;
    it.title = cleanText(tagContent(xml, start, end, "title"), 160);
    if (it.title.isEmpty()) continue;
    String sum;
    for (const char* t : {"description", "summary", "content", "media:description", "content:encoded"}) {
      sum = tagContent(xml, start, end, t);
      if (sum.length()) break;
    }
    it.summary = cleanText(sum, 420);
    String date;
    for (const char* t : {"pubDate", "published", "updated", "dc:date"}) {
      date = tagContent(xml, start, end, t);
      if (date.length()) break;
    }
    it.ts = parseFeedDate(date);
    it.source = source;
    // Google News titles are "Headline - Publisher"
    if (splitSource) {
      int dash = it.title.lastIndexOf(" - ");
      if (dash > 0) { it.source = it.title.substring(dash + 3); it.title = it.title.substring(0, dash); }
      if (it.summary.startsWith(it.title)) it.summary = "";
    }
    if (it.summary == it.title) it.summary = "";
    out.push_back(it);
    count++;
  }
}

static void refresh() {
  std::vector<std::pair<String, String>> feeds;  // name, url
  std::vector<String> topics;
  {
    CfgLock c;
    for (auto& f : cfg.feeds) if (f.enabled) feeds.push_back({f.name, f.url});
    topics = cfg.topics;
  }
  std::vector<NewsItem> items;
  int ok = 0, total = feeds.size() + topics.size();
  String xml;
  for (auto& f : feeds) {
    if (fetch(f.second, xml)) { parse(xml, f.first, false, items); ok++; }
    xml = "";
    vTaskDelay(1);
  }
  for (auto& t : topics) {
    String url = "https://news.google.com/rss/search?q=" + urlEncode(t) + "&hl=en-US&gl=US&ceid=US:en";
    if (fetch(url, xml)) { parse(xml, t, true, items); ok++; }
    xml = "";
  }
  // Newest first; drop duplicates that several feeds carry
  std::stable_sort(items.begin(), items.end(), [](const NewsItem& a, const NewsItem& b) { return a.ts > b.ts; });
  std::vector<NewsItem> unique;
  for (auto& it : items) {
    bool dup = false;
    for (auto& u : unique) if (u.title == it.title) { dup = true; break; }
    if (!dup) unique.push_back(it);
    if (unique.size() >= kMaxItems) break;
  }
  StateLock l;
  if (!unique.empty() || total == 0) news = unique;
  newsStatus = total == 0 ? "No feeds enabled" : ok == 0 ? "Couldn't reach news feeds" : "";
  newsVersion++;
  log_i("news: %d/%d feeds, %u items", ok, total, (unsigned)news.size());
}

void newsService() {
  if (!WiFi.isConnected()) return;
  if ((int32_t)(nextRefresh - millis()) > 0) return;
  uint16_t mins;
  { CfgLock c; mins = cfg.newsRefreshMin; }
  {
    StateLock l;
    if (news.empty()) { newsStatus = "Loading headlines..."; newsVersion++; }
  }
  refresh();
  nextRefresh = millis() + mins * 60000UL;
}
