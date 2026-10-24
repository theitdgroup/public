#include "faces.h"
#include "photos.h"
#include <LittleFS.h>
#include <src/misc/cache/lv_cache.h>  // lv_image_cache_drop
#include <math.h>
#include <sys/time.h>
#include <algorithm>
#include "settings.h"
#include "state.h"
#include "clockapps.h"
#include "weather.h"

static const int kR = 233;  // screen radius; face coordinates are relative to the centre

enum class HandKind : uint8_t { Hour, Minute, Second };
enum class ArcValue : uint8_t { None, Seconds, Minutes, Hours, Battery, DayOfWeek, DayOfMonth, Month, DayOfYear, Volume };

struct Hand {
  lv_obj_t* obj;
  HandKind kind;
  float length, tail, width;
  bool smooth;
  lv_point_precise_t pts[2];
  float lastAngle = -1000;
};

struct DynText {
  lv_obj_t* obj;
  String tmpl, last;
  bool upper;
};

struct DynArc {
  lv_obj_t* obj;
  ArcValue value;
  int last = -1;
};

static std::vector<Hand> hands;
static std::vector<DynText> texts;
static std::vector<DynArc> arcs;
static std::vector<lv_point_precise_t*> staticPoints;

// Image hands: a picture rotated around its pivot (imported .orb faces)
struct ImageHand {
  lv_obj_t* obj;
  HandKind kind;
  bool smooth;
  int32_t lastTenths = -1;
};

// Dial plate with an optional short animation (e.g. gears turning once a minute)
struct Plate {
  lv_obj_t* obj = nullptr;
  std::vector<lv_image_dsc_t*> frames;
  uint16_t frameMs = 125;
  uint16_t everySec = 60;
  int playing = -1;          // current frame while animating, -1 when idle
  uint32_t frameAt = 0;
  time_t lastTrigger = 0;
  bool onStart = false;      // play once when the face appears instead of every `everySec`
  uint32_t startAt = 0;      // millis() when that one play is due; 0 once it has played
};

static std::vector<ImageHand> imageHands;
static std::vector<std::pair<lv_obj_t*, bool>> weatherIcons;  // image, large
static int shownWeatherCode = -1;
static std::vector<Plate> plates;
static std::vector<uint8_t*> imageBuffers;  // raw .bin images loaded for the current face
static lv_obj_t* root = nullptr;

String faceSafeId(const String& id) {
  String out;
  for (char c : id) {
    if (isalnum(c) || c == '-' || c == '_') out += (char)tolower(c);
    else if (c == ' ') out += '-';
  }
  return out.substring(0, 40);
}

static bool parseColor(JsonVariantConst v, lv_color_t& c, lv_opa_t& opa) {
  opa = LV_OPA_COVER;
  String s = v.as<String>();
  if (s.isEmpty()) return false;
  if (s == "none" || s == "transparent") { opa = LV_OPA_TRANSP; c = lv_color_black(); return true; }
  if (s[0] == '#') s = s.substring(1);
  if (s.length() == 3) s = String(s[0]) + s[0] + s[1] + s[1] + s[2] + s[2];
  if (s.length() == 8) { opa = strtoul(s.substring(6).c_str(), nullptr, 16); s = s.substring(0, 6); }
  if (s.length() != 6) return false;
  c = lv_color_hex(strtoul(s.c_str(), nullptr, 16));
  return true;
}

static lv_color_t colorOr(JsonVariantConst v, uint32_t fallback, lv_opa_t* opaOut = nullptr) {
  lv_color_t c;
  lv_opa_t o;
  if (!parseColor(v, c, o)) { c = lv_color_hex(fallback); o = LV_OPA_COVER; }
  if (opaOut) *opaOut = o;
  return c;
}

static const lv_font_t* fontFor(int size, int32_t& scale) {
  static const struct { int px; const lv_font_t* f; } fonts[] = {
    {12, &lv_font_montserrat_12}, {14, &lv_font_montserrat_14}, {16, &lv_font_montserrat_16},
    {20, &lv_font_montserrat_20}, {24, &lv_font_montserrat_24}, {28, &lv_font_montserrat_28},
    {32, &lv_font_montserrat_32}, {40, &lv_font_montserrat_40}, {48, &lv_font_montserrat_48},
  };
  scale = 256;
  if (size > 48) { scale = size * 256 / 48; return &lv_font_montserrat_48; }
  const lv_font_t* best = fonts[0].f;
  for (auto& f : fonts) if (f.px <= size) best = f.f;
  return best;
}

static void styleFont(lv_obj_t* label, int size, const char* align) {
  int32_t scale;
  lv_obj_set_style_text_font(label, fontFor(size, scale), 0);
  if (scale != 256) {
    lv_obj_set_style_transform_scale(label, scale, 0);
    lv_obj_set_style_transform_pivot_x(label, !strcmp(align, "left") ? 0 : !strcmp(align, "right") ? lv_pct(100) : lv_pct(50), 0);
    lv_obj_set_style_transform_pivot_y(label, lv_pct(50), 0);
  }
}

static void place(lv_obj_t* obj, int x, int y, const char* align) {
  if (!strcmp(align, "left"))       lv_obj_align(obj, LV_ALIGN_LEFT_MID, kR + x, y);
  else if (!strcmp(align, "right")) lv_obj_align(obj, LV_ALIGN_RIGHT_MID, x - kR, y);
  else                              lv_obj_align(obj, LV_ALIGN_CENTER, x, y);
}

static lv_obj_t* plainObj(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_clickable(o, false);
  lv_obj_set_scrollable(o, false);
  lv_obj_set_event_bubble(o, true);
  return o;
}

// A line positioned at its own bounding box so updates only redraw that area
static void setLine(lv_obj_t* line, float x1, float y1, float x2, float y2, float width, lv_point_precise_t* pts) {
  float pad = width / 2 + 2;
  float minx = std::min(x1, x2) - pad, miny = std::min(y1, y2) - pad;
  pts[0] = {(lv_value_precise_t)lroundf(x1 - minx), (lv_value_precise_t)lroundf(y1 - miny)};
  pts[1] = {(lv_value_precise_t)lroundf(x2 - minx), (lv_value_precise_t)lroundf(y2 - miny)};
  lv_obj_set_pos(line, (int32_t)(kR + minx), (int32_t)(kR + miny));
  lv_line_set_points(line, pts, 2);
}

static lv_obj_t* makeLine(lv_obj_t* parent, float width, lv_color_t c, lv_opa_t opa, bool round) {
  lv_obj_t* l = lv_line_create(parent);
  lv_obj_set_clickable(l, false);
  lv_obj_set_event_bubble(l, true);
  lv_obj_set_style_line_width(l, (int32_t)width, 0);
  lv_obj_set_style_line_color(l, c, 0);
  lv_obj_set_style_line_opa(l, opa, 0);
  lv_obj_set_style_line_rounded(l, round, 0);
  return l;
}

static void buildTicks(lv_obj_t* p, JsonObjectConst e) {
  int count = e["count"] | 60;
  int majorEvery = e["majorEvery"] | 5;
  float radius = e["radius"] | 225;
  float len = e["length"] | 10, majorLen = e["majorLength"] | (len * 2);
  float w = e["width"] | 2, majorW = e["majorWidth"] | (w * 2);
  lv_opa_t opa, mopa;
  lv_color_t c = colorOr(e["color"], 0x666666, &opa);
  lv_color_t mc = colorOr(e["majorColor"].isNull() ? e["color"] : e["majorColor"], 0xFFFFFF, &mopa);
  bool round = e["round"] | false;
  bool skipMajor = e["skipMajor"] | false;
  for (int i = 0; i < count && count <= 360; i++) {
    bool major = majorEvery > 0 && i % majorEvery == 0;
    if (major && skipMajor) continue;
    float l = major ? majorLen : len, wd = major ? majorW : w;
    if (l <= 0) continue;
    float a = i * 2 * M_PI / count;
    float s = sinf(a), co = -cosf(a);
    lv_point_precise_t* pts = new lv_point_precise_t[2];
    staticPoints.push_back(pts);
    lv_obj_t* line = makeLine(p, wd, major ? mc : c, major ? mopa : opa, round);
    setLine(line, radius * s, radius * co, (radius - l) * s, (radius - l) * co, wd, pts);
  }
}

static void buildNumerals(lv_obj_t* p, JsonObjectConst e) {
  static const char* roman[] = {"XII", "I", "II", "III", "IV", "V", "VI", "VII", "VIII", "IX", "X", "XI"};
  String style = e["style"] | "arabic";
  float radius = e["radius"] | 185;
  int size = e["size"] | 32;
  lv_opa_t opa;
  lv_color_t c = colorOr(e["color"], 0xFFFFFF, &opa);
  bool quarters = style.startsWith("quarters");
  bool isRoman = style.endsWith("roman");
  for (int i = 0; i < 12; i++) {
    if (quarters && i % 3) continue;
    lv_obj_t* l = lv_label_create(p);
    lv_obj_set_event_bubble(l, true);
    lv_label_set_text(l, isRoman ? roman[i] : String(i == 0 ? 12 : i).c_str());
    lv_obj_set_style_text_color(l, c, 0);
    lv_obj_set_style_text_opa(l, opa, 0);
    styleFont(l, size, "center");
    float a = i * M_PI / 6;
    lv_obj_align(l, LV_ALIGN_CENTER, (int32_t)(radius * sinf(a)), (int32_t)(-radius * cosf(a)));
  }
}

static void buildHand(lv_obj_t* p, JsonObjectConst e) {
  Hand h;
  String k = e["hand"] | "minute";
  h.kind = k == "hour" ? HandKind::Hour : k == "second" ? HandKind::Second : HandKind::Minute;
  h.length = e["length"] | (h.kind == HandKind::Hour ? 120 : 180);
  h.tail = e["tail"] | 0;
  h.width = e["width"] | (h.kind == HandKind::Second ? 3 : 8);
  h.smooth = e["smooth"] | false;
  lv_opa_t opa;
  lv_color_t c = colorOr(e["color"], h.kind == HandKind::Second ? 0xFF3B30 : 0xFFFFFF, &opa);
  h.obj = makeLine(p, h.width, c, opa, e["round"] | true);
  hands.push_back(h);
}

static void buildCircle(lv_obj_t* p, JsonObjectConst e) {
  int r = e["radius"] | 8;
  lv_obj_t* o = plainObj(p);
  lv_obj_set_size(o, r * 2, r * 2);
  lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
  lv_opa_t opa;
  lv_obj_set_style_bg_color(o, colorOr(e["color"], 0xFFFFFF, &opa), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  int bw = e["borderWidth"] | 0;
  if (bw > 0) {
    lv_opa_t bo;
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, colorOr(e["borderColor"], 0xFFFFFF, &bo), 0);
    lv_obj_set_style_border_opa(o, bo, 0);
  }
  lv_obj_align(o, LV_ALIGN_CENTER, e["x"] | 0, e["y"] | 0);
}

static void buildRect(lv_obj_t* p, JsonObjectConst e) {
  lv_obj_t* o = plainObj(p);
  lv_obj_set_size(o, e["w"] | 100, e["h"] | 40);
  lv_obj_set_style_radius(o, e["radius"] | 0, 0);
  lv_opa_t opa;
  lv_obj_set_style_bg_color(o, colorOr(e["color"], 0x333333, &opa), 0);
  lv_obj_set_style_bg_opa(o, opa, 0);
  int bw = e["borderWidth"] | 0;
  if (bw > 0) {
    lv_opa_t bo;
    lv_obj_set_style_border_width(o, bw, 0);
    lv_obj_set_style_border_color(o, colorOr(e["borderColor"], 0xFFFFFF, &bo), 0);
    lv_obj_set_style_border_opa(o, bo, 0);
  }
  lv_obj_align(o, LV_ALIGN_CENTER, e["x"] | 0, e["y"] | 0);
}

static void buildText(lv_obj_t* p, JsonObjectConst e) {
  DynText t;
  t.tmpl = e["text"] | "{time}";
  t.upper = e["upper"] | false;
  const char* align = e["align"] | "center";
  lv_obj_t* l = lv_label_create(p);
  lv_obj_set_event_bubble(l, true);
  lv_opa_t opa;
  lv_obj_set_style_text_color(l, colorOr(e["color"], 0xFFFFFF, &opa), 0);
  lv_obj_set_style_text_opa(l, opa, 0);
  lv_obj_set_style_text_letter_space(l, e["letterSpacing"] | 0, 0);
  styleFont(l, e["size"] | 32, align);
  int width = e["width"] | 0;
  if (width > 0) {
    lv_obj_set_width(l, width);
    lv_obj_set_style_text_align(l, !strcmp(align, "left") ? LV_TEXT_ALIGN_LEFT : !strcmp(align, "right") ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, (e["marquee"] | false) ? LV_LABEL_LONG_MODE_SCROLL_CIRCULAR
                              : (e["wrap"] | false) ? LV_LABEL_LONG_MODE_WRAP
                                                    : LV_LABEL_LONG_MODE_DOTS);
    if (!(e["marquee"] | false) && !(e["wrap"] | false)) lv_obj_set_height(l, lv_font_get_line_height(lv_obj_get_style_text_font(l, LV_PART_MAIN)));
  }
  lv_label_set_text(l, "");
  place(l, e["x"] | 0, e["y"] | 0, align);
  t.obj = l;
  texts.push_back(t);
}

static ArcValue arcValueFrom(const String& v) {
  if (v == "seconds") return ArcValue::Seconds;
  if (v == "minutes") return ArcValue::Minutes;
  if (v == "hours") return ArcValue::Hours;
  if (v == "battery") return ArcValue::Battery;
  if (v == "dayOfWeek") return ArcValue::DayOfWeek;
  if (v == "dayOfMonth") return ArcValue::DayOfMonth;
  if (v == "month") return ArcValue::Month;
  if (v == "dayOfYear") return ArcValue::DayOfYear;
  if (v == "volume") return ArcValue::Volume;
  return ArcValue::None;
}

static void buildArc(lv_obj_t* p, JsonObjectConst e) {
  int r = e["radius"] | 225;
  int w = e["width"] | 8;
  int start = e["startAngle"] | 0;   // 0 = 12 o'clock, clockwise
  int sweep = e["sweep"] | 360;
  lv_obj_t* a = lv_arc_create(p);
  lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
  lv_obj_set_clickable(a, false);
  lv_obj_set_event_bubble(a, true);
  lv_obj_set_size(a, r * 2, r * 2);
  lv_obj_set_style_pad_all(a, 0, 0);
  lv_obj_set_style_bg_opa(a, LV_OPA_TRANSP, 0);
  lv_arc_set_rotation(a, (start + 270) % 360);
  lv_arc_set_bg_angles(a, 0, sweep);
  lv_arc_set_range(a, 0, 1000);
  bool round = e["round"] | true;
  lv_opa_t opa, bopa;
  lv_obj_set_style_arc_width(a, w, LV_PART_MAIN);
  lv_obj_set_style_arc_width(a, w, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(a, round, LV_PART_MAIN);
  lv_obj_set_style_arc_rounded(a, round, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, colorOr(e["color"], 0xFF3B30, &opa), LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(a, opa, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(a, colorOr(e["bgColor"], 0x222222, &bopa), LV_PART_MAIN);
  lv_obj_set_style_arc_opa(a, e["bgColor"].isNull() ? LV_OPA_TRANSP : bopa, LV_PART_MAIN);
  lv_obj_align(a, LV_ALIGN_CENTER, e["x"] | 0, e["y"] | 0);
  ArcValue v = arcValueFrom(e["value"] | "none");
  lv_arc_set_value(a, v == ArcValue::None ? 1000 : 0);
  if (v != ArcValue::None) arcs.push_back({a, v});
}

// Loads an LVGL binary image (12-byte lv_image_header_t + pixels) from /faces/assets
// into PSRAM. Pre-converted images draw without PNG decoding and stay resident.
static lv_image_dsc_t* loadBin(const String& name) {
  if (name.isEmpty() || name.indexOf("..") >= 0 || name.indexOf('/') >= 0) return nullptr;
  File f = LittleFS.open("/faces/assets/" + name, "r");
  if (!f) { log_w("missing face asset %s", name.c_str()); return nullptr; }
  size_t len = f.size();
  if (len <= sizeof(lv_image_header_t)) { f.close(); return nullptr; }
  uint8_t* buf = (uint8_t*)ps_malloc(len + sizeof(lv_image_dsc_t));
  if (!buf) { f.close(); return nullptr; }
  size_t got = f.read(buf + sizeof(lv_image_dsc_t), len);
  f.close();
  if (got != len) { free(buf); return nullptr; }
  lv_image_dsc_t* d = (lv_image_dsc_t*)buf;
  memset(d, 0, sizeof(*d));
  memcpy(&d->header, buf + sizeof(lv_image_dsc_t), sizeof(lv_image_header_t));
  if (d->header.magic != LV_IMAGE_HEADER_MAGIC) { free(buf); return nullptr; }
  d->data = buf + sizeof(lv_image_dsc_t) + sizeof(lv_image_header_t);
  d->data_size = len - sizeof(lv_image_header_t);
  imageBuffers.push_back(buf);
  return d;
}

static bool isBin(const String& s) { return s.endsWith(".bin"); }

// A JPEG face asset, kept compressed in PSRAM and decoded by LVGL when drawn. Much
// smaller than .bin for photo-like animation frames (e.g. a short video clip).
static lv_image_dsc_t* loadJpg(const String& name) {
  if (name.isEmpty() || name.indexOf("..") >= 0 || name.indexOf('/') >= 0) return nullptr;
  File f = LittleFS.open("/faces/assets/" + name, "r");
  if (!f) { log_w("missing face asset %s", name.c_str()); return nullptr; }
  size_t len = f.size();
  uint8_t* buf = (uint8_t*)ps_malloc(len + sizeof(lv_image_dsc_t));
  if (!buf) { f.close(); return nullptr; }
  size_t got = f.read(buf + sizeof(lv_image_dsc_t), len);
  f.close();
  uint16_t w, h;
  if (got != len || !jpegSize(buf + sizeof(lv_image_dsc_t), len, w, h)) { free(buf); return nullptr; }
  lv_image_dsc_t* d = (lv_image_dsc_t*)buf;
  memset(d, 0, sizeof(*d));
  d->header.magic = LV_IMAGE_HEADER_MAGIC;
  d->header.cf = LV_COLOR_FORMAT_RAW;
  d->header.w = w;
  d->header.h = h;
  d->data_size = len;
  d->data = buf + sizeof(lv_image_dsc_t);
  imageBuffers.push_back(buf);
  return d;
}

static void buildPlate(lv_obj_t* p, JsonObjectConst e) {
  Plate pl;
  // A frame may repeat (e.g. a back-and-forth sway): load each file once and reuse it
  std::vector<std::pair<String, lv_image_dsc_t*>> loaded;
  for (JsonVariantConst f : e["frames"].as<JsonArrayConst>()) {
    String name = f.as<String>();
    lv_image_dsc_t* d = nullptr;
    for (auto& l : loaded) if (l.first == name) d = l.second;
    if (!d && (d = name.endsWith(".jpg") ? loadJpg(name) : loadBin(name))) loaded.push_back({name, d});
    if (d) pl.frames.push_back(d);
  }
  if (pl.frames.empty()) return;
  pl.frameMs = 1000 / constrain((int)(e["fps"] | 8), 1, 30);
  pl.onStart = e["onStart"] | false;
  // every `everySec` seconds (default 60); a play-on-start animation only repeats if it also sets everySec
  pl.everySec = max(0, (int)(e["everySec"] | (pl.onStart ? 0 : 60)));
  if (pl.onStart) pl.startAt = millis() + max(0, (int)(e["delayMs"] | 1000));
  pl.obj = lv_image_create(p);
  lv_obj_set_event_bubble(pl.obj, true);
  lv_image_set_src(pl.obj, pl.frames[0]);
  // "color": tint for alpha-only (A8) frames, e.g. a two-tone photo over a dark background.
  // Such frames draw natively with no decoding, which keeps full-screen animation smooth.
  if (!e["color"].isNull()) {
    lv_opa_t o;
    lv_obj_set_style_image_recolor(pl.obj, colorOr(e["color"], 0xFFFFFF, &o), 0);
    lv_obj_set_style_image_recolor_opa(pl.obj, o, 0);
  }
  lv_obj_align(pl.obj, LV_ALIGN_CENTER, e["x"] | 0, e["y"] | 0);
  plates.push_back(pl);
}

static void buildImageHand(lv_obj_t* p, JsonObjectConst e) {
  lv_image_dsc_t* d = loadBin(e["src"] | "");
  if (!d) return;
  ImageHand h;
  String k = e["hand"] | "minute";
  h.kind = k == "hour" ? HandKind::Hour : k == "second" ? HandKind::Second : HandKind::Minute;
  h.smooth = e["smooth"] | false;
  int px = e["pivotX"] | (int)(d->header.w / 2), py = e["pivotY"] | (int)d->header.h;
  h.obj = lv_image_create(p);
  lv_obj_set_event_bubble(h.obj, true);
  lv_image_set_src(h.obj, d);
  lv_image_set_pivot(h.obj, px, py);
  lv_image_set_antialias(h.obj, true);
  lv_obj_set_pos(h.obj, kR + (int)(e["x"] | 0) - px, kR + (int)(e["y"] | 0) - py);
  lv_obj_set_style_image_opa(h.obj, e["opacity"] | 255, 0);
  imageHands.push_back(h);
}

static void buildImage(lv_obj_t* p, JsonObjectConst e) {
  String src = e["src"] | "";
  if (src.isEmpty() || src.indexOf("..") >= 0) return;
  lv_image_dsc_t* bin = isBin(src) ? loadBin(src) : nullptr;
  if (isBin(src) && !bin) return;
  String path = "L:/faces/assets/" + src;
  lv_obj_t* img = lv_image_create(p);
  lv_obj_set_event_bubble(img, true);
  if (bin) lv_image_set_src(img, bin);
  else lv_image_set_src(img, path.c_str());
  int scale = e["scale"] | 256;
  if (scale != 256) lv_image_set_scale(img, scale);
  lv_obj_set_style_image_opa(img, e["opacity"] | 255, 0);
  lv_obj_align(img, LV_ALIGN_CENTER, e["x"] | 0, e["y"] | 0);
}

static void clearFace() {
  hands.clear();
  texts.clear();
  arcs.clear();
  for (auto* p : staticPoints) delete[] p;
  staticPoints.clear();
  imageHands.clear();
  plates.clear();
  weatherIcons.clear();
  shownWeatherCode = -1;
  if (root) { lv_obj_delete(root); root = nullptr; }
  // Objects are gone; now the pixel buffers they pointed at can go too
  for (uint8_t* b : imageBuffers) {
    lv_image_cache_drop((lv_image_dsc_t*)b);
    free(b);
  }
  imageBuffers.clear();
}

static bool readFace(const String& id, JsonDocument& doc, String* err) {
  File f = LittleFS.open("/faces/" + faceSafeId(id) + ".json", "r");
  if (!f) { if (err) *err = "Face not found: " + id; return false; }
  DeserializationError de = deserializeJson(doc, f);
  f.close();
  if (de) { if (err) *err = String("Bad JSON: ") + de.c_str(); return false; }
  return true;
}

bool faceValidate(JsonDocument& doc, String& err) {
  if (!doc["name"].is<const char*>()) { err = "Face needs a \"name\""; return false; }
  if (!doc["elements"].is<JsonArray>()) { err = "Face needs an \"elements\" array"; return false; }
  if (doc["elements"].size() > 200) { err = "Too many elements (max 200)"; return false; }
  static const char* types[] = {"ticks", "numerals", "hand", "circle", "rect", "text", "arc", "image", "plate", "imagehand", "weathericon"};
  for (JsonObject e : doc["elements"].as<JsonArray>()) {
    String t = e["type"] | "";
    bool ok = false;
    for (auto* k : types) if (t == k) ok = true;
    if (!ok) { err = "Unknown element type: " + t; return false; }
  }
  return true;
}

static String loadedSound;
// Timing of the last dial animation, for /api/status: how long frames really took
static uint32_t animFrames = 0, animSumMs = 0, animMaxMs = 0, animLastSwitch = 0;
bool faceAnimating() {
  for (auto& pl : plates) if (pl.playing > 0) return true;
  return false;
}

void faceAnimStats(uint32_t& frames, uint32_t& avgMs, uint32_t& maxMs) {
  frames = animFrames; avgMs = animFrames ? animSumMs / animFrames : 0; maxMs = animMaxMs;
}
String faceSound() { return loadedSound; }

bool faceLoad(lv_obj_t* parent, const String& id, String* err) {
  JsonDocument doc;
  if (!readFace(id, doc, err)) return false;
  if (String e; !faceValidate(doc, e)) { if (err) *err = e; return false; }
  loadedSound = doc["sound"] | "";
  if (loadedSound.indexOf('/') >= 0 || loadedSound.indexOf("..") >= 0) loadedSound = "";

  clearFace();
  root = plainObj(parent);
  lv_obj_set_size(root, kR * 2, kR * 2);
  lv_obj_center(root);

  JsonObjectConst bg = doc["background"];
  lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
  lv_obj_set_style_bg_color(root, colorOr(bg["color"], 0x000000), 0);
  String grad = bg["gradient"] | "none";
  if (grad != "none" && !bg["color2"].isNull()) {
    lv_obj_set_style_bg_grad_color(root, colorOr(bg["color2"], 0x000000), 0);
    lv_obj_set_style_bg_grad_dir(root, grad == "horizontal" ? LV_GRAD_DIR_HOR : LV_GRAD_DIR_VER, 0);
  }

  for (JsonObjectConst e : doc["elements"].as<JsonArrayConst>()) {
    String t = e["type"] | "";
    if (t == "ticks") buildTicks(root, e);
    else if (t == "numerals") buildNumerals(root, e);
    else if (t == "hand") buildHand(root, e);
    else if (t == "circle") buildCircle(root, e);
    else if (t == "rect") buildRect(root, e);
    else if (t == "text") buildText(root, e);
    else if (t == "arc") buildArc(root, e);
    else if (t == "image") buildImage(root, e);
    else if (t == "plate") buildPlate(root, e);
    else if (t == "imagehand") buildImageHand(root, e);
    else if (t == "weathericon") {
      lv_obj_t* img = lv_image_create(root);
      lv_obj_set_event_bubble(img, true);
      lv_obj_align(img, LV_ALIGN_CENTER, e["x"] | 0, e["y"] | 0);
      weatherIcons.push_back({img, String(e["size"] | "large") != "small"});
    }
  }
  faceTick();
  return true;
}

// ---------- live updates ----------

static const char* kDays[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char* kMonths[] = {"January", "February", "March", "April", "May", "June", "July", "August", "September", "October", "November", "December"};

static String two(int v) { return v < 10 ? "0" + String(v) : String(v); }

static String expand(const String& tmpl, const struct tm& t, bool h24) {
  if (tmpl.indexOf('{') < 0) return tmpl;
  String out;
  int i = 0;
  while (i < (int)tmpl.length()) {
    int open = tmpl.indexOf('{', i);
    if (open < 0) { out += tmpl.substring(i); break; }
    int close = tmpl.indexOf('}', open);
    if (close < 0) { out += tmpl.substring(i); break; }
    out += tmpl.substring(i, open);
    String k = tmpl.substring(open + 1, close);
    int h12 = t.tm_hour % 12 == 0 ? 12 : t.tm_hour % 12;
    if (k == "HH") out += two(t.tm_hour);
    else if (k == "H") out += t.tm_hour;
    else if (k == "hh") out += two(h12);
    else if (k == "h") out += h12;
    else if (k == "mm") out += two(t.tm_min);
    else if (k == "ss") out += two(t.tm_sec);
    else if (k == "AMPM") out += t.tm_hour < 12 ? "AM" : "PM";
    else if (k == "ampm") out += t.tm_hour < 12 ? "am" : "pm";
    else if (k == "time") out += h24 ? two(t.tm_hour) + ":" + two(t.tm_min) : String(h12) + ":" + two(t.tm_min);
    else if (k == "hour") out += h24 ? two(t.tm_hour) : String(h12);
    else if (k == "ampm?") out += h24 ? "" : (t.tm_hour < 12 ? "AM" : "PM");
    else if (k == "dddd") out += kDays[t.tm_wday];
    else if (k == "ddd") out += String(kDays[t.tm_wday]).substring(0, 3);
    else if (k == "dd") out += two(t.tm_mday);
    else if (k == "d") out += t.tm_mday;
    else if (k == "MMMM") out += kMonths[t.tm_mon];
    else if (k == "MMM") out += String(kMonths[t.tm_mon]).substring(0, 3);
    else if (k == "MM") out += two(t.tm_mon + 1);
    else if (k == "M") out += t.tm_mon + 1;
    else if (k == "yyyy") out += t.tm_year + 1900;
    else if (k == "yy") out += two(t.tm_year % 100);
    else if (k == "battery") out += power.percent >= 0 ? String(power.percent) + "%" : "";
    else if (k == "charging") out += power.charging ? LV_SYMBOL_CHARGE : "";
    else if (k == "wifi") out += net.connected ? LV_SYMBOL_WIFI : "";
    else if (k == "title" || k == "artist" || k == "np" || k == "station") {
      StateLock l;
      bool active = player.playing;
      if (k == "title") out += active ? player.title : "";
      else if (k == "artist") out += active ? player.artist : "";
      else if (k == "station") out += active ? player.station : "";
      else if (active) out += player.artist.length() ? player.artist + " - " + player.title : player.title.length() ? player.title : player.station;
    } else if (k == "headline" || k == "headlineSource") {
      StateLock l;
      if (!news.empty()) {
        // rotate through the 10 newest headlines every 15 seconds
        size_t n = std::min<size_t>(news.size(), 10);
        const NewsItem& it = news[(time(nullptr) / 15) % n];
        out += k == "headline" ? it.title : it.source;
      }
    } else if (k == "temp" || k == "tempHi" || k == "tempLo" || k == "weather") {
      StateLock l;
      if (weather.valid) {
        float v = k == "temp" ? weather.temp : k == "tempHi" ? weather.days[0].hi : weather.days[0].lo;
        if (k == "weather") out += weatherText(weather.code);
        else { out += String((int)lroundf(v)); out += "\xC2\xB0"; }
      }
    } else if (k == "city") {
      CfgLock c;
      out += cfg.place;
    } else if (k == "nextAlarm") {
      out += nextAlarmText();
    } else if (k == "countdown") {
      auto l = countdownLines();
      if (!l.empty()) out += l[0];
    } else if (k == "symbol:bell") out += LV_SYMBOL_BELL;
    else if (k == "symbol:audio") out += LV_SYMBOL_AUDIO;
    else { out += '{'; out += k; out += '}'; }
    i = close + 1;
  }
  return out;
}

static int arcPermille(ArcValue v, const struct tm& t, float secF) {
  switch (v) {
    case ArcValue::Seconds:    return (int)(secF * 1000 / 60);
    case ArcValue::Minutes:    return (t.tm_min * 60 + t.tm_sec) * 1000 / 3600;
    case ArcValue::Hours:      return ((t.tm_hour % 12) * 60 + t.tm_min) * 1000 / 720;
    case ArcValue::Battery:    return power.percent < 0 ? 0 : power.percent * 10;
    case ArcValue::DayOfWeek:  return ((t.tm_wday + 6) % 7 + 1) * 1000 / 7;
    case ArcValue::DayOfMonth: return t.tm_mday * 1000 / 31;
    case ArcValue::Month:      return (t.tm_mon + 1) * 1000 / 12;
    case ArcValue::DayOfYear:  return (t.tm_yday + 1) * 1000 / 366;
    case ArcValue::Volume:     { StateLock l; return player.volume * 1000 / 21; }
    default:                   return 1000;
  }
}

void faceTick() {
  if (!root) return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm t;
  localtime_r(&tv.tv_sec, &t);
  float secF = t.tm_sec + tv.tv_usec / 1e6f;
  bool h24;
  { CfgLock c; h24 = cfg.h24; }

  for (auto& h : hands) {
    float frac;  // 0..1 around the dial
    switch (h.kind) {
      case HandKind::Hour:   frac = ((t.tm_hour % 12) + t.tm_min / 60.0f + t.tm_sec / 3600.0f) / 12.0f; break;
      case HandKind::Minute: frac = (t.tm_min + t.tm_sec / 60.0f) / 60.0f; break;
      default:               frac = (h.smooth ? secF : t.tm_sec) / 60.0f; break;
    }
    float a = frac * 2 * M_PI;
    if (fabsf(a - h.lastAngle) < 0.002f) continue;
    h.lastAngle = a;
    float s = sinf(a), c = -cosf(a);
    setLine(h.obj, -h.tail * s, -h.tail * c, h.length * s, h.length * c, h.width, h.pts);
  }

  for (auto& h : imageHands) {
    float frac;
    switch (h.kind) {
      case HandKind::Hour:   frac = ((t.tm_hour % 12) + t.tm_min / 60.0f) / 12.0f; break;
      case HandKind::Minute: frac = (t.tm_min + t.tm_sec / 60.0f) / 60.0f; break;
      default:               frac = (h.smooth ? secF : t.tm_sec) / 60.0f; break;
    }
    // LVGL rotation is in 0.1 degree steps, clockwise; hand artwork points at 12
    int32_t tenths = (int32_t)(frac * 3600) % 3600;
    if (h.kind != HandKind::Second && !h.smooth) tenths -= tenths % 5;  // skip invisible sub-step redraws
    if (tenths != h.lastTenths) {
      h.lastTenths = tenths;
      lv_image_set_rotation(h.obj, tenths);
    }
  }

  // Dial animation: play the frames when the face appears ("onStart") and/or every
  // `everySec` seconds, then rest on frame 0
  uint32_t nowMs = millis();
  for (auto& pl : plates) {
    if (pl.frames.size() < 2) continue;
    bool due = (pl.startAt && (int32_t)(nowMs - pl.startAt) >= 0) ||
               (pl.everySec && tv.tv_sec % pl.everySec == 0 && tv.tv_sec != pl.lastTrigger);
    if (pl.playing < 0 && due) {
      pl.lastTrigger = tv.tv_sec;
      pl.startAt = 0;
      pl.playing = 1;
      pl.frameAt = nowMs;
      lv_image_set_src(pl.obj, pl.frames[1]);
    } else if (pl.playing > 0 && nowMs - pl.frameAt >= pl.frameMs) {
      uint32_t took = nowMs - animLastSwitch;
      if (pl.playing == 1) { animFrames = animSumMs = animMaxMs = 0; }
      else { animFrames++; animSumMs += took; animMaxMs = max(animMaxMs, took); }
      animLastSwitch = nowMs;
      pl.frameAt = nowMs;
      pl.playing++;
      if (pl.playing >= (int)pl.frames.size()) pl.playing = -1;
      lv_image_set_src(pl.obj, pl.frames[pl.playing < 0 ? 0 : pl.playing]);
    }
  }

  for (auto& tx : texts) {
    String s = expand(tx.tmpl, t, h24);
    if (tx.upper) s.toUpperCase();
    if (s != tx.last) {
      tx.last = s;
      lv_label_set_text(tx.obj, s.c_str());
    }
  }

  if (!weatherIcons.empty()) {
    int code;
    bool day, valid;
    { StateLock l; code = weather.code; day = weather.day; valid = weather.valid; }
    int key = valid ? code * 2 + day : -2;
    if (key != shownWeatherCode) {
      shownWeatherCode = key;
      for (auto& w : weatherIcons) {
        lv_obj_set_hidden(w.first, !valid);
        if (valid) lv_image_set_src(w.first, weatherIcon(code, day, w.second));
      }
    }
  }

  for (auto& a : arcs) {
    int v = arcPermille(a.value, t, secF);
    if (v != a.last) { a.last = v; lv_arc_set_value(a.obj, v); }
  }
}

std::vector<FaceInfo> faceList() {
  std::vector<FaceInfo> out;
  File dir = LittleFS.open("/faces");
  if (!dir) return out;
  while (File f = dir.openNextFile()) {
    String name = f.name();
    if (f.isDirectory() || !name.endsWith(".json")) continue;
    JsonDocument filter;
    filter["name"] = true;
    filter["author"] = true;
    JsonDocument doc;
    deserializeJson(doc, f, DeserializationOption::Filter(filter));
    FaceInfo fi;
    fi.id = name.substring(0, name.length() - 5);
    fi.name = doc["name"] | fi.id.c_str();
    fi.author = doc["author"] | "";
    out.push_back(fi);
  }
  std::sort(out.begin(), out.end(), [](const FaceInfo& a, const FaceInfo& b) { return a.name < b.name; });
  return out;
}
