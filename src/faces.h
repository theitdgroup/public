// Clock faces are JSON files in /faces on LittleFS (format: docs/FACE_FORMAT.md).
// This renders the active face with LVGL and keeps its hands/text up to date.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <lvgl.h>
#include <vector>

struct FaceInfo { String id, name, author; };

bool faceLoad(lv_obj_t* parent, const String& id, String* err = nullptr);
bool faceAnimating();  // a dial animation is playing right now (tick it often)
void faceAnimStats(uint32_t& frames, uint32_t& avgMs, uint32_t& maxMs);  // last dial animation timing
String faceSound();   // optional "sound" asset of the loaded face (e.g. an MP3 that plays when it appears)
void faceTick();                          // call ~10x per second from the UI task
std::vector<FaceInfo> faceList();
bool faceValidate(JsonDocument& doc, String& err);
String faceSafeId(const String& id);      // file-system safe id
