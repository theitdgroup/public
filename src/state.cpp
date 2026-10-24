#include "state.h"

PlayerState player;
std::vector<NewsItem> news;
uint32_t newsVersion = 0;
String newsStatus = "Waiting for Wi-Fi";
NetState net;
WeatherState weather;
std::vector<CalEvent> events;
uint32_t eventsVersion = 0;
String calendarStatus;
std::vector<Game> games;
uint32_t gamesVersion = 0;
PowerState power;
SemaphoreHandle_t stateMutex;

const char* sourceName(Source s) {
  switch (s) {
    case Source::Radio:   return "radio";
    case Source::SD:      return "sd";
    default:              return "none";
  }
}

void stateInit() { stateMutex = xSemaphoreCreateRecursiveMutex(); }
