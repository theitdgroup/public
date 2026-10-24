// Improv Wi-Fi over USB serial (https://www.improv-wifi.com/serial/), so the
// browser installer (ESP Web Tools) can send Wi-Fi details right after flashing.
#pragma once
#include <Arduino.h>

// Feed one received byte. Returns false if the byte isn't part of an Improv
// packet, so the caller can treat it as plain console text.
bool improvFeed(uint8_t b);
void improvLoop();  // call from loop(): finishes a pending Wi-Fi connection
