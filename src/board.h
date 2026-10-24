// Pin map for the Waveshare ESP32-S3-Touch-AMOLED-1.75.
// Taken from Waveshare's BSP (esp32_s3_touch_amoled_1_75.h) and Arduino demos.
#pragma once

// CO5300 AMOLED panel (QSPI)
#define LCD_CS      12
#define LCD_SCLK    38
#define LCD_D0      4
#define LCD_D1      5
#define LCD_D2      6
#define LCD_D3      7
#define LCD_RST     39
#define LCD_W       466
#define LCD_H       466
#define LCD_COL_OFFSET 6

// Shared I2C bus: CST9217 touch (0x5A), ES8311 codec (0x18), AXP2101 PMU,
// PCF85063 RTC, QMI8658 IMU, TCA9554 expander, ES7210 mic ADC
#define I2C_SDA     15
#define I2C_SCL     14
#define TP_INT      11
#define TP_RST      40
#define TP_ADDR     0x5A

// ES8311 codec over I2S
#define I2S_MCLK    42
#define I2S_BCLK    9
#define I2S_LRCK    45
#define I2S_DOUT    8   // ESP32 -> codec DSDIN
#define I2S_DIN     10  // codec ASDOUT -> ESP32
#define PA_EN       46  // speaker amplifier enable

// microSD (SDMMC, 1-bit)
#define SD_CLK      2
#define SD_CMD      1
#define SD_D0       3
#define SD_D3       41
