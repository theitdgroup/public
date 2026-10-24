// KY-040 rotary encoder on the 8-pin header (GPIO16/17/18 by default).
#pragma once
#include <Arduino.h>

enum class KnobPress : uint8_t { None, Short, Long };

void encoderInit();            // reads pins from cfg; safe to call again after settings change
int encoderTakeSteps();        // detents turned since last call (+ clockwise)
KnobPress encoderTakePress();  // call from the UI loop; debounced
// Wiring test: levels of GPIO16/17/18 with pull-ups and with pull-downs (bit2=16, bit1=17, bit0=18)
void encoderWiringTest(int& withPullUp, int& withPullDown);
void encoderDebug(long& transitions, int& pinLevels, long& presses);  // for /api/status
