// Scores for followed teams from ESPN's public scoreboard feeds. Network task.
#pragma once
#include <Arduino.h>

void sportsService();       // every minute while a game is live, else every 15 minutes
void sportsRefreshNow();
// Pop-up events (game started / final) for the UI task to show, oldest first
bool sportsTakeAlert(String& title, String& detail);
// Used by the web page's team picker (runs on the web task): JSON [{id,abbr,name}]
String sportsTeamList(const String& sport, const String& league);
String sportsLastError();
