// microSD ownership: the watch (music player, web uploads, network drive) or
// the PC over USB ("USB drive mode"). Never both at once, or the card corrupts.
#pragma once
#include <Arduino.h>

void storageInit();           // call first thing in setup(), registers the USB drive
bool storageMount();          // mount the card for the watch's own use; true if mounted
bool storageAvailable();      // mounted and not lent to the PC
bool storageUsbActive();      // card currently shown to the PC as a USB drive
bool storageUsbStart(String* err = nullptr);  // stops music and hands the card to the PC
void storageUsbStop();        // takes the card back and rescans music
