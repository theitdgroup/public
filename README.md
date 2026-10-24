# ClockHub firmware

ClockHub turns a **Waveshare ESP32-S3-Touch-AMOLED-1.75** (466×466 round AMOLED) into a smart desk clock:
a gallery of clock faces (some animated), internet radio and SD-card music through the built-in speaker,
weather, news headlines, calendar and week view, sports scores, a Bible verse of the day, alarms and
timers, a photo slideshow, and messages — all set up from a web page on your phone.

This repository is the complete source code for the firmware that ships on ClockHub clocks.

## Hardware

- Waveshare ESP32-S3-Touch-AMOLED-1.75 (ESP32-S3, 16 MB flash, 8 MB PSRAM, CO5300 display,
  CST9217 touch, ES8311 audio codec, AXP2101 power management, PCF85063 clock)
- Optional KY-040 rotary encoder: CLK → GPIO 18, DT → GPIO 17, SW → GPIO 16, + → 3.3 V, GND → GND
- microSD card (recommended) for music, photos and the face gallery
- A small speaker on the board's speaker connector

## Building

The project uses [PlatformIO](https://platformio.org/) with the pioarduino ESP32 platform (Arduino core 3.x)
and LVGL 9.

```
pio run                      # build the firmware
pio run -t upload            # flash it over USB
pio run -t uploadfs          # flash the web page, faces and sounds (data/) - erases settings
```

`tools/build_installer.py` builds a browser installer (ESP Web Tools) with the firmware and file system image.

## Layout

| Folder | What's in it |
|---|---|
| `src/` | Firmware: display/UI (LVGL), faces, audio, networking, web server and settings page API |
| `lib/ESP32-audioI2S/` | ESP32-audioI2S 3.4.7 with one watchdog fix (see its `CLOCKHUB.md`) |
| `include/` | LVGL configuration |
| `data/` | Files for the clock's flash: the settings web page, face gallery, Bible verses, sounds |
| `gallery-extra/` | Source artwork and generator scripts for the ClockHub faces |
| `tools/` | Build and helper scripts |
| `installer/` | Template for the browser installer page |

Clock faces are JSON files (see the built-in faces in `data/gallery/faces/`) with optional LVGL binary images.

## License

ClockHub firmware is released under the **GNU General Public License v3.0** — see `LICENSE`.
It is built on open-source components, each under its own license:

- [LVGL](https://lvgl.io) — MIT
- [Arduino core for ESP32](https://github.com/espressif/arduino-esp32) — LGPL 2.1
- [Arduino_GFX](https://github.com/moononournation/Arduino_GFX) — BSD
- [ArduinoJson](https://arduinojson.org) — MIT
- [SensorLib](https://github.com/lewisxhe/SensorLib) and [XPowersLib](https://github.com/lewisxhe/XPowersLib) — MIT
- [ESP32-audioI2S](https://github.com/schreibfaul1/ESP32-audioI2S) — GPL 3.0 (included in `lib/`)

Content:

- `data/sounds/reveille.mp3` — "Reveille Sound Effect" from Free Sounds Library, CC BY 4.0 (see `data/sounds/CREDITS.txt`)
- `data/bible/verses.txt` — King James Version (public domain), retrieved via bible-api.com
- Verse of the Day is fetched at run time from Bible Gateway
