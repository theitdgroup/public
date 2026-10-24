#include "es8311.h"

namespace es8311 {

static TwoWire* bus = nullptr;
static const uint8_t kAddr = 0x18;

static bool wr(uint8_t reg, uint8_t val) {
  bus->beginTransmission(kAddr);
  bus->write(reg);
  bus->write(val);
  return bus->endTransmission() == 0;
}

static uint8_t rd(uint8_t reg) {
  bus->beginTransmission(kAddr);
  bus->write(reg);
  bus->endTransmission(false);
  bus->requestFrom(kAddr, (uint8_t)1);
  return bus->available() ? bus->read() : 0;
}

bool begin(TwoWire& wire) {
  bus = &wire;
  bus->beginTransmission(kAddr);
  if (bus->endTransmission() != 0) return false;

  bool ok = wr(0x00, 0x1F);   // reset
  delay(20);
  ok &= wr(0x00, 0x00);
  ok &= wr(0x00, 0x80);       // power on, slave mode

  ok &= wr(0x01, 0x3F);       // MCLK from pin, all clocks on
  // Clock dividers for MCLK 12.288 MHz, fs 48 kHz:
  // pre_div 1, pre_multi 1x, adc/dac div 1, single speed, osr 0x10, lrck 0x00FF, bclk div 4
  ok &= wr(0x02, 0x00);
  ok &= wr(0x03, 0x10);
  ok &= wr(0x04, 0x10);
  ok &= wr(0x05, 0x00);
  ok &= wr(0x06, (rd(0x06) & 0xC0) | 0x03);  // SCLK not inverted, bclk_div-1
  ok &= wr(0x07, rd(0x07) & 0xC0);
  ok &= wr(0x08, 0xFF);

  ok &= wr(0x09, 0x0C);       // SDP in: I2S, 16-bit
  ok &= wr(0x0A, 0x0C);       // SDP out: I2S, 16-bit

  ok &= wr(0x0D, 0x01);       // power up analog
  ok &= wr(0x0E, 0x02);       // enable PGA / ADC modulator
  ok &= wr(0x12, 0x00);       // power up DAC
  ok &= wr(0x13, 0x10);       // enable output to HP drive
  ok &= wr(0x1C, 0x6A);       // ADC EQ bypass, DC offset cancel
  ok &= wr(0x37, 0x08);       // bypass DAC EQ
  ok &= mute(false);
  return ok;
}

bool setVolume(uint8_t v) {
  if (v > 100) v = 100;
  return wr(0x32, v == 0 ? 0 : (v * 256 / 100) - 1);
}

bool mute(bool m) {
  uint8_t r = rd(0x31);
  r = m ? (r | 0x60) : (r & ~0x60);
  return wr(0x31, r);
}

}  // namespace es8311
