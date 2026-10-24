// Weather from Open-Meteo (free, no account). Runs on the network task.
#pragma once
#include <Arduino.h>
#include <lvgl.h>

void weatherService();      // refresh every 15 minutes
void weatherRefreshNow();
const char* weatherText(int wmoCode);
const lv_image_dsc_t* weatherIcon(int wmoCode, bool day, bool large);
