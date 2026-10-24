#include "weather.h"
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFi.h>
#include "icons.h"
#include "settings.h"
#include "state.h"

static uint32_t nextRefresh = 0;

void weatherRefreshNow() { nextRefresh = 0; }

const char* weatherText(int c) {
  if (c == 0) return "Clear";
  if (c == 1) return "Mostly clear";
  if (c == 2) return "Partly cloudy";
  if (c == 3) return "Cloudy";
  if (c == 45 || c == 48) return "Fog";
  if (c >= 51 && c <= 57) return "Drizzle";
  if (c >= 61 && c <= 67) return c >= 65 ? "Heavy rain" : "Rain";
  if (c >= 71 && c <= 77) return "Snow";
  if (c >= 80 && c <= 82) return "Showers";
  if (c == 85 || c == 86) return "Snow showers";
  if (c >= 95) return "Thunderstorms";
  return "";
}

const lv_image_dsc_t* weatherIcon(int c, bool day, bool large) {
#define PICK(n) (large ? &icon_##n##_large : &icon_##n##_small)
  if (c == 0 || c == 1) return day ? PICK(clear_day) : PICK(clear_night);
  if (c == 2) return day ? PICK(partly_day) : PICK(partly_night);
  if (c == 3) return PICK(cloudy);
  if (c == 45 || c == 48) return PICK(fog);
  if (c >= 51 && c <= 57) return PICK(drizzle);
  if ((c >= 61 && c <= 67) || (c >= 80 && c <= 82)) return PICK(rain);
  if ((c >= 71 && c <= 77) || c == 85 || c == 86) return PICK(snow);
  if (c >= 95) return PICK(storm);
  return PICK(cloudy);
#undef PICK
}

static void fetch() {
  float lat, lon;
  bool f;
  { CfgLock c; lat = cfg.lat; lon = cfg.lon; f = cfg.fahrenheit; }
  if (isnan(lat)) {
    StateLock l;
    weather.error = "Set your location on the web page";
    weather.version++;
    return;
  }
  char url[400];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,apparent_temperature,relative_humidity_2m,weather_code,wind_speed_10m,is_day"
           "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
           "&timezone=auto&forecast_days=6%s",
           lat, lon, f ? "&temperature_unit=fahrenheit&wind_speed_unit=mph" : "");
  NetworkClientSecure tls;
  tls.setInsecure();
  HTTPClient http;
  http.useHTTP10(true);
  http.setTimeout(10000);
  if (!http.begin(tls, url)) return;
  int code = http.GET();
  if (code != 200) {
    http.end();
    StateLock l;
    weather.error = "Weather unavailable (" + String(code) + ")";
    weather.version++;
    return;
  }
  JsonDocument doc;
  DeserializationError e = deserializeJson(doc, http.getStream());
  http.end();
  if (e) return;

  StateLock l;
  JsonObject cur = doc["current"];
  weather.temp = cur["temperature_2m"] | 0.0f;
  weather.feels = cur["apparent_temperature"] | 0.0f;
  weather.humidity = cur["relative_humidity_2m"] | 0;
  weather.code = cur["weather_code"] | 0;
  weather.wind = cur["wind_speed_10m"] | 0.0f;
  weather.day = (cur["is_day"] | 1) == 1;
  JsonObject d = doc["daily"];
  int n = min<int>(6, d["time"].size());
  for (int i = 0; i < n; i++) {
    DayForecast& df = weather.days[i];
    df.code = d["weather_code"][i] | 0;
    df.hi = d["temperature_2m_max"][i] | 0.0f;
    df.lo = d["temperature_2m_min"][i] | 0.0f;
    df.rain = d["precipitation_probability_max"][i] | 0;
    // "YYYY-MM-DD" -> weekday (Sakamoto's method)
    const char* t = d["time"][i] | "2000-01-01";
    int y = atoi(t), m = atoi(t + 5), dd = atoi(t + 8);
    static const int k[] = {0, 3, 2, 5, 0, 3, 5, 1, 4, 6, 2, 4};
    if (m < 3) y--;
    df.wday = (y + y / 4 - y / 100 + y / 400 + k[(m + 11) % 12] + dd) % 7;
  }
  weather.ndays = n;
  weather.valid = true;
  weather.error = "";
  weather.updated = time(nullptr);
  weather.version++;
}

void weatherService() {
  if (!WiFi.isConnected()) return;
  if ((int32_t)(nextRefresh - millis()) > 0) return;
  nextRefresh = millis() + 15 * 60000UL;
  fetch();
}
