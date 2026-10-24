// Screens: Clock | Music | News | Settings (swipe left/right).
#pragma once
#include <Arduino.h>

void uiInit();
void uiLoop();                    // run from loop() on the UI task
// Thread-safe requests from other tasks (web server)
void uiRequestFaceReload();
void uiRequestSettingsApply();
extern volatile int uiTile;      // 0 clock, 1 music, 2 news, 3 settings (for diagnostics)
