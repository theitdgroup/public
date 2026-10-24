// Wi-Fi (with setup access point fallback), mDNS, NTP, and the background
// network task that also runs the news, weather and other online services.
#pragma once
#include <Arduino.h>

void netInit();             // starts the network task
void netReconnect();        // re-read Wi-Fi settings and reconnect
void netApplyTime();        // apply timezone / NTP server from cfg
bool netTakeRtcSync();      // true once after NTP sync -> UI task writes the RTC
