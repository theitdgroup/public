#include "player.h"
#include <Audio.h>
#include <LittleFS.h>
#include <SD_MMC.h>
#include <math.h>
#include <algorithm>
#include "board.h"
#include "settings.h"
#include "storage.h"

struct PlayerCmd {
  Cmd cmd;
  int arg;
  char* str;  // heap copy, freed by the audio task
};

static QueueHandle_t queue;
static Audio* audio;
static std::vector<String> tracks;
static bool wantPlay = false;        // user intent: something should be playing
static uint32_t lastStart = 0;       // millis of the last connect attempt
static uint32_t retryDelay = 3000;
static volatile bool alerting = false;
static bool alertOnce = false;
static int resumeStation = -1;  // radio station to restart after a one-off chime
static String alertPath;

static void bump() { player.version++; }

static String baseName(const String& path) {
  int s = path.lastIndexOf('/');
  String n = path.substring(s + 1);
  int d = n.lastIndexOf('.');
  return d > 0 ? n.substring(0, d) : n;
}

static bool isAudioFile(const String& name) {
  String n = name;
  n.toLowerCase();
  for (const char* ext : {".mp3", ".m4a", ".aac", ".flac", ".wav", ".ogg", ".opus"})
    if (n.endsWith(ext)) return true;
  return false;
}

static void scanDir(fs::FS& fs, const String& dir, int depth, std::vector<String>& out) {
  File d = fs.open(dir);
  if (!d || !d.isDirectory()) return;
  while (out.size() < 1000) {
    File f = d.openNextFile();
    if (!f) break;
    String path = f.path();
    String name = f.name();
    bool isDir = f.isDirectory();
    f.close();
    if (name.startsWith(".") || name == "System Volume Information") continue;
    if (isDir) {
      if (depth < 4) scanDir(fs, path, depth + 1, out);
    } else if (isAudioFile(name)) {
      out.push_back(path);
    }
  }
}

static void mountAndScan() {
  std::vector<String> found;
  bool mounted = storageMount();
  if (mounted) {
    scanDir(SD_MMC, SD_MMC.exists("/music") ? "/music" : "/", 0, found);
    std::sort(found.begin(), found.end(), [](const String& a, const String& b) {
      String x = a, y = b;
      x.toLowerCase();
      y.toLowerCase();
      return x < y;
    });
  }
  StateLock l;
  tracks = found;
  player.sdMounted = mounted;
  player.trackCount = tracks.size();
  if (player.trackIdx >= (int)tracks.size()) player.trackIdx = -1;
  bump();
  log_i("SD %s, %u tracks", mounted ? "mounted" : "not found", (unsigned)tracks.size());
}

static void setNowPlaying(const String& title, const String& artist, const String& station) {
  StateLock l;
  player.title = title;
  player.artist = artist;
  player.album = "";
  player.station = station;
  player.error = "";
  player.posMs = player.durMs = 0;
  bump();
}

static void setError(const String& e) {
  StateLock l;
  player.error = e;
  player.busy = false;
  bump();
}

static void stopLocal() {
  audio->stopSong();
  wantPlay = false;
  StateLock l;
  player.playing = false;
  player.busy = false;
  bump();
}

static void startStation(int idx) {
  String name, url;
  {
    CfgLock c;
    if (cfg.stations.empty()) { setError("No stations configured"); return; }
    idx = (idx % (int)cfg.stations.size() + cfg.stations.size()) % cfg.stations.size();
    name = cfg.stations[idx].name;
    url = cfg.stations[idx].url;
  }
  {
    StateLock l;
    player.source = Source::Radio;
    player.stationIdx = idx;
    player.busy = true;
  }
  setNowPlaying("", "", name);
  audio->stopSong();
  wantPlay = true;
  lastStart = millis();
  bool ok = audio->connecttohost(url.c_str());
  StateLock l;
  player.busy = false;
  player.playing = ok;
  if (!ok) player.error = "Could not connect";
  bump();
}

static void startUrl(const String& url) {
  {
    StateLock l;
    player.source = Source::Radio;
    player.stationIdx = -1;
    player.busy = true;
  }
  setNowPlaying("", "", url);
  audio->stopSong();
  wantPlay = true;
  lastStart = millis();
  bool ok = audio->connecttohost(url.c_str());
  StateLock l;
  player.busy = false;
  player.playing = ok;
  if (!ok) player.error = "Could not connect";
  bump();
}

static void startTrack(int idx) {
  String path;
  if (!storageAvailable()) { setError(storageUsbActive() ? "SD card is in USB drive mode" : "No SD card"); return; }
  {
    StateLock l;
    if (tracks.empty()) { player.error = player.sdMounted ? "No music files on card" : "No SD card"; bump(); return; }
    idx = (idx % (int)tracks.size() + tracks.size()) % tracks.size();
    path = tracks[idx];
    player.source = Source::SD;
    player.trackIdx = idx;
  }
  setNowPlaying(baseName(path), "", "");
  audio->stopSong();
  wantPlay = true;
  bool ok = audio->connecttoFS(SD_MMC, path.c_str());
  StateLock l;
  player.playing = ok;
  if (!ok) player.error = "Can't play this file";
  bump();
}

static int nextTrackIndex(int dir) {
  StateLock l;
  if (tracks.empty()) return 0;
  if (player.shuffle && dir > 0) return random(tracks.size());
  return player.trackIdx + dir;
}

static void onAudioInfo(Audio::msg_t m) {
  if (!m.msg) return;
  String msg = m.msg;
  switch (m.e) {
    case Audio::evt_streamtitle: {
      StateLock l;
      int sep = msg.indexOf(" - ");
      if (sep > 0) { player.artist = msg.substring(0, sep); player.title = msg.substring(sep + 3); }
      else { player.artist = ""; player.title = msg; }
      bump();
      break;
    }
    case Audio::evt_name: {
      StateLock l;
      if (player.stationIdx < 0 || player.station.isEmpty()) { player.station = msg; bump(); }
      break;
    }
    case Audio::evt_id3data: {
      int colon = msg.indexOf(':');
      if (colon < 0) break;
      String key = msg.substring(0, colon);
      String val = msg.substring(colon + 1);
      val.trim();
      key.toLowerCase();
      StateLock l;
      if (key == "title" && val.length()) player.title = val;
      else if (key == "artist" && val.length()) player.artist = val;
      else if (key == "album" && val.length()) player.album = val;
      else break;
      bump();
      break;
    }
    case Audio::evt_eof: {
      bool sd;
      { StateLock l; sd = player.source == Source::SD; }
      if (alerting && alertOnce) {
        alerting = false;
        uint8_t v; { CfgLock cl; v = cfg.volume; } audio->setVolume(v);
        if (resumeStation >= 0) {
          PlayerCmd c{Cmd::PlayStation, resumeStation, nullptr};
          xQueueSend(queue, &c, 0);
          resumeStation = -1;
        }
      } else if (alerting) {
        PlayerCmd c{Cmd::AlertReplay, 0, nullptr};
        xQueueSend(queue, &c, 0);
      } else if (sd && wantPlay) {
        PlayerCmd c{Cmd::Next, 0, nullptr};
        xQueueSend(queue, &c, 0);
      }
      break;
    }
    default:
      log_v("audio %s: %s", m.s ? m.s : "", m.msg);
      break;
  }
}

static void handle(const PlayerCmd& c) {
  Source src;
  { StateLock l; src = player.source; }

  switch (c.cmd) {
    case Cmd::SetSource: {
      Source s = (Source)c.arg;
      if (s == src) break;
      stopLocal();
      { StateLock l; player.source = s; player.title = player.artist = player.album = player.station = player.error = ""; bump(); }
      break;
    }
    case Cmd::PlayStation: startStation(c.arg); break;
    case Cmd::PlayTrack:   startTrack(c.arg); break;
    case Cmd::PlayUrl:     if (c.str) startUrl(c.str); break;
    case Cmd::Toggle:
      if (src == Source::Radio) {
        if (audio->isRunning()) stopLocal();
        else { int i; { StateLock l; i = player.stationIdx; } startStation(i < 0 ? 0 : i); }
      } else if (src == Source::SD) {
        if (audio->isRunning() || audio->getAudioCurrentTime() > 0) {
          audio->pauseResume();
          wantPlay = audio->isRunning();
          StateLock l; player.playing = wantPlay; bump();
        } else {
          int i; { StateLock l; i = player.trackIdx; } startTrack(i < 0 ? 0 : i);
        }
      }
      break;
    case Cmd::Next:
    case Cmd::Prev: {
      int dir = c.cmd == Cmd::Next ? 1 : -1;
      if (src == Source::Radio) { int i; { StateLock l; i = player.stationIdx; } startStation(i + dir); }
      else if (src == Source::SD) startTrack(nextTrackIndex(dir));
      break;
    }
    case Cmd::Stop: stopLocal(); break;
    case Cmd::Volume: {
      uint8_t v = constrain(c.arg, 0, 21);
      audio->setVolume(v);
      { CfgLock cl; cfg.volume = v; }
      StateLock l; player.volume = v; bump();
      break;
    }
    case Cmd::Shuffle: { StateLock l; player.shuffle = c.arg; bump(); break; }
    case Cmd::Rescan: mountAndScan(); break;
    case Cmd::Chime:
    case Cmd::AlertStart:
      alertOnce = c.cmd == Cmd::Chime;
      { StateLock l; resumeStation = alertOnce && player.source == Source::Radio && player.playing ? player.stationIdx : -1; }
      stopLocal();
      // Served by our own web server: this task's stack is in PSRAM, so it must not
      // read the flash file system itself (flash access disables the PSRAM cache)
      alertPath = c.str ? String(c.str) : String("http://127.0.0.1/sounds/chime.wav");
      alerting = true;
      audio->setVolume(constrain(c.arg, 1, 21));
      audio->connecttohost(alertPath.c_str());
      break;
    case Cmd::AlertReplay:
      if (alerting) audio->connecttohost(alertPath.c_str());
      break;
    case Cmd::AlertStop:
      if (!alerting) break;
      alerting = false;
      audio->stopSong();
      { uint8_t v; { CfgLock cl; v = cfg.volume; } audio->setVolume(v); }
      break;
  }
}

static void audioTask(void*) {
  audio = new Audio();
  Audio::audio_info_callback = onAudioInfo;
  audio->setPinout(I2S_BCLK, I2S_LRCK, I2S_DOUT, I2S_MCLK);
  audio->setOutputSampleRate(Audio::SR_48000);
  audio->setConnectionTimeout(4000, 8000);
  uint8_t vol;
  { CfgLock c; vol = cfg.volume; }
  audio->setVolume(vol);
  { StateLock l; player.volume = vol; bump(); }
  mountAndScan();

  uint32_t lastPos = 0;
  for (;;) {
    PlayerCmd c;
    while (xQueueReceive(queue, &c, 0) == pdTRUE) {
      handle(c);
      free(c.str);
    }

    audio->loop();

    uint32_t now = millis();
    if (now - lastPos > 500) {
      lastPos = now;
      StateLock l;
      if (player.source == Source::SD || player.source == Source::Radio) {
        bool running = audio->isRunning();
        uint32_t pos = audio->getAudioCurrentTime() * 1000, dur = audio->getAudioFileDuration() * 1000;
        if (running != player.playing || pos != player.posMs || dur != player.durMs) {
          player.playing = running;
          player.posMs = pos;
          player.durMs = dur;
          bump();
        }
        // Radio streams drop occasionally; reconnect with back-off while the user wants audio
        if (player.source == Source::Radio && wantPlay && !running && !player.busy && now - lastStart > retryDelay) {
          int i = player.stationIdx;
          retryDelay = min<uint32_t>(retryDelay * 2, 60000);
          PlayerCmd rc{Cmd::PlayStation, i < 0 ? 0 : i, nullptr};
          if (i >= 0) xQueueSend(queue, &rc, 0);
        } else if (running) {
          retryDelay = 3000;
        }
      }
    }
    vTaskDelay(1);
  }
}

// Two-note bell ("ding-dong") synthesised once and stored as a WAV file
void playerMakeChime() {
  if (LittleFS.exists("/sounds/chime.wav")) return;
  LittleFS.mkdir("/sounds");
  const int rate = 22050, len = rate * 13 / 10;
  File f = LittleFS.open("/sounds/chime.wav", "w");
  if (!f) return;
  auto u32 = [&](uint32_t v) { f.write((uint8_t*)&v, 4); };
  auto u16 = [&](uint16_t v) { f.write((uint8_t*)&v, 2); };
  f.write((const uint8_t*)"RIFF", 4); u32(36 + len * 2); f.write((const uint8_t*)"WAVEfmt ", 8);
  u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  f.write((const uint8_t*)"data", 4); u32(len * 2);
  int16_t buf[256];
  int n = 0;
  for (int i = 0; i < len; i++) {
    float t = (float)i / rate, v = 0;
    // note 1 (E5) at 0 s, note 2 (C5) at 0.45 s; bell = fundamental + overtones with decay
    for (int k = 0; k < 2; k++) {
      float t0 = k * 0.45f, fq = k ? 523.25f : 659.25f;
      if (t < t0) continue;
      float tt = t - t0, env = expf(-3.2f * tt) * fminf(1.0f, tt * 400);
      v += env * (sinf(2 * M_PI * fq * tt) + 0.45f * sinf(2 * M_PI * fq * 2.76f * tt) + 0.2f * sinf(2 * M_PI * fq * 5.4f * tt));
    }
    buf[n++] = (int16_t)constrain(v * 9000, -32000, 32000);
    if (n == 256) { f.write((uint8_t*)buf, sizeof(buf)); n = 0; }
  }
  if (n) f.write((uint8_t*)buf, n * 2);
  f.close();
}

// Reveille bugle call, synthesised once and stored as a WAV file.
// Bugles only play the natural notes G4 C5 E5 G5; durations are in sixteenths.
static const char* reveilleUrl = "http://127.0.0.1/sounds/reveille.wav";
const char* playerReveilleUrl() { return reveilleUrl; }

void playerMakeReveille() {
  // A recording in /sounds/reveille.mp3 wins over the synthesised version
  if (LittleFS.exists("/sounds/reveille.mp3")) {
    reveilleUrl = "http://127.0.0.1/sounds/reveille.mp3";
    if (LittleFS.exists("/sounds/reveille.wav")) LittleFS.remove("/sounds/reveille.wav");
    return;
  }
  enum { G4, C5, E5, G5 };
  static const float hz[] = {392.00f, 523.25f, 659.25f, 783.99f};
  static const uint8_t tune[][2] = {
    {G4, 2},
    // "I can't get 'em up, I can't get 'em up, I can't get 'em up in the morning"
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {E5, 1}, {C5, 1},
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {E5, 1}, {C5, 1},
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {C5, 1}, {E5, 1},
    {G5, 4}, {E5, 2}, {G4, 2},
    // "I can't get 'em up, I can't get 'em up, I can't get 'em up at all"
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {E5, 1}, {C5, 1},
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {E5, 1}, {C5, 1},
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {C5, 1}, {E5, 1},
    {E5, 2}, {G4, 2}, {C5, 4},
    // "The corporal's worse than the privates, the sergeant's worse than the corporal"
    {E5, 2}, {E5, 1}, {E5, 1}, {E5, 2}, {E5, 2},
    {G5, 2}, {E5, 2}, {C5, 4},
    {E5, 2}, {E5, 1}, {E5, 1}, {E5, 2}, {E5, 2},
    {G5, 2}, {E5, 2}, {C5, 2}, {G4, 2},
    // "...and the captain's the worst of all"
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {E5, 1}, {C5, 1},
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {E5, 1}, {C5, 1},
    {C5, 2}, {E5, 1}, {C5, 1}, {G4, 2}, {C5, 1}, {E5, 1},
    {E5, 2}, {G4, 2}, {C5, 6},
  };
  const int rate = 16000, sixteenth = rate / 10, tail = rate * 3 / 2;  // 150 bpm, then 1.5 s quiet
  int len = tail;
  for (auto& n : tune) len += n[1] * sixteenth;
  // Regenerate if missing or written by an older tune
  {
    File old = LittleFS.open("/sounds/reveille.wav", "r");
    bool same = old && old.size() == (size_t)(44 + len * 2);
    if (old) old.close();
    if (same) return;
  }
  LittleFS.mkdir("/sounds");
  File f = LittleFS.open("/sounds/reveille.wav", "w");
  if (!f) return;
  auto u32 = [&](uint32_t v) { f.write((uint8_t*)&v, 4); };
  auto u16 = [&](uint16_t v) { f.write((uint8_t*)&v, 2); };
  f.write((const uint8_t*)"RIFF", 4); u32(36 + len * 2); f.write((const uint8_t*)"WAVEfmt ", 8);
  u32(16); u16(1); u16(1); u32(rate); u32(rate * 2); u16(2); u16(16);
  f.write((const uint8_t*)"data", 4); u32(len * 2);
  // Brassy tone: strong low harmonics, quick attack, a short gap between notes (tonguing)
  static const float harm[] = {1.0f, 0.75f, 0.55f, 0.38f, 0.22f, 0.12f};
  int16_t buf[256];
  int n = 0;
  auto out = [&](float v) {
    buf[n++] = (int16_t)constrain(v * 9500, -32000, 32000);
    if (n == 256) { f.write((uint8_t*)buf, sizeof(buf)); n = 0; }
  };
  for (auto& note : tune) {
    int samples = note[1] * sixteenth, gap = rate * 3 / 100;
    float fq = hz[note[0]];
    for (int i = 0; i < samples; i++) {
      float t = (float)i / rate;
      float env = fminf(1.0f, t * 60) * (i < samples - gap ? 1.0f : 0.0f);
      env *= 1.0f - 0.25f * fminf(1.0f, t * 4);           // settle after the attack
      float vib = 1.0f + (t > 0.25f ? 0.004f * sinf(2 * M_PI * 5.5f * t) : 0);
      float v = 0;
      for (int k = 0; k < 6; k++) v += harm[k] * sinf(2 * M_PI * fq * (k + 1) * vib * t);
      out(env * v * 0.55f);
    }
  }
  for (int i = 0; i < tail; i++) out(0);
  if (n) f.write((uint8_t*)buf, n * 2);
  f.close();
}

void playerInit() {
  queue = xQueueCreate(16, sizeof(PlayerCmd));
  // Audio decode on core 0, away from LVGL rendering on core 1
  // Stack in PSRAM to leave internal RAM for TLS; this task never writes flash
  xTaskCreatePinnedToCoreWithCaps(audioTask, "audio", 12288, nullptr, 3, nullptr, 0, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void playerSend(Cmd c, int arg, const String& str) {
  PlayerCmd pc{c, arg, str.length() ? strdup(str.c_str()) : nullptr};
  if (xQueueSend(queue, &pc, pdMS_TO_TICKS(50)) != pdTRUE) free(pc.str);
}

std::vector<String> playerTracks() {
  StateLock l;
  return tracks;
}
