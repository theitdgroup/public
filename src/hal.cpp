#include "hal.h"
#include <Arduino_GFX_Library.h>
#include <RtcDrv.hpp>
#include <TouchDrv.hpp>
#include <Wire.h>
#define XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>
#include <lvgl.h>
#include <sys/time.h>
#include "board.h"
#include "es8311.h"
#include "state.h"

static Arduino_DataBus* bus = new Arduino_ESP32QSPI(LCD_CS, LCD_SCLK, LCD_D0, LCD_D1, LCD_D2, LCD_D3);
static Arduino_CO5300* gfx = new Arduino_CO5300(bus, LCD_RST, 0, LCD_W, LCD_H, LCD_COL_OFFSET, 0, 0, 0);
static TouchDrvCST92xx touch;
static XPowersAXP2101 pmu;
static SensorPCF85063 rtc;

static bool touchOk = false, pmuOk = false, rtcOk = false;
bool halCodecOk = false;  // sound chip answered at start-up (shown in /api/status)

static volatile uint32_t lastActivity = 0;
static bool screenOn = true;
static bool swallowTouch = false;   // finger that woke the screen shouldn't click anything

volatile uint32_t halFlushUs = 0;  // time spent sending pixels to the panel (diagnostics)

static void flushCb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
  uint32_t t0 = micros();
  gfx->draw16bitRGBBitmap(a->x1, a->y1, (uint16_t*)px, lv_area_get_width(a), lv_area_get_height(a));
  halFlushUs += micros() - t0;
  lv_display_flush_ready(disp);
}

// CO5300 needs even start coordinates and even widths/heights
static void rounderCb(lv_event_t* e) {
  lv_area_t* a = (lv_area_t*)lv_event_get_param(e);
  a->x1 &= ~1;
  a->y1 &= ~1;
  a->x2 |= 1;
  a->y2 |= 1;
}

static void touchCb(lv_indev_t*, lv_indev_data_t* data) {
  data->state = LV_INDEV_STATE_RELEASED;
  if (!touchOk) return;
  const TouchPoints& tp = touch.getTouchPoints();
  if (!tp.hasPoints()) { swallowTouch = false; return; }
  lastActivity = millis();
  if (!screenOn) { halScreenOn(true); swallowTouch = true; }
  if (swallowTouch) return;
  data->point.x = tp[0].x;
  data->point.y = tp[0].y;
  data->state = LV_INDEV_STATE_PRESSED;
}

static void rtcToSystem() {
  RTC_DateTime dt = rtc.getDateTime();
  if (dt.getYear() < 2024 || dt.getYear() > 2099) return;  // RTC never set / lost power
  // RTC keeps UTC; TZ is still UTC here because net hasn't applied the user's zone
  setenv("TZ", "UTC0", 1);
  tzset();
  struct tm t = {};
  t.tm_year = dt.getYear() - 1900;
  t.tm_mon = dt.getMonth() - 1;
  t.tm_mday = dt.getDay();
  t.tm_hour = dt.getHour();
  t.tm_min = dt.getMinute();
  t.tm_sec = dt.getSecond();
  struct timeval tv = {mktime(&t), 0};
  settimeofday(&tv, nullptr);
  log_i("Time restored from RTC");
}

void halWriteRtcFromSystem() {
  if (!rtcOk) return;
  time_t now = time(nullptr);
  struct tm t;
  gmtime_r(&now, &t);
  rtc.setDateTime(RTC_DateTime(t));
}

void halInit() {
  Wire.begin(I2C_SDA, I2C_SCL, 400000);

  // Power management first so the rails are known-good
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, I2C_SDA, I2C_SCL);
  if (pmuOk) {
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();
    pmu.enableVbusVoltageMeasure();
    pmu.enableSystemVoltageMeasure();
  }

  rtcOk = rtc.begin(Wire, I2C_SDA, I2C_SCL);
  if (rtcOk) rtcToSystem();

  // Speaker amp + codec
  pinMode(PA_EN, OUTPUT);
  digitalWrite(PA_EN, HIGH);
  halCodecOk = es8311::begin(Wire);
  if (!halCodecOk) log_e("ES8311 not found");
  es8311::setVolume(85);

  // Touch controller
  pinMode(TP_RST, OUTPUT);
  digitalWrite(TP_RST, LOW);
  delay(30);
  digitalWrite(TP_RST, HIGH);
  delay(80);
  touch.setPins(TP_RST, TP_INT);
  touchOk = touch.begin(Wire, TP_ADDR, I2C_SDA, I2C_SCL);
  if (touchOk) {
    touch.setMaxCoordinates(LCD_W, LCD_H);
    touch.setMirrorXY(true, true);
  } else {
    log_e("Touch controller not found");
  }

  // Display
  gfx->begin();
  gfx->fillScreen(RGB565_BLACK);
  gfx->setBrightness(200);

  lv_init();
  lv_tick_set_cb([]() -> uint32_t { return millis(); });
  lv_display_t* disp = lv_display_create(LCD_W, LCD_H);
  // Draw buffers in PSRAM; Arduino_GFX copies into its own DMA buffer while sending
  size_t bufBytes = LCD_W * (LCD_H / 4) * 2;
  void* b1 = heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  void* b2 = heap_caps_malloc(bufBytes, MALLOC_CAP_SPIRAM);
  lv_display_set_buffers(disp, b1, b2, bufBytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(disp, flushCb);
  lv_display_add_event_cb(disp, rounderCb, LV_EVENT_INVALIDATE_AREA, nullptr);

  lv_indev_t* indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchCb);

  lastActivity = millis();
  halUpdatePower();
}

void halSetBrightness(uint8_t level) {
  if (screenOn) gfx->setBrightness(level);
}

void halScreenOn(bool on) {
  if (on == screenOn) return;
  screenOn = on;
  if (on) { gfx->displayOn(); lastActivity = millis(); }
  else { gfx->setBrightness(0); gfx->displayOff(); }
}

bool halScreenIsOn() { return screenOn; }
uint32_t halIdleMs() { return millis() - lastActivity; }
void halPokeActivity() { lastActivity = millis(); }

void halUpdatePower() {
  if (!pmuOk) return;
  power.present = true;
  power.battery = pmu.isBatteryConnect();
  power.charging = pmu.isCharging();
  power.usb = pmu.isVbusIn();
  power.percent = power.battery ? pmu.getBatteryPercent() : -1;
}

