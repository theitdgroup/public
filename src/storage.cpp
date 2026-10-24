#include "storage.h"
#include <SD_MMC.h>
#include <USB.h>
#include <USBMSC.h>
#include <diskio.h>
#include "board.h"
#include "player.h"
#include "state.h"

static USBMSC msc;
static volatile bool usbActive = false;
static bool mounted = false;

// SD_MMC keeps the FatFs drive number private; reach it through a derived view
struct SdAccess : fs::SDMMCFS {
  static uint8_t pdrv(fs::SDMMCFS& s) { return static_cast<SdAccess&>(s)._pdrv; }
};

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  if (!usbActive || offset) return -1;
  uint32_t n = bufsize / 512;
  return disk_read(SdAccess::pdrv(SD_MMC), (BYTE*)buffer, lba, n) == RES_OK ? (int32_t)bufsize : -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  if (!usbActive || offset) return -1;
  uint32_t n = bufsize / 512;
  return disk_write(SdAccess::pdrv(SD_MMC), buffer, lba, n) == RES_OK ? (int32_t)bufsize : -1;
}

static bool onStartStop(uint8_t, bool start, bool loadEject) {
  // "Eject" in Windows: hand the card back to the watch
  if (!start && loadEject) storageUsbStop();
  return true;
}

void storageInit() {
  msc.vendorID("ClockHub");
  msc.productID("Watch SD card");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(false);
  msc.begin(0, 512);  // real size is set when USB drive mode starts
}

bool storageMount() {
  if (usbActive) return false;
  if (mounted && SD_MMC.cardType() != CARD_NONE) return true;
  SD_MMC.end();
  pinMode(SD_D3, OUTPUT);
  digitalWrite(SD_D3, HIGH);
  SD_MMC.setPins(SD_CLK, SD_CMD, SD_D0);
  mounted = SD_MMC.begin("/sdcard", true /* 1-bit */);
  return mounted;
}

bool storageAvailable() { return mounted && !usbActive; }
bool storageUsbActive() { return usbActive; }

bool storageUsbStart(String* err) {
  if (usbActive) return true;
  playerSend(Cmd::Stop);
  delay(300);  // let the audio task close any open file
  if (!storageMount()) { if (err) *err = "No SD card in the watch"; return false; }
  if (!msc.begin(SD_MMC.numSectors(), SD_MMC.sectorSize())) { if (err) *err = "USB drive could not start"; return false; }
  usbActive = true;
  msc.mediaPresent(true);
  StateLock l;
  player.version++;
  return true;
}

void storageUsbStop() {
  if (!usbActive) return;
  msc.mediaPresent(false);
  usbActive = false;
  mounted = false;           // force a fresh mount: the PC may have changed everything
  playerSend(Cmd::Rescan);
}
