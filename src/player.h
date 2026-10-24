// Music: internet radio + microSD playback through the ES8311.
#pragma once
#include <Arduino.h>
#include <vector>
#include "state.h"

enum class Cmd : uint8_t {
  SetSource,    // arg = (int)Source
  PlayStation,  // arg = station index
  PlayTrack,    // arg = track index
  PlayUrl,      // str = stream URL
  Toggle,       // play/pause
  Next,
  Prev,
  Stop,
  Volume,       // arg = 0..21
  Shuffle,      // arg = 0/1
  Rescan,       // re-read the SD card
  AlertStart,   // loop a sound file (str, default chime) at volume arg until AlertStop
  AlertStop,
  Chime,        // play the chime once at volume arg
  AlertReplay,  // internal: restart the alert sound when it ends
};

void playerInit();
void playerSend(Cmd c, int arg = 0, const String& str = String());
std::vector<String> playerTracks();  // copy of the SD track list
const char* playerReveilleUrl();     // local URL of the reveille sound (recording or synthesised)
void playerMakeReveille();           // writes /sounds/reveille.wav if missing (call from setup)
void playerMakeChime();              // writes /sounds/chime.wav if missing (call from setup)
