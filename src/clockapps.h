// Alarms, countdown timer, radio sleep timer, night mode and countdowns.
// Runs on the UI task; ui.cpp draws the screens and the alert pop-up.
#pragma once
#include <Arduino.h>
#include <vector>

enum class AlertKind : uint8_t { None, Alarm, Timer, Message, Reminder };

struct AlertInfo {
  AlertKind kind = AlertKind::None;
  String title, detail;
  bool canSnooze = false;
  uint32_t id = 0;          // changes for every new alert
  bool photo = false;       // message comes with /msg.jpg
};

void appsTick();                    // call often from the UI loop
AlertInfo appsCurrentAlert();
void appsAlertStop();
void appsAlertSnooze();
void appsRaise(AlertKind kind, const String& title, const String& detail, bool sound = true, const char* url = "");
// Thread-safe: queue a message from the web page; shown by the UI task
void appsPostMessage(const String& from, const String& text, bool photo);

// Countdown timer
struct TimerInfo { uint32_t totalSec, remainingSec; bool running; };
TimerInfo appsTimer();
void timerAdjust(int deltaSec);     // while stopped: change the set time
void timerToggle();
void timerReset();

// Radio sleep timer
int sleepMinutesLeft();             // 0 = off
int sleepCycle();                   // off -> 15 -> 30 -> 60 -> 90 -> off; returns minutes

bool nightActive();
String nextAlarmText();             // e.g. "Tue 7:00 AM", "" if none
std::vector<String> countdownLines();
