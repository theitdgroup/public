// Minimal ES8311 codec setup: I2S slave, 16-bit, 48 kHz, MCLK = 256*fs from the MCLK pin.
// The audio library resamples everything to 48 kHz, so the codec clocks never change.
// Register sequence follows Espressif's es8311 driver used in Waveshare's demo.
#pragma once
#include <Arduino.h>
#include <Wire.h>

namespace es8311 {
bool begin(TwoWire& wire);       // Wire must already be started
bool setVolume(uint8_t percent); // DAC digital volume, 0-100
bool mute(bool m);
}
