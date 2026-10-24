#include "encoder.h"
#include "settings.h"

static int pinA = -1, pinB = -1, pinBtn = -1;
static bool reverse = false;
static uint8_t stepsPerDetent = 4;
static volatile int32_t transitions = 0;
static volatile uint8_t lastState = 0;
static int32_t consumed = 0;

// Quadrature decoding: index = (previous AB << 2) | current AB
static const int8_t kTable[16] = {0, -1, 1, 0, 1, 0, 0, -1, -1, 0, 0, 1, 0, 1, -1, 0};

static void IRAM_ATTR onEdge() {
  uint8_t s = (digitalRead(pinA) << 1) | digitalRead(pinB);
  transitions += kTable[(lastState << 2) | s];
  lastState = s;
}

void encoderInit() {
  int a, b, btn;
  {
    CfgLock c;
    if (!cfg.encoderEnabled) a = b = btn = -1;
    else { a = cfg.encoderA; b = cfg.encoderB; btn = cfg.encoderBtn; }
    reverse = cfg.encoderReverse;
    stepsPerDetent = cfg.encoderStepsPerDetent;
  }
  if (pinA >= 0) { detachInterrupt(pinA); detachInterrupt(pinB); }
  pinA = a; pinB = b; pinBtn = btn;
  if (pinA < 0 || pinB < 0) return;
  // KY-040 boards have pull-ups on CLK/DT but often none on SW
  pinMode(pinA, INPUT_PULLUP);
  pinMode(pinB, INPUT_PULLUP);
  if (pinBtn >= 0) pinMode(pinBtn, INPUT_PULLUP);
  lastState = (digitalRead(pinA) << 1) | digitalRead(pinB);
  transitions = consumed = 0;
  attachInterrupt(pinA, onEdge, CHANGE);
  attachInterrupt(pinB, onEdge, CHANGE);
  log_i("Rotary encoder on A=%d B=%d SW=%d", pinA, pinB, pinBtn);
}

static volatile long pressCount = 0;

void encoderWiringTest(int& up, int& down) {
  auto read3 = [](uint8_t mode) {
    for (int p : {16, 17, 18}) pinMode(p, mode);
    delay(5);
    return (digitalRead(16) << 2) | (digitalRead(17) << 1) | digitalRead(18);
  };
  if (pinA >= 0) { detachInterrupt(pinA); detachInterrupt(pinB); }
  up = read3(INPUT_PULLUP);
  down = read3(INPUT_PULLDOWN);
  pinA = -1;
  encoderInit();  // back to normal
}

void encoderDebug(long& t, int& levels, long& presses) {
  t = transitions;
  levels = pinA < 0 ? -1 : (digitalRead(pinA) << 2) | (digitalRead(pinB) << 1) | (pinBtn >= 0 ? digitalRead(pinBtn) : 1);
  presses = pressCount;
}

int encoderTakeSteps() {
#ifdef ENCODER_DIAG
  // Wiring check: report header pin levels and edge count whenever they change
  static uint32_t lastLog = 0;
  static int lastLevels = -1;
  static int32_t lastT = 0;
  int levels = (digitalRead(16) << 2) | (digitalRead(17) << 1) | digitalRead(18);
  if ((levels != lastLevels || transitions != lastT) && millis() - lastLog > 50) {
    lastLog = millis();
    lastLevels = levels;
    lastT = transitions;
    log_i("diag GPIO16=%d GPIO17=%d GPIO18=%d transitions=%ld", (levels >> 2) & 1, (levels >> 1) & 1, levels & 1, (long)transitions);
  }
#endif
  if (pinA < 0) return 0;
  int32_t t = transitions;
  int steps = (t - consumed) / (int)max<uint8_t>(1, stepsPerDetent);
  consumed += steps * max<uint8_t>(1, stepsPerDetent);
  return reverse ? -steps : steps;
}

KnobPress encoderTakePress() {
  static bool down = false, longSent = false;
  static uint32_t changedAt = 0, pressedAt = 0;
  static bool stable = false;
  static uint32_t releasedSince = 0;  // when the button was last seen up and settled
  if (pinBtn < 0) return KnobPress::None;
  bool raw = digitalRead(pinBtn) == LOW;
  uint32_t now = millis();
  if (raw != stable) { stable = raw; changedAt = now; }
  if (now - changedAt < 25) return KnobPress::None;  // debounce
  // Power-up guard: while the watch starts (and the button input settles) a press can be
  // read that nobody made; it used to step the clock to the next face. Only accept presses
  // after 3 s of uptime, and only once the button has been seen released for half a second.
  if (!stable && !releasedSince) releasedSince = now;
  if (stable && !down && (now < 3000 || !releasedSince || now - releasedSince < 500)) { releasedSince = 0; return KnobPress::None; }

  if (stable && !down) { down = true; longSent = false; pressedAt = now; pressCount++; }
  if (stable && down && !longSent && now - pressedAt > 700) { longSent = true; return KnobPress::Long; }
  if (!stable && down) {
    down = false;
    if (!longSent) return KnobPress::Short;
  }
  return KnobPress::None;
}
