ESP32-audioI2S 3.4.7 (https://github.com/schreibfaul1/ESP32-audioI2S), vendored for one change:

Audio::parseHttpResponseHeader() waited for the HTTP response header in a loop that never
yielded. The watch fetches alarm and face sounds from its own web server over loopback; if
that server was busy (e.g. receiving an upload) the audio task (priority 3, core 0) spun for
up to 5 s and starved the idle task, so the task watchdog reset the watch. The loop now
calls vTaskDelay(1) while no data is available.
