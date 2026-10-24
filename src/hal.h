// Board bring-up: AMOLED + LVGL, touch, PMU (battery), RTC, codec.
// Everything here runs on the UI task (the I2C bus owner after setup).
#pragma once
#include <Arduino.h>

void halInit();                  // call once from setup(), before LVGL use
void halSetBrightness(uint8_t level);
void halScreenOn(bool on);
bool halScreenIsOn();
uint32_t halIdleMs();            // ms since last touch
void halPokeActivity();          // treat as user activity (e.g. web command)
void halUpdatePower();           // refresh PowerState from the PMU
void halWriteRtcFromSystem();    // after NTP sync
