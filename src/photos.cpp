#include "photos.h"
#include <LittleFS.h>
#include <SD_MMC.h>
#include <algorithm>
#include "storage.h"

static volatile uint32_t version = 1;

uint32_t photosVersion() { return version; }
void photosChanged() { version++; }

static bool isJpeg(const String& n) {
  String l = n;
  l.toLowerCase();
  return l.endsWith(".jpg") || l.endsWith(".jpeg");
}

static void scan(fs::FS& fs, bool sd, std::vector<PhotoRef>& out) {
  File dir = fs.open("/photos");
  if (!dir || !dir.isDirectory()) return;
  while (File f = dir.openNextFile()) {
    String n = f.name();
    if (!f.isDirectory() && isJpeg(n) && !n.startsWith(".")) out.push_back({sd, n, (size_t)f.size()});
    f.close();
    if (out.size() > 2000) break;
  }
}

std::vector<PhotoRef> photosList() {
  std::vector<PhotoRef> out;
  scan(LittleFS, false, out);
  if (storageAvailable()) scan(SD_MMC, true, out);
  std::sort(out.begin(), out.end(), [](const PhotoRef& a, const PhotoRef& b) { return a.name < b.name; });
  return out;
}

// Width/height from the JPEG's start-of-frame marker
bool jpegSize(const uint8_t* d, size_t len, uint16_t& w, uint16_t& h) {
  if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return false;
  size_t i = 2;
  while (i + 9 < len) {
    if (d[i] != 0xFF) { i++; continue; }
    uint8_t m = d[i + 1];
    if (m == 0xC2) return false;  // progressive JPEGs aren't supported by the decoder
    if (m == 0xC0 || m == 0xC1) {
      h = (d[i + 5] << 8) | d[i + 6];
      w = (d[i + 7] << 8) | d[i + 8];
      return true;
    }
    i += 2 + ((d[i + 2] << 8) | d[i + 3]);
  }
  return false;
}

uint8_t* photosRead(const PhotoRef& p, size_t& len, uint16_t& w, uint16_t& h) {
  if (p.sd && !storageAvailable()) return nullptr;
  fs::FS& fs = p.sd ? (fs::FS&)SD_MMC : (fs::FS&)LittleFS;
  File f = fs.open("/photos/" + p.name, "r");
  if (!f) return nullptr;
  len = f.size();
  if (len < 100 || len > 1500000) { f.close(); return nullptr; }
  uint8_t* buf = (uint8_t*)ps_malloc(len);
  if (!buf) { f.close(); return nullptr; }
  size_t got = f.read(buf, len);
  f.close();
  if (got != len || !jpegSize(buf, len, w, h) || w > 1024 || h > 1024) { free(buf); return nullptr; }
  return buf;
}

bool photosDelete(bool sd, const String& name) {
  if (name.indexOf('/') >= 0 || name.indexOf("..") >= 0) return false;
  if (sd && !storageAvailable()) return false;
  fs::FS& fs = sd ? (fs::FS&)SD_MMC : (fs::FS&)LittleFS;
  bool ok = fs.remove("/photos/" + name);
  if (ok) photosChanged();
  return ok;
}
