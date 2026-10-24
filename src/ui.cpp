#include "ui.h"
#include <LittleFS.h>
#include <lvgl.h>
#include <algorithm>
#include <math.h>
#include <src/misc/cache/lv_cache.h>  // lv_image_cache_drop (not in the public headers in 9.6)
#include <time.h>
#include "encoder.h"
#include "faces.h"
#include "hal.h"
#include "net.h"
#include "clockapps.h"
#include "photos.h"
#include "bible.h"
#include "weather.h"
#include "player.h"
#include "settings.h"
#include "state.h"
#include "storage.h"

#define C_BG      0x000000
#define C_SURFACE 0x1C1C1E
#define C_PRESSED 0x3A3A3C
#define C_TEXT    0xFFFFFF
#define C_MUTED   0x8E8E93
#define C_ACCENT  0x0A84FF
#define C_GREEN   0x1DB954
#define C_ERROR   0xFF453A

static lv_obj_t *tv, *tileClock, *tileMusic, *tileNews, *tilePhotos, *tileSet, *faceHost;
static lv_obj_t *tileWeather, *tileToday, *tileWeek, *tileTimer, *tileSports, *tileBible;
static lv_obj_t *btnSleep, *lblSleep;  // radio sleep timer button on the Music screen
static void onSleep(lv_event_t*);

// Screens, in the order set on the web page; disabled ones are built off-screen
enum class Scr : uint8_t { Clock, Weather, Today, Week, Sports, Music, News, Photos, Timer, Bible, Settings, Count };
static const char* const kScrIds[] = {"clock", "weather", "today", "week", "sports", "music", "news", "photos", "timer", "bible", "settings"};
static lv_obj_t* scrObj[(int)Scr::Count];
static std::vector<Scr> order;
static lv_obj_t *srcBtn[2], *artIcon, *lblTitle, *lblArtist, *lblInfo;
static lv_obj_t *btnPlay, *lblPlay, *btnList, *btnShuffle, *volArc, *progArc;
static lv_obj_t *newsList, *lblNewsStatus;
static lv_obj_t *qr, *lblNet, *briSlider, *lblFace, *lblPower;
static lv_obj_t *overlay = nullptr, *toast = nullptr;
static lv_obj_t* overlayScroll = nullptr;  // scrolled by the rotary knob while an overlay is open

static uint32_t seenPlayer = UINT32_MAX, seenNews = UINT32_MAX, seenNet = UINT32_MAX;
static volatile bool reqFace = true, reqSettings = true;
static String faceSoundPending;  // played once the web server is up (the audio task fetches it over loopback)
static String qrText;
static uint8_t appliedBrightness = 0;
static bool dimmed = false;

volatile int uiTile = 0;
volatile uint32_t uiRenderSamples = 0, uiRenderUs = 0, uiFlushUs = 0;
void uiRequestFaceReload() { reqFace = true; }
void uiRequestSettingsApply() { reqSettings = true; }

// ---------- helpers ----------

static lv_obj_t* label(lv_obj_t* p, const lv_font_t* f, uint32_t color, const char* txt = "") {
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_style_text_font(l, f, 0);
  lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
  lv_label_set_text(l, txt);
  return l;
}

static lv_obj_t* roundBtn(lv_obj_t* p, const char* sym, int size, lv_event_cb_t cb, void* ud = nullptr) {
  lv_obj_t* b = lv_button_create(p);
  lv_obj_set_size(b, size, size);
  lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_PRESSED), LV_STATE_PRESSED);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_t* l = label(b, size >= 80 ? &lv_font_montserrat_32 : &lv_font_montserrat_20, C_TEXT, sym);
  lv_obj_center(l);
  if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, ud);
  return b;
}

static void hideToast(lv_timer_t* t) {
  if (toast) { lv_obj_delete(toast); toast = nullptr; }
  lv_timer_delete(t);
}

static void showToast(const String& msg) {
  if (toast) lv_obj_delete(toast);
  toast = lv_label_create(lv_layer_top());
  lv_label_set_text(toast, msg.c_str());
  lv_obj_set_style_text_font(toast, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(toast, lv_color_hex(C_TEXT), 0);
  lv_obj_set_style_bg_color(toast, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_bg_opa(toast, LV_OPA_90, 0);
  lv_obj_set_style_pad_hor(toast, 18, 0);
  lv_obj_set_style_pad_ver(toast, 10, 0);
  lv_obj_set_style_radius(toast, 20, 0);
  lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -40);
  lv_timer_create(hideToast, 1800, nullptr);
}

static void closeOverlay(lv_event_t* = nullptr) {
  if (overlay) { lv_obj_delete_async(overlay); overlay = nullptr; }
  overlayScroll = nullptr;
}

static lv_obj_t* openOverlay(const char* title) {
  closeOverlay();
  overlay = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(overlay);
  lv_obj_set_size(overlay, 466, 466);
  lv_obj_set_style_bg_color(overlay, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_COVER, 0);
  lv_obj_set_clickable(overlay, true);
  lv_obj_t* close = roundBtn(overlay, LV_SYMBOL_CLOSE, 56, closeOverlay);
  lv_obj_align(close, LV_ALIGN_TOP_MID, 0, 22);
  lv_obj_t* t = label(overlay, &lv_font_montserrat_16, C_MUTED, title);
  lv_obj_align(t, LV_ALIGN_TOP_MID, 0, 84);
  return overlay;
}

// Fade list rows towards the top/bottom of the round screen
static void roundFade(lv_event_t* e) {
  lv_obj_t* cont = (lv_obj_t*)lv_event_get_target(e);
  lv_area_t ca;
  lv_obj_get_coords(cont, &ca);
  int32_t cy = ca.y1 + lv_area_get_height(&ca) / 2;
  int32_t half = lv_area_get_height(&ca) / 2;
  if (half < 10) return;  // not laid out yet (e.g. a screen that's switched off)
  uint32_t n = lv_obj_get_child_count(cont);
  for (uint32_t i = 0; i < n; i++) {
    lv_obj_t* ch = lv_obj_get_child(cont, i);
    lv_area_t a;
    lv_obj_get_coords(ch, &a);
    int32_t d = LV_ABS(a.y1 + lv_area_get_height(&a) / 2 - cy);
    int32_t opa = d < half * 6 / 10 ? 255 : LV_MAX(60, 255 - (d - half * 6 / 10) * 255 / (half * 5 / 10));
    lv_obj_set_style_opa(ch, (lv_opa_t)LV_MIN(255, opa), 0);
  }
}

static lv_obj_t* roundList(lv_obj_t* p, int w, int h) {
  lv_obj_t* l = lv_obj_create(p);
  lv_obj_remove_style_all(l);
  lv_obj_set_size(l, w, h);
  lv_obj_set_flex_flow(l, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(l, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(l, 10, 0);
  lv_obj_set_style_pad_ver(l, h / 3, 0);
  lv_obj_set_scroll_dir(l, LV_DIR_VER);
  lv_obj_set_scrollbar_mode(l, LV_SCROLLBAR_MODE_OFF);
  lv_obj_add_event_cb(l, roundFade, LV_EVENT_SCROLL, nullptr);
  return l;
}

static lv_obj_t* listRow(lv_obj_t* list, const String& text, bool active, lv_event_cb_t cb, intptr_t idx) {
  lv_obj_t* b = lv_button_create(list);
  lv_obj_set_width(b, lv_pct(100));
  lv_obj_set_height(b, LV_SIZE_CONTENT);
  lv_obj_set_style_radius(b, 18, 0);
  lv_obj_set_style_shadow_width(b, 0, 0);
  lv_obj_set_style_pad_all(b, 14, 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(active ? C_ACCENT : C_SURFACE), 0);
  lv_obj_set_style_bg_color(b, lv_color_hex(C_PRESSED), LV_STATE_PRESSED);
  lv_obj_t* l = label(b, &lv_font_montserrat_20, C_TEXT, text.c_str());
  lv_obj_set_width(l, lv_pct(100));
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_height(l, 24);
  lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void*)idx);
  return b;
}

// ---------- clock ----------

static void loadFace() {
  String id;
  { CfgLock c; id = cfg.face; }
  String err;
  if (!faceLoad(faceHost, id, &err)) {
    auto list = faceList();
    if (!list.empty() && faceLoad(faceHost, list[0].id, &err)) {
      CfgLock c;
      cfg.face = list[0].id;
    } else {
      // No faces on flash (LittleFS image not uploaded): plain digital fallback
      lv_obj_clean(faceHost);
      lv_obj_t* l = label(faceHost, &lv_font_montserrat_48, C_TEXT, "--:--");
      lv_obj_center(l);
      lv_obj_t* h = label(faceHost, &lv_font_montserrat_16, C_MUTED, "No faces installed.\nRun 'pio run -t uploadfs'");
      lv_obj_align(h, LV_ALIGN_CENTER, 0, 70);
    }
  }
  // A face can bring a sound that plays once when it appears (at start-up or when switched to)
  static String lastId;
  { CfgLock c; id = cfg.face; }
  if (id != lastId) {
    lastId = id;
    faceSoundPending = faceSound();
  }
  String name = id;
  for (auto& f : faceList()) if (f.id == id) name = f.name;
  if (lblFace) lv_label_set_text_fmt(lblFace, "Face: %s  " LV_SYMBOL_RIGHT, name.c_str());
}

static void nextFace(int dir = 1) {
  auto list = faceList();
  if (list.empty()) return;
  String cur;
  { CfgLock c; cur = cfg.face; }
  size_t i = 0;
  for (; i < list.size(); i++) if (list[i].id == cur) break;
  const FaceInfo& next = list[(i + dir + list.size() * 4) % list.size()];
  {
    CfgLock c;
    cfg.face = next.id;
    settingsSave();
  }
  loadFace();
  showToast(next.name);
}

// Ignored for the first few seconds: a hand resting on the screen while the watch is
// plugged in shouldn't change the face
static void onClockLongPress(lv_event_t*) { if (millis() > 5000) nextFace(); }

static void buildClock() {
  faceHost = lv_obj_create(tileClock);
  lv_obj_remove_style_all(faceHost);
  lv_obj_set_size(faceHost, 466, 466);
  lv_obj_center(faceHost);
  lv_obj_set_scrollable(faceHost, false);
  lv_obj_set_event_bubble(faceHost, true);
  lv_obj_add_event_cb(tileClock, onClockLongPress, LV_EVENT_LONG_PRESSED, nullptr);
}

// ---------- music ----------

static void onSource(lv_event_t* e) { playerSend(Cmd::SetSource, (int)(intptr_t)lv_event_get_user_data(e)); }
static void onPlay(lv_event_t*) { playerSend(Cmd::Toggle); }
static void onNext(lv_event_t*) { playerSend(Cmd::Next); }
static void onPrev(lv_event_t*) { playerSend(Cmd::Prev); }
static void onShuffle(lv_event_t*) { StateLock l; playerSend(Cmd::Shuffle, !player.shuffle); }
static void onVolume(lv_event_t* e) { playerSend(Cmd::Volume, lv_arc_get_value((lv_obj_t*)lv_event_get_target(e))); }

static void onPickStation(lv_event_t* e) { playerSend(Cmd::PlayStation, (int)(intptr_t)lv_event_get_user_data(e)); closeOverlay(); }
static void onPickTrack(lv_event_t* e) { playerSend(Cmd::PlayTrack, (int)(intptr_t)lv_event_get_user_data(e)); closeOverlay(); }

static void onList(lv_event_t*) {
  Source src;
  int cur;
  { StateLock l; src = player.source; cur = src == Source::Radio ? player.stationIdx : player.trackIdx; }
  lv_obj_t* list = nullptr;
  if (src == Source::Radio) {
    lv_obj_t* o = openOverlay("Stations");
    list = roundList(o, 340, 330);
    overlayScroll = list;
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
    CfgLock c;
    for (size_t i = 0; i < cfg.stations.size(); i++) listRow(list, cfg.stations[i].name, (int)i == cur, onPickStation, i);
    if (cfg.stations.empty()) label(list, &lv_font_montserrat_20, C_MUTED, "Add stations in the web settings");
  } else if (src == Source::SD) {
    lv_obj_t* o = openOverlay("Tracks");
    list = roundList(o, 340, 330);
    overlayScroll = list;
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -10);
    auto tracks = playerTracks();
    // Many buttons are slow to build; show a window around the current track
    int start = LV_MAX(0, cur - 50), end = LV_MIN((int)tracks.size(), start + 150);
    for (int i = start; i < end; i++) {
      String n = tracks[i].substring(tracks[i].lastIndexOf('/') + 1);
      listRow(list, n, i == cur, onPickTrack, i);
    }
    if (tracks.empty()) label(list, &lv_font_montserrat_20, C_MUTED, "No music found.\nPut files in /music on the SD card.");
  }
  if (list) lv_obj_send_event(list, LV_EVENT_SCROLL, nullptr);
}

static void buildMusic() {
  static const char* names[] = {"Radio", "SD card"};
  lv_obj_t* row = lv_obj_create(tileMusic);
  lv_obj_remove_style_all(row);
  lv_obj_set_size(row, 260, 44);
  lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_align(row, LV_ALIGN_CENTER, 0, -168);
  for (int i = 0; i < 2; i++) {
    lv_obj_t* b = lv_button_create(row);
    lv_obj_set_size(b, 116, 40);
    lv_obj_set_style_radius(b, 20, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_ACCENT), LV_STATE_CHECKED);
    lv_obj_t* l = label(b, &lv_font_montserrat_16, C_TEXT, names[i]);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, onSource, LV_EVENT_CLICKED, (void*)(intptr_t)(i + 1));
    srcBtn[i] = b;
  }

  artIcon = label(tileMusic, &lv_font_montserrat_48, C_MUTED, LV_SYMBOL_AUDIO);
  lv_obj_align(artIcon, LV_ALIGN_CENTER, 0, -58);

  btnSleep = roundBtn(tileMusic, "zZ", 52, onSleep);
  lv_obj_align(btnSleep, LV_ALIGN_CENTER, -125, -58);
  lblSleep = lv_obj_get_child(btnSleep, 0);
  btnShuffle = roundBtn(tileMusic, LV_SYMBOL_SHUFFLE, 52, onShuffle);
  lv_obj_align(btnShuffle, LV_ALIGN_CENTER, -125, -58);
  btnList = roundBtn(tileMusic, LV_SYMBOL_LIST, 52, onList);
  lv_obj_align(btnList, LV_ALIGN_CENTER, 125, -58);

  lblTitle = label(tileMusic, &lv_font_montserrat_24, C_TEXT, "Not playing");
  lv_obj_set_width(lblTitle, 330);
  lv_label_set_long_mode(lblTitle, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
  lv_obj_align(lblTitle, LV_ALIGN_CENTER, 0, 38);
  lblArtist = label(tileMusic, &lv_font_montserrat_20, C_MUTED);
  lv_obj_set_width(lblArtist, 320);
  lv_label_set_long_mode(lblArtist, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
  lv_obj_align(lblArtist, LV_ALIGN_CENTER, 0, 70);
  lblInfo = label(tileMusic, &lv_font_montserrat_14, C_MUTED);
  lv_obj_set_width(lblInfo, 290);
  lv_label_set_long_mode(lblInfo, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_align(lblInfo, LV_ALIGN_CENTER, 0, 98);

  lv_obj_t* prev = roundBtn(tileMusic, LV_SYMBOL_PREV, 62, onPrev);
  lv_obj_align(prev, LV_ALIGN_CENTER, -90, 158);
  btnPlay = roundBtn(tileMusic, LV_SYMBOL_PLAY, 84, onPlay);
  lv_obj_set_style_bg_color(btnPlay, lv_color_hex(C_ACCENT), 0);
  lv_obj_align(btnPlay, LV_ALIGN_CENTER, 0, 158);
  lblPlay = lv_obj_get_child(btnPlay, 0);
  lv_obj_t* next = roundBtn(tileMusic, LV_SYMBOL_NEXT, 62, onNext);
  lv_obj_align(next, LV_ALIGN_CENTER, 90, 158);

  // Volume: draggable arc on the right edge. Progress: thin arc on the left edge.
  volArc = lv_arc_create(tileMusic);
  lv_obj_set_size(volArc, 450, 450);
  lv_obj_center(volArc);
  lv_arc_set_rotation(volArc, 0);
  lv_arc_set_bg_angles(volArc, 315, 45);
  lv_arc_set_mode(volArc, LV_ARC_MODE_REVERSE);
  lv_arc_set_range(volArc, 0, 21);
  lv_obj_set_style_arc_width(volArc, 8, LV_PART_MAIN);
  lv_obj_set_style_arc_width(volArc, 8, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(volArc, lv_color_hex(C_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_arc_color(volArc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(volArc, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_set_style_pad_all(volArc, 4, LV_PART_KNOB);
  lv_obj_set_scroll_chain(volArc, false);
  lv_obj_set_adv_hittest(volArc, true);
  lv_obj_add_event_cb(volArc, onVolume, LV_EVENT_RELEASED, nullptr);
  lv_obj_t* volIcon = label(tileMusic, &lv_font_montserrat_16, C_MUTED, LV_SYMBOL_VOLUME_MAX);
  lv_obj_align(volIcon, LV_ALIGN_RIGHT_MID, -30, 0);

  progArc = lv_arc_create(tileMusic);
  lv_obj_set_size(progArc, 450, 450);
  lv_obj_center(progArc);
  lv_obj_remove_style(progArc, nullptr, LV_PART_KNOB);
  lv_obj_set_clickable(progArc, false);
  lv_arc_set_bg_angles(progArc, 135, 225);
  lv_arc_set_mode(progArc, LV_ARC_MODE_REVERSE);
  lv_arc_set_range(progArc, 0, 1000);
  lv_obj_set_style_arc_width(progArc, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_width(progArc, 4, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(progArc, lv_color_hex(C_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_arc_color(progArc, lv_color_hex(C_MUTED), LV_PART_INDICATOR);
}

static void updateMusic() {
  StateLock l;
  if (player.version == seenPlayer) return;
  seenPlayer = player.version;
  Source s = player.source;
  for (int i = 0; i < 2; i++) {
    if ((int)s == i + 1) lv_obj_add_state(srcBtn[i], LV_STATE_CHECKED);
    else lv_obj_remove_state(srcBtn[i], LV_STATE_CHECKED);
  }

  String title = player.title, artist = player.artist, info;
  if (s == Source::None) { title = "Choose a source"; artist = "Radio or SD card"; }
  else if (player.busy) { title = "Connecting..."; artist = player.station; }
  else if (s == Source::Radio) {
    if (title.isEmpty()) title = player.station.length() ? player.station : "Radio";
    else info = player.station;
    if (!player.playing && player.title.isEmpty()) artist = "Press play";
  } else if (s == Source::SD) {
    if (!player.sdMounted) { title = "No SD card"; artist = "Insert a card and tap " LV_SYMBOL_LIST; }
    else if (player.trackIdx < 0) { title = String(player.trackCount) + " tracks"; artist = "Press play"; }
    else info = String(player.trackIdx + 1) + " / " + String(player.trackCount) + (player.album.length() ? "  -  " + player.album : "");
  }
  bool err = player.error.length();
  if (err) info = player.error;
  lv_label_set_text(lblTitle, title.c_str());
  lv_label_set_text(lblArtist, artist.c_str());
  lv_label_set_text(lblInfo, info.c_str());
  lv_obj_set_style_text_color(lblInfo, lv_color_hex(err ? C_ERROR : C_MUTED), 0);
  lv_label_set_text(lblPlay, player.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
  lv_label_set_text(artIcon, s == Source::SD ? LV_SYMBOL_SD_CARD : LV_SYMBOL_AUDIO);
  lv_obj_set_hidden(btnShuffle, s != Source::SD);
  lv_obj_set_hidden(btnSleep, s == Source::SD);
  lv_obj_set_style_bg_color(btnShuffle, lv_color_hex(player.shuffle ? C_ACCENT : C_SURFACE), 0);
  lv_obj_set_hidden(btnList, !(s == Source::Radio || s == Source::SD));

  if (!lv_obj_has_state(volArc, LV_STATE_PRESSED)) lv_arc_set_value(volArc, player.volume);
  lv_arc_set_value(progArc, player.durMs ? (int)((uint64_t)player.posMs * 1000 / player.durMs) : 0);
}

// ---------- news ----------

static String ago(time_t ts) {
  if (!ts) return "";
  long d = time(nullptr) - ts;
  if (d < 0 || time(nullptr) < 1700000000) return "";
  if (d < 3600) return String(LV_MAX(1, d / 60)) + "m";
  if (d < 86400) return String(d / 3600) + "h";
  return String(d / 86400) + "d";
}

static void onNewsItem(lv_event_t* e) {
  int idx = (int)(intptr_t)lv_event_get_user_data(e);
  NewsItem it;
  {
    StateLock l;
    if (idx >= (int)news.size()) return;
    it = news[idx];
  }
  lv_obj_t* o = openOverlay((it.source + (it.ts ? "  -  " + ago(it.ts) + " ago" : "")).c_str());
  lv_obj_t* body = lv_obj_create(o);
  overlayScroll = body;
  lv_obj_remove_style_all(body);
  lv_obj_set_size(body, 340, 330);
  lv_obj_align(body, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(body, 14, 0);
  lv_obj_set_style_pad_bottom(body, 120, 0);
  lv_obj_set_scrollbar_mode(body, LV_SCROLLBAR_MODE_OFF);
  lv_obj_t* t = label(body, &lv_font_montserrat_24, C_TEXT, it.title.c_str());
  lv_obj_set_width(t, lv_pct(100));
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
  if (it.summary.length()) {
    lv_obj_t* s = label(body, &lv_font_montserrat_20, 0xC7C7CC, it.summary.c_str());
    lv_obj_set_width(s, lv_pct(100));
    lv_label_set_long_mode(s, LV_LABEL_LONG_MODE_WRAP);
  }
}

static void buildNews() {
  lv_obj_t* h = label(tileNews, &lv_font_montserrat_16, C_MUTED, "NEWS");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 34);
  newsList = roundList(tileNews, 350, 400);
  lv_obj_align(newsList, LV_ALIGN_CENTER, 0, 18);
  lblNewsStatus = label(tileNews, &lv_font_montserrat_20, C_MUTED, "");
  lv_obj_set_width(lblNewsStatus, 300);
  lv_obj_center(lblNewsStatus);
}

static void updateNews() {
  StateLock l;
  if (newsVersion == seenNews) return;
  seenNews = newsVersion;
  lv_label_set_text(lblNewsStatus, news.empty() ? newsStatus.c_str() : "");
  lv_obj_clean(newsList);
  for (size_t i = 0; i < news.size() && i < 40; i++) {
    const NewsItem& it = news[i];
    lv_obj_t* b = lv_button_create(newsList);
    lv_obj_set_width(b, lv_pct(100));
    lv_obj_set_height(b, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(b, 22, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_all(b, 16, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_flex_flow(b, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(b, 6, 0);
    String meta = it.source;
    String a = ago(it.ts);
    if (a.length()) meta += "  -  " + a;
    lv_obj_t* m = label(b, &lv_font_montserrat_14, C_ACCENT, meta.c_str());
    lv_obj_set_style_text_align(m, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_t* t = label(b, &lv_font_montserrat_20, C_TEXT, it.title.c_str());
    lv_obj_set_width(t, lv_pct(100));
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_event_cb(b, onNewsItem, LV_EVENT_CLICKED, (void*)(intptr_t)i);
  }
  lv_obj_scroll_to_y(newsList, 0, LV_ANIM_OFF);
  lv_obj_send_event(newsList, LV_EVENT_SCROLL, nullptr);
}

// ---------- photos ----------

static lv_obj_t *photoImg, *lblPhotos;
static lv_image_dsc_t photoDsc[2];
static uint8_t* photoBuf[2] = {nullptr, nullptr};
static int photoSlot = 0;
static std::vector<PhotoRef> photoList;
static uint32_t photoListVersion = 0;
static int photoIdx = -1;
static uint32_t photoShownAt = 0;

static void showPhoto(int step) {
  if (photoListVersion != photosVersion()) {
    photoListVersion = photosVersion();
    photoList = photosList();
    photoIdx = -1;
  }
  if (photoList.empty()) {
    lv_obj_set_hidden(photoImg, true);
    lv_label_set_text(lblPhotos, LV_SYMBOL_IMAGE "\n\nNo photos yet.\nAdd some on the web page\nunder Photos.");
    lv_obj_set_hidden(lblPhotos, false);
    return;
  }
  bool shuffle;
  { CfgLock c; shuffle = cfg.photoShuffle; }
  int n = photoList.size();
  // Try a few in case a file is unreadable
  for (int tries = 0; tries < 4 && tries < n; tries++) {
    if (shuffle && n > 1 && step > 0) { int r; do r = random(n); while (r == photoIdx); photoIdx = r; }
    else photoIdx = ((photoIdx < 0 ? 0 : photoIdx + step) % n + n) % n;
    size_t len; uint16_t w, h;
    uint8_t* jpg = photosRead(photoList[photoIdx], len, w, h);
    if (!jpg) { if (step == 0) step = 1; continue; }
    int slot = photoSlot ^ 1;
    lv_image_dsc_t& d = photoDsc[slot];
    memset(&d, 0, sizeof(d));
    d.header.magic = LV_IMAGE_HEADER_MAGIC;
    d.header.cf = LV_COLOR_FORMAT_RAW;
    d.header.w = w;
    d.header.h = h;
    d.data_size = len;
    d.data = jpg;
    lv_image_set_src(photoImg, &d);
    lv_obj_center(photoImg);
    lv_image_cache_drop(&photoDsc[photoSlot]);
    free(photoBuf[photoSlot]);
    photoBuf[photoSlot] = nullptr;
    photoBuf[slot] = jpg;
    photoSlot = slot;
    lv_obj_set_hidden(photoImg, false);
    lv_obj_set_hidden(lblPhotos, true);
    photoShownAt = millis();
    return;
  }
  lv_label_set_text(lblPhotos, "Couldn't open the photos.\nRe-upload them from the web page.");
  lv_obj_set_hidden(lblPhotos, false);
}

static void onPhotoTap(lv_event_t*) { showPhoto(1); }

static void buildPhotos() {
  lv_obj_set_style_bg_color(tilePhotos, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(tilePhotos, LV_OPA_COVER, 0);
  photoImg = lv_image_create(tilePhotos);
  lv_obj_center(photoImg);
  lv_obj_set_hidden(photoImg, true);
  lblPhotos = label(tilePhotos, &lv_font_montserrat_20, C_MUTED, "");
  lv_obj_center(lblPhotos);
  lv_obj_add_event_cb(tilePhotos, onPhotoTap, LV_EVENT_SHORT_CLICKED, nullptr);
}

static void photoTick() {
  if (lv_tileview_get_tile_active(tv) != tilePhotos || !halScreenIsOn()) return;
  uint16_t secs;
  { CfgLock c; secs = cfg.photoSeconds; }
  if (photoIdx < 0 || photoListVersion != photosVersion() || millis() - photoShownAt > secs * 1000UL) showPhoto(photoIdx < 0 ? 0 : 1);
}

// ---------- bible ----------

static lv_obj_t *bibHead, *bibText, *bibRef;
static int bibleShownDay = -2;      // day of the verse of the day on screen, -1 for a random verse
static uint32_t bibleShownAt = 0, bibleShownOnline = 0;

static void setVerse(const char* head, const char* text, const String& ref) {
  lv_label_set_text(bibHead, head);
  // Bigger text for short verses; long ones step down so they still fit the round screen
  size_t len = strlen(text);
  lv_obj_set_style_text_font(bibText, len <= 90 ? &lv_font_montserrat_28 : len <= 160 ? &lv_font_montserrat_24 : len <= 260 ? &lv_font_montserrat_20 : &lv_font_montserrat_16, 0);
  lv_label_set_text(bibText, text);
  lv_label_set_text(bibRef, ref.c_str());
}

static void showVerse(bool random) {
  bibleShownAt = millis();
  bibleShownOnline = bibleOnlineVersion();
  // Verse of the day: Bible Gateway's when we have it, otherwise one from the watch's own list
  OnlineVerse ov;
  if (!random && bibleOnline(ov)) {
    setVerse("VERSE OF THE DAY", ov.text.c_str(), ov.ref + "  " + ov.version);
    bibleShownDay = ov.day;
    return;
  }
  int n = bibleCount();
  if (!n) {
    setVerse("", "Verse of the day needs Wi-Fi.", "");
    bibleShownDay = -1;
    return;
  }
  int daily = bibleDailyIndex();
  bool rnd = random || daily < 0;
  int idx = rnd ? (int)(esp_random() % n) : daily;
  BibleVerse v = bibleGet(idx);
  setVerse(rnd ? "VERSE" : "VERSE OF THE DAY", v.text, String(v.ref) + "  KJV");
  bibleShownDay = rnd ? -1 : bibleToday();
}

static void onBibleTap(lv_event_t*) { showVerse(true); }

static void buildBible() {
  lv_obj_set_style_bg_color(tileBible, lv_color_hex(0x1A1208), 0);
  lv_obj_set_style_bg_grad_color(tileBible, lv_color_black(), 0);
  lv_obj_set_style_bg_grad_dir(tileBible, LV_GRAD_DIR_VER, 0);
  lv_obj_set_style_bg_opa(tileBible, LV_OPA_COVER, 0);
  lv_obj_t* col = lv_obj_create(tileBible);
  lv_obj_remove_style_all(col);
  lv_obj_set_size(col, 350, LV_SIZE_CONTENT);
  lv_obj_center(col);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(col, 16, 0);
  lv_obj_remove_flag(col, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(col, LV_OBJ_FLAG_EVENT_BUBBLE);
  bibHead = label(col, &lv_font_montserrat_16, 0xC9A227, "");
  lv_obj_set_style_text_letter_space(bibHead, 3, 0);
  bibText = label(col, &lv_font_montserrat_24, 0xF5EFE0, "");
  lv_label_set_long_mode(bibText, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(bibText, 350);
  lv_obj_set_style_text_line_space(bibText, 4, 0);
  bibRef = label(col, &lv_font_montserrat_20, 0xC9A227, "");
  lv_obj_add_event_cb(tileBible, onBibleTap, LV_EVENT_SHORT_CLICKED, nullptr);
}

static void bibleTick() {
  if (lv_tileview_get_tile_active(tv) != tileBible || !halScreenIsOn()) return;
  bool rnd;
  uint16_t mins;
  { CfgLock c; rnd = cfg.bibleRandom; mins = cfg.bibleMinutes; }
  uint32_t age = millis() - bibleShownAt;
  if (bibleShownDay == -2) showVerse(rnd);
  else if (rnd) { if (age > mins * 60000UL) showVerse(true); }
  // Daily mode: follow the date and newly downloaded verses, and go back to
  // today's verse a while after a tap
  else if (bibleShownDay >= 0 ? bibleShownDay != bibleToday() || bibleShownOnline != bibleOnlineVersion()
                              : age > 5 * 60000UL) showVerse(false);
}

// ---------- weather ----------

static lv_obj_t *wxPlace, *wxIcon, *wxTemp, *wxCond, *wxHiLo, *wxMore, *wxMsg;
static lv_obj_t *wxDay[5], *wxDayIcon[5], *wxDayTemp[5];
static uint32_t seenWeather = UINT32_MAX;

static String deg(float v) { return String((int)lroundf(v)) + "\xC2\xB0"; }

static void buildWeather() {
  wxPlace = label(tileWeather, &lv_font_montserrat_16, C_MUTED, "WEATHER");
  lv_obj_align(wxPlace, LV_ALIGN_TOP_MID, 0, 40);
  wxIcon = lv_image_create(tileWeather);
  lv_obj_align(wxIcon, LV_ALIGN_CENTER, -70, -86);
  wxTemp = label(tileWeather, &lv_font_montserrat_48, C_TEXT, "");
  lv_obj_set_style_transform_scale(wxTemp, 340, 0);
  lv_obj_set_style_transform_pivot_x(wxTemp, 0, 0);
  lv_obj_set_style_transform_pivot_y(wxTemp, lv_pct(50), 0);
  lv_obj_align(wxTemp, LV_ALIGN_LEFT_MID, 233 - 10, -86);
  wxCond = label(tileWeather, &lv_font_montserrat_24, C_TEXT, "");
  lv_obj_align(wxCond, LV_ALIGN_CENTER, 0, -18);
  wxHiLo = label(tileWeather, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_align(wxHiLo, LV_ALIGN_CENTER, 0, 14);
  wxMore = label(tileWeather, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(wxMore, LV_ALIGN_CENTER, 0, 38);
  for (int i = 0; i < 5; i++) {
    int x = -136 + i * 68;
    wxDay[i] = label(tileWeather, &lv_font_montserrat_14, C_MUTED, "");
    lv_obj_align(wxDay[i], LV_ALIGN_CENTER, x, 82);
    wxDayIcon[i] = lv_image_create(tileWeather);
    lv_obj_align(wxDayIcon[i], LV_ALIGN_CENTER, x, 112);
    wxDayTemp[i] = label(tileWeather, &lv_font_montserrat_14, C_TEXT, "");
    lv_obj_align(wxDayTemp[i], LV_ALIGN_CENTER, x, 144);
  }
  wxMsg = label(tileWeather, &lv_font_montserrat_20, C_MUTED, "Loading weather...");
  lv_obj_set_width(wxMsg, 320);
  lv_label_set_long_mode(wxMsg, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_center(wxMsg);
}

static void updateWeather() {
  StateLock l;
  if (weather.version == seenWeather) return;
  seenWeather = weather.version;
  bool ok = weather.valid;
  lv_obj_set_hidden(wxMsg, ok);
  if (!ok) { lv_label_set_text(wxMsg, weather.error.length() ? weather.error.c_str() : "Loading weather..."); }
  for (lv_obj_t* o : {wxIcon, wxTemp, wxCond, wxHiLo, wxMore}) lv_obj_set_hidden(o, !ok);
  String place;
  bool f;
  { CfgLock c; place = cfg.place; f = cfg.fahrenheit; }
  place.toUpperCase();
  lv_label_set_text(wxPlace, place.length() ? place.c_str() : "WEATHER");
  if (!ok) { for (int i = 0; i < 5; i++) { lv_label_set_text(wxDay[i], ""); lv_label_set_text(wxDayTemp[i], ""); lv_obj_set_hidden(wxDayIcon[i], true); } return; }
  lv_image_set_src(wxIcon, weatherIcon(weather.code, weather.day, true));
  lv_label_set_text(wxTemp, deg(weather.temp).c_str());
  lv_label_set_text(wxCond, weatherText(weather.code));
  String hilo = "H " + deg(weather.days[0].hi) + "   L " + deg(weather.days[0].lo) + "   Feels " + deg(weather.feels);
  lv_label_set_text(wxHiLo, hilo.c_str());
  String more = String(weather.humidity) + "% humidity   " + String((int)lroundf(weather.wind)) + (f ? " mph" : " km/h") + " wind";
  if (weather.days[0].rain >= 20) more += "   " + String(weather.days[0].rain) + "% rain";
  lv_label_set_text(wxMore, more.c_str());
  static const char* dn[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  for (int i = 0; i < 5; i++) {
    bool has = i + 1 < weather.ndays;
    lv_obj_set_hidden(wxDayIcon[i], !has);
    if (!has) { lv_label_set_text(wxDay[i], ""); lv_label_set_text(wxDayTemp[i], ""); continue; }
    const DayForecast& d = weather.days[i + 1];
    lv_label_set_text(wxDay[i], dn[d.wday]);
    lv_image_set_src(wxDayIcon[i], weatherIcon(d.code, true, false));
    lv_label_set_text(wxDayTemp[i], (deg(d.hi) + " " + deg(d.lo)).c_str());
  }
}

// ---------- today (date, next alarm, countdowns) ----------

static lv_obj_t *tdDay, *tdDate, *tdAlarm, *tdList;

static void buildToday() {
  lv_obj_t* h = label(tileToday, &lv_font_montserrat_16, C_MUTED, "TODAY");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 40);
  tdDay = label(tileToday, &lv_font_montserrat_40, C_TEXT, "");
  lv_obj_align(tdDay, LV_ALIGN_CENTER, 0, -128);
  tdDate = label(tileToday, &lv_font_montserrat_20, C_ACCENT, "");
  lv_obj_align(tdDate, LV_ALIGN_CENTER, 0, -88);
  tdAlarm = label(tileToday, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_align(tdAlarm, LV_ALIGN_CENTER, 0, -56);
  tdList = lv_obj_create(tileToday);
  lv_obj_remove_style_all(tdList);
  lv_obj_set_size(tdList, 330, 250);
  lv_obj_align(tdList, LV_ALIGN_CENTER, 0, 82);
  lv_obj_set_flex_flow(tdList, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(tdList, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_row(tdList, 8, 0);
  lv_obj_set_scrollbar_mode(tdList, LV_SCROLLBAR_MODE_OFF);
}

static void updateToday() {
  static String last;
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm t;
  localtime_r(&now, &t);
  char day[16], date[32];
  strftime(day, sizeof(day), "%A", &t);
  strftime(date, sizeof(date), "%B %e, %Y", &t);
  String alarm = nextAlarmText();
  auto cds = countdownLines();
  // Upcoming events: rest of today and tomorrow, up to 5
  std::vector<String> evs;
  String calMsg;
  {
    StateLock l;
    time_t endTomorrow;
    struct tm m = t;
    m.tm_mday += 2; m.tm_hour = m.tm_min = m.tm_sec = 0; m.tm_isdst = -1;
    endTomorrow = mktime(&m);
    for (auto& e : events) {
      if (e.end < now || e.start >= endTomorrow || evs.size() >= 5) continue;
      struct tm s;
      localtime_r(&e.start, &s);
      bool tomorrow = s.tm_yday != t.tm_yday;
      char tb[16];
      bool h24;
      { CfgLock c; h24 = cfg.h24; }
      if (e.allDay) snprintf(tb, sizeof(tb), "All day");
      else if (h24) snprintf(tb, sizeof(tb), "%02d:%02d", s.tm_hour, s.tm_min);
      else snprintf(tb, sizeof(tb), "%d:%02d %s", s.tm_hour % 12 == 0 ? 12 : s.tm_hour % 12, s.tm_min, s.tm_hour < 12 ? "AM" : "PM");
      evs.push_back(String(tomorrow ? "Tmrw " : "") + tb + "  " + e.title);
    }
    calMsg = calendarStatus;
  }
  String sig = String(day) + date + alarm + calMsg;
  for (auto& c : cds) sig += c;
  for (auto& e : evs) sig += e;
  if (sig == last) return;
  last = sig;
  lv_label_set_text(tdDay, day);
  lv_label_set_text(tdDate, date);
  lv_label_set_text(tdAlarm, alarm.length() ? (String(LV_SYMBOL_BELL " Next alarm: ") + alarm).c_str() : "No alarms set");
  lv_obj_clean(tdList);
  for (auto& e : evs) {
    lv_obj_t* l = label(tdList, &lv_font_montserrat_20, C_TEXT, e.c_str());
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_height(l, 26);
  }
  if (calMsg.length()) label(tdList, &lv_font_montserrat_14, C_ERROR, calMsg.c_str());
  for (auto& c : cds) {
    lv_obj_t* row = lv_obj_create(tdList);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(C_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 16, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_t* l = label(row, &lv_font_montserrat_20, C_TEXT, c.c_str());
    lv_obj_set_width(l, lv_pct(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  }
  if (cds.empty() && evs.empty()) label(tdList, &lv_font_montserrat_16, C_MUTED, "Add calendars and countdowns\non the web page");
}

// ---------- week ----------

static lv_obj_t* wkList;

static void buildWeek() {
  lv_obj_t* h = label(tileWeek, &lv_font_montserrat_16, C_MUTED, "THIS WEEK");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 40);
  wkList = roundList(tileWeek, 340, 380);
  lv_obj_set_style_pad_row(wkList, 6, 0);
  lv_obj_align(wkList, LV_ALIGN_CENTER, 0, 20);
}

static String clockText(const struct tm& s, bool h24) {
  char b[16];
  if (h24) snprintf(b, sizeof(b), "%02d:%02d", s.tm_hour, s.tm_min);
  else snprintf(b, sizeof(b), "%d:%02d %s", s.tm_hour % 12 == 0 ? 12 : s.tm_hour % 12, s.tm_min, s.tm_hour < 12 ? "AM" : "PM");
  return b;
}

// The next 7 days, each with its events (all-day and multi-day events show on every day they cover)
static void updateWeek() {
  static uint32_t seenVersion = UINT32_MAX;
  static int seenDay = -1;
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm t;
  localtime_r(&now, &t);
  uint32_t ver;
  { StateLock l; ver = eventsVersion; }
  if (ver == seenVersion && t.tm_yday == seenDay) return;
  seenVersion = ver;
  seenDay = t.tm_yday;

  bool h24;
  { CfgLock c; h24 = cfg.h24; }
  String calMsg;
  std::vector<CalEvent> evs;
  { StateLock l; evs = events; calMsg = calendarStatus; }

  lv_obj_clean(wkList);
  if (calMsg.length()) label(wkList, &lv_font_montserrat_14, C_ERROR, calMsg.c_str());
  int total = 0;
  for (int d = 0; d < 7; d++) {
    struct tm ds = t;
    ds.tm_mday += d; ds.tm_hour = ds.tm_min = ds.tm_sec = 0; ds.tm_isdst = -1;
    time_t dayStart = mktime(&ds);
    struct tm de = ds;
    de.tm_mday += 1; de.tm_isdst = -1;
    time_t dayEnd = mktime(&de);
    char hb[32];
    strftime(hb, sizeof(hb), d == 0 ? "Today  -  %a %b %e" : d == 1 ? "Tomorrow  -  %a %b %e" : "%A %b %e", &ds);
    lv_obj_t* hd = label(wkList, &lv_font_montserrat_16, C_ACCENT, hb);
    lv_obj_set_width(hd, lv_pct(100));
    lv_obj_set_style_text_align(hd, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_pad_top(hd, d ? 10 : 0, 0);
    int n = 0;
    for (auto& e : evs) {
      time_t end = e.end > e.start ? e.end : e.start + 1;
      if (e.start >= dayEnd || end <= dayStart) continue;
      if (d == 0 && end < now) continue;  // already over today
      struct tm s;
      localtime_r(&e.start, &s);
      String when = e.allDay || e.start < dayStart ? String("All day") : clockText(s, h24);
      lv_obj_t* l = label(wkList, &lv_font_montserrat_20, C_TEXT, (when + "  " + e.title).c_str());
      lv_obj_set_width(l, lv_pct(100));
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_height(l, 26);
      n++;
    }
    if (!n) {
      lv_obj_t* l = label(wkList, &lv_font_montserrat_16, C_MUTED, "Nothing scheduled");
      lv_obj_set_width(l, lv_pct(100));
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_LEFT, 0);
    }
    total += n;
  }
  if (!total && !calMsg.length()) {
    std::vector<CalendarSrc> cals;
    { CfgLock c; cals = cfg.calendars; }
    if (cals.empty()) label(wkList, &lv_font_montserrat_16, C_MUTED, "\nAdd a calendar on the web page\n(Settings > Calendar)");
  }
  lv_obj_scroll_to_y(wkList, 0, LV_ANIM_OFF);
  lv_obj_send_event(wkList, LV_EVENT_SCROLL, nullptr);
}

// ---------- sports ----------

static lv_obj_t *spList, *spMsg;
static uint32_t seenGames = UINT32_MAX;

static void buildSports() {
  lv_obj_t* h = label(tileSports, &lv_font_montserrat_16, C_MUTED, "SCORES");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 40);
  spList = roundList(tileSports, 340, 380);
  lv_obj_align(spList, LV_ALIGN_CENTER, 0, 20);
  spMsg = label(tileSports, &lv_font_montserrat_20, C_MUTED, "Pick your teams on the web page\n(Settings > Sports)");
  lv_obj_set_width(spMsg, 320);
  lv_label_set_long_mode(spMsg, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_center(spMsg);
}

static void updateSports() {
  StateLock l;
  if (gamesVersion == seenGames) return;
  seenGames = gamesVersion;
  lv_obj_clean(spList);
  bool anyTeams;
  { CfgLock c; anyTeams = !cfg.teams.empty(); }
  lv_obj_set_hidden(spMsg, !games.empty());
  if (games.empty()) {
    lv_label_set_text(spMsg, anyTeams ? "No games today for your teams" : "Pick your teams on the web page\n(Settings > Sports)");
    return;
  }
  for (auto& g : games) {
    lv_obj_t* row = lv_obj_create(spList);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(g.state == 1 ? 0x1E2A44 : C_SURFACE), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 20, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(row, 4, 0);
    String score = g.state == 0 ? g.away + "  at  " + g.home
                                : g.away + "  " + String(g.awayScore) + "  -  " + String(g.homeScore) + "  " + g.home;
    label(row, &lv_font_montserrat_24, C_TEXT, score.c_str());
    String detail = g.league + "  -  " + g.detail;
    label(row, &lv_font_montserrat_14, g.state == 1 ? C_GREEN : C_MUTED, detail.c_str());
  }
  lv_obj_send_event(spList, LV_EVENT_SCROLL, nullptr);
}

// ---------- timer ----------

static lv_obj_t *tmArc, *tmTime, *tmState, *tmPlayLbl;

static void onTimerToggle(lv_event_t*) { timerToggle(); }
static void onTimerReset(lv_event_t*) { timerReset(); }
static void onTimerMinus(lv_event_t*) { timerAdjust(-60); }
static void onTimerPlus(lv_event_t*) { timerAdjust(60); }

static void buildTimer() {
  lv_obj_t* h = label(tileTimer, &lv_font_montserrat_16, C_MUTED, "TIMER");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 40);
  tmArc = lv_arc_create(tileTimer);
  lv_obj_set_size(tmArc, 430, 430);
  lv_obj_center(tmArc);
  lv_obj_remove_style(tmArc, nullptr, LV_PART_KNOB);
  lv_obj_set_clickable(tmArc, false);
  lv_arc_set_bg_angles(tmArc, 0, 360);
  lv_arc_set_rotation(tmArc, 270);
  lv_arc_set_range(tmArc, 0, 1000);
  lv_obj_set_style_arc_width(tmArc, 12, LV_PART_MAIN);
  lv_obj_set_style_arc_width(tmArc, 12, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(tmArc, lv_color_hex(C_SURFACE), LV_PART_MAIN);
  lv_obj_set_style_arc_color(tmArc, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  tmTime = label(tileTimer, &lv_font_montserrat_48, C_TEXT, "5:00");
  lv_obj_set_style_transform_scale(tmTime, 400, 0);
  lv_obj_set_style_transform_pivot_x(tmTime, lv_pct(50), 0);
  lv_obj_set_style_transform_pivot_y(tmTime, lv_pct(50), 0);
  lv_obj_align(tmTime, LV_ALIGN_CENTER, 0, -40);
  tmState = label(tileTimer, &lv_font_montserrat_16, C_MUTED, "");
  lv_obj_align(tmState, LV_ALIGN_CENTER, 0, 30);
  lv_obj_t* minus = roundBtn(tileTimer, LV_SYMBOL_MINUS, 58, onTimerMinus);
  lv_obj_align(minus, LV_ALIGN_CENTER, -96, 100);
  lv_obj_t* play = roundBtn(tileTimer, LV_SYMBOL_PLAY, 80, onTimerToggle);
  lv_obj_set_style_bg_color(play, lv_color_hex(C_ACCENT), 0);
  lv_obj_align(play, LV_ALIGN_CENTER, 0, 100);
  tmPlayLbl = lv_obj_get_child(play, 0);
  lv_obj_t* plus = roundBtn(tileTimer, LV_SYMBOL_PLUS, 58, onTimerPlus);
  lv_obj_align(plus, LV_ALIGN_CENTER, 96, 100);
  lv_obj_t* reset = roundBtn(tileTimer, LV_SYMBOL_REFRESH, 46, onTimerReset);
  lv_obj_align(reset, LV_ALIGN_CENTER, 0, 172);
}

static void updateTimer() {
  TimerInfo t = appsTimer();
  static uint32_t lastRem = UINT32_MAX, lastTot = 0;
  static bool lastRun = false;
  if (t.remainingSec == lastRem && t.running == lastRun && t.totalSec == lastTot) return;
  lastRem = t.remainingSec; lastRun = t.running; lastTot = t.totalSec;
  char b[16];
  uint32_t r = t.remainingSec;
  if (r >= 3600) snprintf(b, sizeof(b), "%u:%02u:%02u", (unsigned)(r / 3600), (unsigned)(r / 60 % 60), (unsigned)(r % 60));
  else snprintf(b, sizeof(b), "%u:%02u", (unsigned)(r / 60), (unsigned)(r % 60));
  lv_label_set_text(tmTime, b);
  lv_arc_set_value(tmArc, t.totalSec ? (int)(t.remainingSec * 1000 / t.totalSec) : 0);
  lv_label_set_text(tmState, t.running ? "Running" : t.remainingSec == t.totalSec ? "Press play to start" : "Paused");
  lv_label_set_text(tmPlayLbl, t.running ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// ---------- radio sleep timer (button on the Music screen) ----------

static void onSleep(lv_event_t*) {
  int m = sleepCycle();
  showToast(m ? "Music stops in " + String(m) + " min" : String("Sleep timer off"));
}

static void updateSleep() {
  static int last = -1;
  int m = sleepMinutesLeft();
  if (m == last) return;
  last = m;
  lv_label_set_text(lblSleep, m ? String(m).c_str() : "zZ");
  lv_obj_set_style_bg_color(btnSleep, lv_color_hex(m ? C_ACCENT : C_SURFACE), 0);
}

// ---------- alarm / timer pop-up ----------

static lv_obj_t* alertScreen = nullptr;
static uint32_t shownAlertId = 0;
static uint8_t* msgPhoto = nullptr;
static lv_image_dsc_t msgDsc;

static void freeMsgPhoto() {
  if (!msgPhoto) return;
  lv_image_cache_drop(&msgDsc);
  free(msgPhoto);
  msgPhoto = nullptr;
}

static void onAlertStop(lv_event_t*) { appsAlertStop(); }
static void onAlertSnooze(lv_event_t*) { appsAlertSnooze(); }

static void updateAlert() {
  AlertInfo a = appsCurrentAlert();
  if (a.kind == AlertKind::None) {
    if (alertScreen) { lv_obj_delete(alertScreen); alertScreen = nullptr; shownAlertId = 0; freeMsgPhoto(); }
    return;
  }
  if (a.id == shownAlertId) return;
  shownAlertId = a.id;
  if (alertScreen) lv_obj_delete(alertScreen);
  freeMsgPhoto();
  closeOverlay();
  halScreenOn(true);
  alertScreen = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(alertScreen);
  lv_obj_set_size(alertScreen, 466, 466);
  lv_obj_set_style_bg_color(alertScreen, lv_color_hex(0x0A1630), 0);
  lv_obj_set_style_bg_opa(alertScreen, LV_OPA_COVER, 0);
  lv_obj_set_clickable(alertScreen, true);
  if (a.photo) {
    // Photo sent with the message fills the screen behind a dark band for the text
    File f = LittleFS.open("/msg.jpg", "r");
    if (f) {
      size_t len = f.size();
      msgPhoto = (uint8_t*)ps_malloc(len);
      if (msgPhoto && f.read(msgPhoto, len) == len) {
        memset(&msgDsc, 0, sizeof(msgDsc));
        msgDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
        msgDsc.header.cf = LV_COLOR_FORMAT_RAW;
        msgDsc.header.w = 466;
        msgDsc.header.h = 466;
        msgDsc.data_size = len;
        msgDsc.data = msgPhoto;
        lv_obj_t* img = lv_image_create(alertScreen);
        lv_image_set_src(img, &msgDsc);
        lv_obj_center(img);
        lv_obj_t* band = lv_obj_create(alertScreen);
        lv_obj_remove_style_all(band);
        lv_obj_set_size(band, 466, 230);
        lv_obj_align(band, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_set_style_bg_color(band, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_70, 0);
      } else { free(msgPhoto); msgPhoto = nullptr; }
      f.close();
    }
  }
  lv_obj_t* icon = label(alertScreen, &lv_font_montserrat_48, C_ACCENT, a.kind == AlertKind::Timer ? LV_SYMBOL_BELL : LV_SYMBOL_BELL);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -120);
  lv_obj_t* t = label(alertScreen, &lv_font_montserrat_32, C_TEXT, a.title.c_str());
  lv_obj_set_width(t, 360);
  lv_label_set_long_mode(t, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -50);
  lv_obj_t* d = label(alertScreen, &lv_font_montserrat_20, C_MUTED, a.detail.c_str());
  lv_obj_set_width(d, 340);
  lv_label_set_long_mode(d, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(d, LV_ALIGN_CENTER, 0, 4);
  auto pill = [&](const char* txt, uint32_t bg, int x, lv_event_cb_t cb) {
    lv_obj_t* b = lv_button_create(alertScreen);
    lv_obj_set_size(b, 140, 60);
    lv_obj_set_style_radius(b, 30, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_align(b, LV_ALIGN_CENTER, x, 100);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_center(label(b, &lv_font_montserrat_20, C_TEXT, txt));
  };
  if (a.canSnooze) {
    pill("Snooze", C_SURFACE, -78, onAlertSnooze);
    pill("Stop", C_ACCENT, 78, onAlertStop);
  } else {
    pill(a.kind == AlertKind::Message ? "OK" : "Stop", C_ACCENT, 0, onAlertStop);
  }
}

// ---------- settings ----------

static void onBrightness(lv_event_t* e) {
  int v = lv_slider_get_value((lv_obj_t*)lv_event_get_target(e));
  { CfgLock c; cfg.brightness = v; }
  halSetBrightness(v);
  appliedBrightness = v;
}

static void onBrightnessDone(lv_event_t*) { CfgLock c; settingsSave(); }
static void onFaceBtn(lv_event_t*) { nextFace(); }
static void onUsbBtn(lv_event_t*) {
  String err;
  if (!storageUsbStart(&err)) showToast(err);
}

static void buildSettings() {
  lv_obj_t* h = label(tileSet, &lv_font_montserrat_16, C_MUTED, "SETTINGS");
  lv_obj_align(h, LV_ALIGN_TOP_MID, 0, 34);

  qr = lv_qrcode_create(tileSet);
  lv_qrcode_set_size(qr, 116);
  lv_qrcode_set_dark_color(qr, lv_color_black());
  lv_qrcode_set_light_color(qr, lv_color_white());
  lv_obj_set_style_border_color(qr, lv_color_white(), 0);
  lv_obj_set_style_border_width(qr, 6, 0);
  lv_obj_align(qr, LV_ALIGN_CENTER, 0, -100);

  lblNet = label(tileSet, &lv_font_montserrat_16, C_TEXT, "");
  lv_obj_set_width(lblNet, 330);
  lv_label_set_long_mode(lblNet, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_align(lblNet, LV_ALIGN_CENTER, 0, 10);

  lv_obj_t* sun = label(tileSet, &lv_font_montserrat_20, C_MUTED, LV_SYMBOL_EYE_OPEN);
  lv_obj_align(sun, LV_ALIGN_CENTER, -150, 88);
  briSlider = lv_slider_create(tileSet);
  lv_obj_set_width(briSlider, 250);
  lv_slider_set_range(briSlider, 10, 255);
  lv_obj_set_style_bg_color(briSlider, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(briSlider, lv_color_hex(C_TEXT), LV_PART_KNOB);
  lv_obj_align(briSlider, LV_ALIGN_CENTER, 10, 88);
  lv_obj_set_scroll_chain(briSlider, false);
  lv_obj_add_event_cb(briSlider, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);
  lv_obj_add_event_cb(briSlider, onBrightnessDone, LV_EVENT_RELEASED, nullptr);

  lv_obj_t* fb = lv_button_create(tileSet);
  lv_obj_set_size(fb, 150, 48);
  lv_obj_set_style_radius(fb, 24, 0);
  lv_obj_set_style_shadow_width(fb, 0, 0);
  lv_obj_set_style_bg_color(fb, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_bg_color(fb, lv_color_hex(C_PRESSED), LV_STATE_PRESSED);
  lv_obj_align(fb, LV_ALIGN_CENTER, -80, 148);
  lv_obj_add_event_cb(fb, onFaceBtn, LV_EVENT_CLICKED, nullptr);
  lblFace = label(fb, &lv_font_montserrat_16, C_TEXT, "Face");
  lv_obj_set_width(lblFace, 130);
  lv_label_set_long_mode(lblFace, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_center(lblFace);

  lv_obj_t* ub = lv_button_create(tileSet);
  lv_obj_set_size(ub, 150, 48);
  lv_obj_set_style_radius(ub, 24, 0);
  lv_obj_set_style_shadow_width(ub, 0, 0);
  lv_obj_set_style_bg_color(ub, lv_color_hex(C_SURFACE), 0);
  lv_obj_set_style_bg_color(ub, lv_color_hex(C_PRESSED), LV_STATE_PRESSED);
  lv_obj_align(ub, LV_ALIGN_CENTER, 80, 148);
  lv_obj_add_event_cb(ub, onUsbBtn, LV_EVENT_CLICKED, nullptr);
  lv_obj_center(label(ub, &lv_font_montserrat_16, C_TEXT, LV_SYMBOL_USB " USB drive"));

  lblPower = label(tileSet, &lv_font_montserrat_14, C_MUTED, "");
  lv_obj_align(lblPower, LV_ALIGN_CENTER, 0, 196);
}

// Full-screen notice while the PC owns the SD card
static lv_obj_t* usbScreen = nullptr;

static void onUsbDone(lv_event_t*) { storageUsbStop(); }

static void updateUsbScreen() {
  bool active = storageUsbActive();
  if (active == (usbScreen != nullptr)) return;
  if (!active) { lv_obj_delete(usbScreen); usbScreen = nullptr; showToast("SD card back on the watch"); return; }
  closeOverlay();
  usbScreen = lv_obj_create(lv_layer_top());
  lv_obj_remove_style_all(usbScreen);
  lv_obj_set_size(usbScreen, 466, 466);
  lv_obj_set_style_bg_color(usbScreen, lv_color_hex(C_BG), 0);
  lv_obj_set_style_bg_opa(usbScreen, LV_OPA_COVER, 0);
  lv_obj_set_clickable(usbScreen, true);
  lv_obj_t* icon = label(usbScreen, &lv_font_montserrat_48, C_ACCENT, LV_SYMBOL_USB);
  lv_obj_align(icon, LV_ALIGN_CENTER, 0, -110);
  lv_obj_t* t = label(usbScreen, &lv_font_montserrat_24, C_TEXT, "USB drive mode");
  lv_obj_align(t, LV_ALIGN_CENTER, 0, -40);
  lv_obj_t* m = label(usbScreen, &lv_font_montserrat_16, C_MUTED,
                      "The SD card is open on your computer.\nCopy music into the music folder,\n"
                      "then eject the drive in Windows\nor tap Done.");
  lv_obj_align(m, LV_ALIGN_CENTER, 0, 30);
  lv_obj_t* done = lv_button_create(usbScreen);
  lv_obj_set_size(done, 160, 56);
  lv_obj_set_style_radius(done, 28, 0);
  lv_obj_set_style_bg_color(done, lv_color_hex(C_ACCENT), 0);
  lv_obj_align(done, LV_ALIGN_CENTER, 0, 130);
  lv_obj_add_event_cb(done, onUsbDone, LV_EVENT_CLICKED, nullptr);
  lv_obj_center(label(done, &lv_font_montserrat_20, C_TEXT, "Done"));
}

static void updateNet() {
  String text, url;
  {
    StateLock l;
    if (net.version == seenNet) return;
    seenNet = net.version;
    String host;
    { CfgLock c; host = cfg.hostname; }
    if (net.connected) {
      url = "http://" + net.ip + "/";
      text = String(LV_SYMBOL_WIFI " ") + net.ssid + "\nhttp://" + host + ".local\n" + net.ip;
    } else if (net.apMode) {
      url = "WIFI:T:WPA;S:" + net.apName + ";P:clockhub;;";
      text = "Scan to join " + net.apName + "\n(password: clockhub)\nthen open http://192.168.4.1";
    } else {
      text = "Connecting to Wi-Fi...";
    }
  }
  lv_label_set_text(lblNet, text.c_str());
  if (url != qrText) {
    qrText = url;
    lv_obj_set_hidden(qr, url.isEmpty());
    if (url.length()) lv_qrcode_update(qr, url.c_str(), url.length());
  }
}

static void updatePower() {
  halUpdatePower();
  if (!power.present) { lv_label_set_text(lblPower, ""); return; }
  String s;
  if (power.battery) s = String(power.charging ? LV_SYMBOL_CHARGE " " : LV_SYMBOL_BATTERY_FULL " ") + power.percent + "%";
  else s = LV_SYMBOL_USB " USB power";
  lv_label_set_text(lblPower, s.c_str());
}

// ---------- main ----------

static void applySettings() {
  uint8_t b;
  { CfgLock c; b = cfg.brightness; }
  lv_slider_set_value(briSlider, b, LV_ANIM_OFF);
  appliedBrightness = 0;  // force re-apply in the idle logic
  seenNet = UINT32_MAX;
}

void uiInit() {
  lv_obj_t* scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
  lv_obj_set_style_text_color(scr, lv_color_hex(C_TEXT), 0);

  tv = lv_tileview_create(scr);
  lv_obj_set_style_bg_color(tv, lv_color_hex(C_BG), 0);
  lv_obj_set_scrollbar_mode(tv, LV_SCROLLBAR_MODE_OFF);
  // Which screens to show: clock first, settings always present
  {
    std::vector<String> ids;
    { CfgLock c; ids = cfg.screens; }
    if (ids.empty()) ids = {"clock", "weather", "today", "week", "sports", "music", "news", "photos", "timer", "bible", "settings"};
    order.clear();
    order.push_back(Scr::Clock);
    for (auto& id : ids)
      for (int i = 1; i < (int)Scr::Count; i++)
        if (id == kScrIds[i] && std::find(order.begin(), order.end(), (Scr)i) == order.end()) order.push_back((Scr)i);
    if (std::find(order.begin(), order.end(), Scr::Settings) == order.end()) order.push_back(Scr::Settings);
  }
  lv_obj_t* offscreen = lv_obj_create(scr);
  lv_obj_set_hidden(offscreen, true);
  for (int i = 0; i < (int)Scr::Count; i++) {
    auto it = std::find(order.begin(), order.end(), (Scr)i);
    if (it == order.end()) {
      scrObj[i] = lv_obj_create(offscreen);
      lv_obj_set_size(scrObj[i], 466, 466);
      continue;
    }
    int col = it - order.begin();
    lv_dir_t dir = col == 0 ? LV_DIR_RIGHT : col == (int)order.size() - 1 ? LV_DIR_LEFT : LV_DIR_HOR;
    scrObj[i] = lv_tileview_add_tile(tv, col, 0, dir);
    lv_obj_set_scrollbar_mode(scrObj[i], LV_SCROLLBAR_MODE_OFF);
  }
  tileClock = scrObj[(int)Scr::Clock];
  tileWeather = scrObj[(int)Scr::Weather];
  tileToday = scrObj[(int)Scr::Today];
  tileWeek = scrObj[(int)Scr::Week];
  tileSports = scrObj[(int)Scr::Sports];
  tileMusic = scrObj[(int)Scr::Music];
  tileNews = scrObj[(int)Scr::News];
  tilePhotos = scrObj[(int)Scr::Photos];
  tileTimer = scrObj[(int)Scr::Timer];
  tileBible = scrObj[(int)Scr::Bible];
  tileSet = scrObj[(int)Scr::Settings];

  buildClock();
  buildMusic();
  buildNews();
  buildPhotos();
  buildWeather();
  buildToday();
  buildWeek();
  buildTimer();
  buildSports();
  buildBible();
  buildSettings();
}

// ---------- rotary knob ----------

static int tileIndex() {
  lv_obj_t* t = lv_tileview_get_tile_active(tv);
  for (size_t i = 0; i < order.size(); i++) if (scrObj[(int)order[i]] == t) return i;
  return 0;
}

static void gotoTile(int i) {
  closeOverlay();
  int n = order.size();
  lv_tileview_set_tile_by_index(tv, (i % n + n) % n, 0, LV_ANIM_ON);
}

// Turning normally moves between screens. A press on Music/News/Settings can
// switch the knob into an adjust mode; press again (or wait 5 s) to leave it.
enum class KnobMode : uint8_t { Navigate, Volume, Scroll, Brightness, TimerSet };
static KnobMode knobMode = KnobMode::Navigate;
static uint32_t knobModeAt = 0;
static int knobModeTile = 0;
static int tileIndex();

static void setKnobMode(KnobMode m) {
  knobMode = m;
  knobModeAt = millis();
  knobModeTile = tileIndex();
  switch (m) {
    case KnobMode::Volume:     showToast(LV_SYMBOL_VOLUME_MAX " Turn for volume"); break;
    case KnobMode::Scroll:     showToast(LV_SYMBOL_LIST " Turn to scroll"); break;
    case KnobMode::Brightness: showToast(LV_SYMBOL_EYE_OPEN " Turn for brightness"); break;
    case KnobMode::TimerSet:   showToast(LV_SYMBOL_EDIT " Turn to set the timer"); break;
    default:                   showToast("Turn to change screen"); break;
  }
}

static void handleKnob() {
  static int pendingVol = -1;
  static uint32_t volChangedAt = 0;
  int steps = encoderTakeSteps();
  KnobPress press = encoderTakePress();
  uint32_t now = millis();

  if (knobMode != KnobMode::Navigate && now - knobModeAt > 5000) setKnobMode(KnobMode::Navigate);

  // Volume is sent once the knob stops turning
  if (pendingVol >= 0 && now - volChangedAt > 250) {
    playerSend(Cmd::Volume, pendingVol);
    pendingVol = -1;
  }
  if (!steps && press == KnobPress::None) return;
  log_d("knob: steps %d, press %d", steps, (int)press);

  bool wasAsleep = !halScreenIsOn() || dimmed;
  halPokeActivity();
  if (wasAsleep) return;  // first knob action only wakes the screen

  int tile = tileIndex();
  if (knobMode != KnobMode::Navigate && tile != knobModeTile) knobMode = KnobMode::Navigate;  // swiped away
  if (knobMode != KnobMode::Navigate) knobModeAt = now;

  // Alarm/timer pop-up: press stops, long press snoozes
  if (appsCurrentAlert().kind != AlertKind::None) {
    if (press == KnobPress::Short) appsAlertStop();
    else if (press == KnobPress::Long) appsAlertSnooze();
    return;
  }

  // Lists and article pop-ups: turn scrolls, press closes
  if (overlayScroll || overlay) {
    if (steps && overlayScroll) lv_obj_scroll_by_bounded(overlayScroll, 0, -steps * 60, LV_ANIM_ON);
    if (press != KnobPress::None) closeOverlay();
    return;
  }

  if (steps) {
    switch (knobMode) {
      case KnobMode::Volume: {
        int base = pendingVol >= 0 ? pendingVol : lv_arc_get_value(volArc);
        pendingVol = constrain(base + steps, 0, 21);
        volChangedAt = now;
        lv_arc_set_value(volArc, pendingVol);
        break;
      }
      case KnobMode::Scroll:
        lv_obj_scroll_by_bounded(order[tile] == Scr::Week ? wkList : newsList, 0, -steps * 90, LV_ANIM_ON);
        break;
      case KnobMode::TimerSet: {
        TimerInfo ti = appsTimer();
        timerAdjust(steps * (ti.totalSec >= 600 ? 60 : ti.totalSec >= 60 ? 30 : 10));
        break;
      }
      case KnobMode::Brightness: {
        int v = constrain((int)lv_slider_get_value(briSlider) + steps * 10, 10, 255);
        lv_slider_set_value(briSlider, v, LV_ANIM_OFF);
        lv_obj_send_event(briSlider, LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_send_event(briSlider, LV_EVENT_RELEASED, nullptr);
        break;
      }
      default:  // move between screens, one per click, wrapping around so either direction reaches every screen
        gotoTile(tile + (steps > 0 ? 1 : -1));
        break;
    }
  }

  if (press == KnobPress::Short) {
    if (knobMode != KnobMode::Navigate) { setKnobMode(KnobMode::Navigate); return; }
    switch (order[tile]) {
      case Scr::Clock:    nextFace(); break;
      case Scr::Music:    playerSend(Cmd::Toggle); break;
      case Scr::News:     setKnobMode(KnobMode::Scroll); break;
      case Scr::Week:     setKnobMode(KnobMode::Scroll); break;
      case Scr::Photos:   showPhoto(1); break;
      case Scr::Timer:    timerToggle(); break;
      case Scr::Bible:    showVerse(true); break;
      case Scr::Weather:  weatherRefreshNow(); showToast("Updating weather"); break;
      case Scr::Settings: setKnobMode(KnobMode::Brightness); break;
      default: break;
    }
  } else if (press == KnobPress::Long && knobMode == KnobMode::Navigate) {
    if (order[tile] == Scr::Music) setKnobMode(KnobMode::Volume);
    else if (order[tile] == Scr::Timer) { timerReset(); setKnobMode(KnobMode::TimerSet); }
  }
}

void uiLoop() {
  static uint32_t lastFace = 0, lastPoll = 0, lastPower = 0, lastShift = 0;
  uint32_t now = millis();

  if (reqFace) { reqFace = false; loadFace(); }
  if (faceSoundPending.length() && now > 8000) {
    if (!nightActive()) {
      uint8_t vol;
      { CfgLock c; vol = cfg.chimeVolume; }
      playerSend(Cmd::Chime, vol, "http://127.0.0.1/sounds/face?f=" + faceSoundPending);
    }
    faceSoundPending = "";
  }
  if (reqSettings) { reqSettings = false; applySettings(); encoderInit(); }
  appsTick();
  updateAlert();
  handleKnob();
  uiTile = tileIndex();
  settingsService();
  if (netTakeRtcSync()) halWriteRtcFromSystem();

  // Hands and text need 10 updates a second; a playing dial animation needs every loop pass
  if (now - lastFace >= 100 || faceAnimating()) {
    lastFace = now;
    if (lv_tileview_get_tile_active(tv) == tileClock && halScreenIsOn()) faceTick();
  }
  if (now - lastPoll >= 200) {
    lastPoll = now;
    updateMusic();
    updateNews();
    updateNet();
    photoTick();
    updateWeather();
    updateToday();
    updateWeek();
    updateTimer();
    updateSports();
    bibleTick();
    updateSleep();
    updateUsbScreen();
  }
  if (now - lastPower >= 10000 || lastPower == 0) { lastPower = now; updatePower(); }

  // Idle: dim, return to the clock, then optionally switch the panel off
  uint16_t dimAfter, offAfter;
  uint8_t bright, dimBright;
  bool shift;
  uint8_t nightBright;
  { CfgLock c; dimAfter = cfg.dimAfter; offAfter = cfg.offAfter; bright = cfg.brightness; dimBright = cfg.dimBrightness; shift = cfg.pixelShift; nightBright = cfg.nightBrightness; }
  uint32_t idle = halIdleMs();
  if (offAfter && idle > offAfter * 1000UL) {
    if (halScreenIsOn()) { closeOverlay(); halScreenOn(false); appliedBrightness = 0; }
  } else {
    if (!halScreenIsOn()) halScreenOn(true);
    bool wantDim = dimAfter && idle > dimAfter * 1000UL;
    bool frame;
    { CfgLock c; frame = cfg.photoFrame; }
    if (wantDim && !dimmed) {
      closeOverlay();
      // Photo-frame mode: show the slideshow while idle instead of the clock
      int ph = std::find(order.begin(), order.end(), Scr::Photos) - order.begin();
      lv_tileview_set_tile_by_index(tv, frame && ph < (int)order.size() ? ph : 0, 0, LV_ANIM_ON);
    }
    dimmed = wantDim;
    uint8_t target = dimmed && !frame ? dimBright : bright;
    if (nightActive()) target = min<uint8_t>(target, nightBright);
    if (target != appliedBrightness) { halSetBrightness(target); appliedBrightness = target; }
  }

  // AMOLED burn-in protection: move the face a few pixels each minute
  if (shift && now - lastShift > 60000) {
    lastShift = now;
    lv_obj_set_style_translate_x(faceHost, (int32_t)random(-4, 5), 0);
    lv_obj_set_style_translate_y(faceHost, (int32_t)random(-4, 5), 0);
  }

  // Diagnostics while a dial animation plays: total LVGL time and panel-transfer time per pass
  extern volatile uint32_t halFlushUs;
  bool anim = faceAnimating();
  uint32_t f0 = halFlushUs, t0 = micros();
  lv_timer_handler();
  if (anim) {
    uint32_t total = micros() - t0, flush = halFlushUs - f0;
    if (total > 20000) { uiRenderSamples++; uiRenderUs += total; uiFlushUs += flush; }
  }
}
