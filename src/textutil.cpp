#include "textutil.h"

static void appendUtf8(String& out, uint32_t cp) {
  if (cp < 0x80) out += (char)cp;
  else if (cp < 0x800) { out += (char)(0xC0 | (cp >> 6)); out += (char)(0x80 | (cp & 0x3F)); }
  else if (cp < 0x10000) { out += (char)(0xE0 | (cp >> 12)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
  else { out += (char)(0xF0 | (cp >> 18)); out += (char)(0x80 | ((cp >> 12) & 0x3F)); out += (char)(0x80 | ((cp >> 6) & 0x3F)); out += (char)(0x80 | (cp & 0x3F)); }
}

String decodeEntities(const String& in) {
  static const struct { const char* name; uint32_t cp; } named[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", ' '},
    {"hellip", 0x2026}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"rsquo", 0x2019}, {"lsquo", 0x2018},
    {"rdquo", 0x201D}, {"ldquo", 0x201C}, {"pound", 0xA3}, {"euro", 0x20AC}, {"copy", 0xA9},
    {"eacute", 0xE9}, {"egrave", 0xE8}, {"aacute", 0xE1}, {"agrave", 0xE0}, {"oacute", 0xF3},
    {"uuml", 0xFC}, {"ouml", 0xF6}, {"auml", 0xE4}, {"ccedil", 0xE7}, {"ntilde", 0xF1}, {"iacute", 0xED},
  };
  String out;
  out.reserve(in.length());
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c != '&') { out += c; continue; }
    int semi = in.indexOf(';', i);
    if (semi < 0 || semi - (int)i > 10) { out += c; continue; }
    String ent = in.substring(i + 1, semi);
    bool done = false;
    if (ent.startsWith("#x") || ent.startsWith("#X")) {
      appendUtf8(out, strtoul(ent.c_str() + 2, nullptr, 16)); done = true;
    } else if (ent.startsWith("#")) {
      appendUtf8(out, strtoul(ent.c_str() + 1, nullptr, 10)); done = true;
    } else {
      for (auto& n : named) if (ent == n.name) { appendUtf8(out, n.cp); done = true; break; }
    }
    if (done) i = semi; else out += c;
  }
  return out;
}

String stripTags(const String& in) {
  String out;
  out.reserve(in.length());
  bool inTag = false, lastSpace = true;
  for (size_t i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '<') { inTag = true; continue; }
    if (c == '>' && inTag) { inTag = false; if (!lastSpace) { out += ' '; lastSpace = true; } continue; }
    if (inTag) continue;
    if (c == '\n' || c == '\r' || c == '\t' || c == ' ') {
      if (!lastSpace) { out += ' '; lastSpace = true; }
      continue;
    }
    out += c;
    lastSpace = false;
  }
  out.trim();
  return out;
}

static const char* foldCodepoint(uint32_t cp) {
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return "'";
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: case 0xAB: case 0xBB: return "\"";
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2212: return "-";
    case 0x2026: return "...";
    case 0x2022: case 0xB7: return "*";
    case 0xA0: case 0x2009: case 0x200A: case 0x202F: case 0x2002: case 0x2003: return " ";
    case 0x200B: case 0xFEFF: case 0x200D: return "";
    case 0xA3: return "GBP ";
    case 0x20AC: return "EUR ";
    case 0xA5: return "JPY ";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0x2122: return "TM";
    case 0xB0: return "\xC2\xB0";  // degree sign exists in the LVGL fonts
    case 0xD7: return "x";
    case 0xBD: return "1/2";
    case 0xDF: return "ss";
    case 0xC6: return "AE";
    case 0xE6: return "ae";
    case 0x152: return "OE";
    case 0x153: return "oe";
  }
  // Latin-1 / Latin Extended-A letters with diacritics -> base letter
  static const char latin1[] = "AAAAAAACEEEEIIIIDNOOOOO*OUUUUYTsaaaaaaaceeeeiiiidnooooo/ouuuuyty";
  if (cp >= 0xC0 && cp <= 0xFF) {
    static char buf[2];
    buf[0] = latin1[cp - 0xC0];
    buf[1] = 0;
    return buf;
  }
  static const char extA[] =  // U+0100..U+017F
      "AaAaAaCcCcCcCcDdDdEeEeEeEeEeGgGgGgGgHhHhIiIiIiIiIiJjJjKkkLlLlLlLlLlNnNnNnnNnOoOoOoOoRrRrRrSsSsSsSsTtTtTtUuUuUuUuUuUuWwYyYZzZzZzs";
  if (cp >= 0x100 && cp <= 0x17F) {
    static char buf[2];
    buf[0] = extA[cp - 0x100];
    buf[1] = 0;
    return buf;
  }
  return nullptr;
}

String asciiFold(const String& in) {
  String out;
  out.reserve(in.length());
  const uint8_t* s = (const uint8_t*)in.c_str();
  size_t n = in.length();
  for (size_t i = 0; i < n;) {
    uint8_t c = s[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    uint32_t cp = 0;
    int len = (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (len == 1 || i + len > n) { i++; continue; }
    cp = c & (0xFF >> (len + 1));
    for (int k = 1; k < len; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += len;
    const char* r = foldCodepoint(cp);
    if (r) out += r;
  }
  return out;
}

String cleanText(const String& in, size_t maxLen) {
  String s = in;
  s.trim();
  if (s.startsWith("<![CDATA[")) {
    int end = s.lastIndexOf("]]>");
    s = s.substring(9, end > 0 ? end : s.length());
  }
  // Feeds often double-encode (&lt;p&gt;): decode, strip, decode again
  s = stripTags(decodeEntities(s));
  s = asciiFold(decodeEntities(s));
  s.trim();
  if (maxLen && s.length() > maxLen) {
    int cut = s.lastIndexOf(' ', maxLen);
    s = s.substring(0, cut > (int)maxLen / 2 ? cut : maxLen) + "...";
  }
  return s;
}

static int64_t daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return (int64_t)era * 146097 + (int64_t)doe - 719468;
}

static int parseOffset(const char* p) {  // "+0100", "-05:00", "Z", "GMT", "EST" -> seconds
  while (*p == ' ') p++;
  if (*p == '+' || *p == '-') {
    int sign = *p == '-' ? -1 : 1;
    int h = (p[1] - '0') * 10 + (p[2] - '0');
    const char* q = p + 3;
    if (*q == ':') q++;
    int m = isdigit(q[0]) ? (q[0] - '0') * 10 + (q[1] - '0') : 0;
    return sign * (h * 3600 + m * 60);
  }
  if (!strncmp(p, "EDT", 3)) return -4 * 3600;
  if (!strncmp(p, "EST", 3) || !strncmp(p, "CDT", 3)) return (p[0] == 'E' ? -5 : -5) * 3600;
  if (!strncmp(p, "CST", 3) || !strncmp(p, "MDT", 3)) return -6 * 3600;
  if (!strncmp(p, "MST", 3) || !strncmp(p, "PDT", 3)) return (p[0] == 'M' ? -7 : -7) * 3600;
  if (!strncmp(p, "PST", 3)) return -8 * 3600;
  return 0;
}

time_t parseFeedDate(const String& str) {
  const char* s = str.c_str();
  while (*s == ' ' || *s == '\n') s++;
  int y, mo, d, h = 0, mi = 0, se = 0;
  if (isdigit(s[0]) && s[4] == '-') {  // ISO 8601
    if (sscanf(s, "%d-%d-%dT%d:%d:%d", &y, &mo, &d, &h, &mi, &se) < 3) return 0;
    const char* tz = strchr(s, 'T');
    tz = tz ? tz + 9 : s + 10;
    while (*tz == '.' || isdigit(*tz)) tz++;
    int64_t t = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
    return (time_t)(t - parseOffset(tz));
  }
  const char* comma = strchr(s, ',');  // RFC 822, weekday optional
  if (comma) s = comma + 1;
  char mon[4] = {0};
  int consumed = 0;
  if (sscanf(s, "%d %3s %d %d:%d:%d%n", &d, mon, &y, &h, &mi, &se, &consumed) < 3) return 0;
  static const char* months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* mp = strstr(months, mon);
  if (!mp) return 0;
  mo = (mp - months) / 3 + 1;
  if (y < 100) y += 2000;
  int64_t t = daysFromCivil(y, mo, d) * 86400 + h * 3600 + mi * 60 + se;
  return (time_t)(t - (consumed ? parseOffset(s + consumed) : 0));
}

String urlEncode(const String& s) {
  String out;
  const char* hex = "0123456789ABCDEF";
  for (size_t i = 0; i < s.length(); i++) {
    uint8_t c = s[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char)c;
    else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
  }
  return out;
}
