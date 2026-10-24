// Photo slideshow storage: 466x466 baseline JPEGs in /photos on the watch's
// flash and/or the SD card. The web page resizes pictures before uploading.
#pragma once
#include <Arduino.h>
#include <vector>

struct PhotoRef {
  bool sd;       // false = internal flash (LittleFS)
  String name;   // file name inside /photos
  size_t size;
};

std::vector<PhotoRef> photosList();
// Reads a photo into a PSRAM buffer; caller frees it. Returns nullptr on error.
uint8_t* photosRead(const PhotoRef& p, size_t& len, uint16_t& w, uint16_t& h);
bool photosDelete(bool sd, const String& name);
uint32_t photosVersion();   // bumps whenever photos are added or removed
void photosChanged();

// Width/height from a JPEG's frame header; false if it isn't a JPEG LVGL can decode
bool jpegSize(const uint8_t* d, size_t len, uint16_t& w, uint16_t& h);
