// Host-side unit tests for burst_flash_pure.h.
// Compile on any C++17 host (no Arduino / ESP-IDF required):
//   g++ -std=c++17 -I. tests/test_burst_flash_pure.cpp -o /tmp/bf && /tmp/bf
#include <cstdio>
#include <cstring>
#include <string>
#include "burst_flash_pure.h"

static int g_checks = 0;
static int g_fails = 0;

static void check(bool ok, const char *what) {
  g_checks++;
  if (!ok) {
    g_fails++;
    std::printf("FAIL: %s\n", what);
  }
}

static std::string hex(const uint8_t *p, uint8_t n) {
  char b[32];
  std::string s;
  for (uint8_t i = 0; i < n; i++) {
    std::snprintf(b, sizeof(b), "%02X", p[i]);
    s += b;
  }
  return s;
}

static BurstFlashConfigPure enabledConfig() {
  BurstFlashConfigPure c = burstFlashDefaultConfigPure();
  c.masterEnabled = true;
  c.ch[BURST_FLASH_FRONT_PURE].enabled = true;
  c.ch[BURST_FLASH_FRONT_PURE].templateValid = true;
  c.ch[BURST_FLASH_REAR_PURE].enabled = true;
  c.ch[BURST_FLASH_REAR_PURE].templateValid = true;
  return c;
}

static void testDefaults() {
  BurstFlashConfigPure c = burstFlashDefaultConfigPure();
  check(!c.masterEnabled, "default master must be disabled");
  check(c.requireStationary && !c.allowUnknownSpeed, "default speed gate must be strict");
  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++) {
    check(!c.ch[i].enabled, "default channel must be disabled");
    check(!c.ch[i].templateValid, "default template must be invalid");
    check(!c.ch[i].learned, "default template must not be marked learned");
  }
  check(c.ch[0].canId == 0x249 && c.ch[0].dlc == 4, "front is 0x249 DLC4");
  check(c.ch[1].canId == 0x3F4 && c.ch[1].dlc == 8, "rear is 0x3F4 DLC8");
  check(c.ch[0].mask[1] == 0x30, "front owns byte1 bit4-5");
  check(c.ch[1].mask[7] == 0x0E, "rear owns byte7 bit1-3");
  check(c.ch[1].mask[0] == 0x1F, "rear owns MUX selector");
}

static void testSanitize() {
  BurstFlashConfigPure c = enabledConfig();
  check(burstFlashSanitizePure(c), "valid config accepted");

  c = enabledConfig();
  c.ch[0].count = 0;
  check(burstFlashSanitizePure(c) && c.ch[0].count == BURST_FLASH_MIN_COUNT_PURE,
        "count clamped to minimum");
  c = enabledConfig();
  c.ch[0].count = 200;
  check(burstFlashSanitizePure(c) && c.ch[0].count == BURST_FLASH_MAX_COUNT_PURE,
        "count clamped to maximum");
  c = enabledConfig();
  c.ch[0].onMs = 1; c.ch[0].offMs = 60000;
  check(burstFlashSanitizePure(c) &&
        c.ch[0].onMs == BURST_FLASH_MIN_PHASE_MS_PURE &&
        c.ch[0].offMs == BURST_FLASH_MAX_PHASE_MS_PURE,
        "phase durations clamped");

  c = enabledConfig();
  c.ch[0].dlc = 9;
  check(!burstFlashSanitizePure(c), "DLC > 8 rejected");
  c = enabledConfig();
  c.ch[1].canId = 0x800;
  check(!burstFlashSanitizePure(c), "extended CAN id rejected");
  c = enabledConfig();
  c.ch[0].mask[6] = 0x01;
  check(!burstFlashSanitizePure(c), "mask outside DLC rejected");
  c = enabledConfig();
  c.ch[0].onVal[1] = c.ch[0].offVal[1];
  check(!burstFlashSanitizePure(c), "ON == OFF rejected");

  c = enabledConfig();
  c.ch[0].onVal[1] = 0xFF;
  check(burstFlashSanitizePure(c) && c.ch[0].onVal[1] == 0x30,
        "ON value masked down to owned bits");
  c = enabledConfig();
  c.version = 99;
  check(!burstFlashSanitizePure(c), "unknown config version rejected");
}

static void testStalkComposition() {
  BurstFlashConfigPure c = enabledConfig();
  const uint8_t stock[4] = {0x00, 0xA7, 0x3C, 0x81};  // high nibble A, counter 7
  uint8_t on[8] = {}, off[8] = {};
  check(burstFlashComposeStalkPure(stock, 8, c.ch[0], true, on), "front ON composed");
  check(burstFlashComposeStalkPure(stock, 8, c.ch[0], false, off), "front OFF composed");

  check((on[1] & 0x30) == 0x20, "ON writes the learned high-beam value");
  check((off[1] & 0x30) == 0x00, "OFF clears the high-beam bits");
  check((on[1] & 0xC0) == (stock[1] & 0xC0), "wash/wipe bits untouched");
  check((off[1] & 0xC0) == (stock[1] & 0xC0), "wash/wipe bits untouched (OFF)");
  check(on[2] == stock[2] && on[3] == stock[3], "turn/angle bytes untouched");
  check((on[1] & 0x0F) == 8 && (off[1] & 0x0F) == 8, "rolling counter written");

  // The decisive property: changing high-beam bits must be covered by the
  // existing P4-CAN CRC without any checksum change.
  check(on[0] == leftStalkChecksumPure(on, 8), "ON CRC matches P4-CAN helper");
  check(off[0] == leftStalkChecksumPure(off, 8), "OFF CRC matches P4-CAN helper");
  check(on[0] != off[0], "ON and OFF frames carry different CRCs");

  uint8_t bad[8] = {};
  check(!burstFlashComposeStalkPure(nullptr, 8, c.ch[0], true, bad),
        "never compose a front frame without live stock");
  std::printf("  stalk ON=%s OFF=%s\n", hex(on, 4).c_str(), hex(off, 4).c_str());
}

static void testGenericComposition() {
  BurstFlashConfigPure c = enabledConfig();
  uint8_t on[8] = {}, off[8] = {};
  check(burstFlashComposeGenericPure(c.ch[1], true, on), "rear ON composed");
  check(burstFlashComposeGenericPure(c.ch[1], false, off), "rear OFF composed");
  check((on[0] & 0x1F) == 0, "MUX page 0 selected");
  check(on[7] == 0x0E && off[7] == 0x00, "brake + tail L/R asserted on ON only");
  check(on[6] == 0 && on[5] == 0, "unowned bytes stay zero");
  std::printf("  rear  ON=%s OFF=%s\n", hex(on, 8).c_str(), hex(off, 8).c_str());
}

static void testAdmission() {
  BurstFlashConfigPure c = enabledConfig();
  BurstFlashRuntimePure rt = {};
  const bool stationaryArgs[3] = {true, true, false};
  (void)stationaryArgs;

  check(burstFlashStartPure(c, 0, rt, 1000, true, true,
                            BURST_FLASH_STATIONARY_RAW_PURE, false, true, true) ==
        BURST_FLASH_REJECT_NONE_PURE, "stationary + valid starts");
  check(rt.phase == BURST_FLASH_PHASE_ON_PURE && rt.pendingSend, "session armed");
  check(burstFlashStartPure(c, 0, rt, 1001, true, true,
                            BURST_FLASH_STATIONARY_RAW_PURE, false, true, true) ==
        BURST_FLASH_REJECT_ALREADY_RUNNING_PURE, "duplicate start rejected");

  auto startWith = [&](BurstFlashConfigPure cfg, bool sv, bool sf, uint16_t raw,
                       bool phys, bool can, bool stock) {
    BurstFlashRuntimePure r = {};
    return burstFlashStartPure(cfg, 1, r, 1000, sv, sf, raw, phys, can, stock);
  };
  BurstFlashConfigPure off = c; off.masterEnabled = false;
  check(startWith(off, true, true, 500, false, true, true) ==
        BURST_FLASH_REJECT_MASTER_DISABLED_PURE, "master off rejects");
  BurstFlashConfigPure noCh = c; noCh.ch[1].enabled = false;
  check(startWith(noCh, true, true, 500, false, true, true) ==
        BURST_FLASH_REJECT_CHANNEL_DISABLED_PURE, "channel off rejects");
  BurstFlashConfigPure noTpl = c; noTpl.ch[1].templateValid = false;
  check(startWith(noTpl, true, true, 500, false, true, true) ==
        BURST_FLASH_REJECT_TEMPLATE_INVALID_PURE, "unvalidated template rejects");
  check(startWith(c, true, true, 900, false, true, true) ==
        BURST_FLASH_REJECT_NOT_STATIONARY_PURE, "moving vehicle rejects");
  check(startWith(c, false, false, 0, false, true, true) ==
        BURST_FLASH_REJECT_SPEED_UNKNOWN_PURE, "unknown speed rejects");
  check(startWith(c, true, true, 500, true, true, true) ==
        BURST_FLASH_REJECT_PHYSICAL_INPUT_PURE, "physical input rejects");
  check(startWith(c, true, true, 500, false, false, true) ==
        BURST_FLASH_REJECT_CAN_NOT_READY_PURE, "CAN not ready rejects");

  BurstFlashRuntimePure frt = {};
  check(burstFlashStartPure(c, 0, frt, 1000, true, true, 500, false, true, false) ==
        BURST_FLASH_REJECT_NO_STOCK_TEMPLATE_PURE, "front needs live 0x249 stock");

  BurstFlashConfigPure bench = c; bench.allowUnknownSpeed = true;
  check(startWith(bench, false, false, 0, false, true, true) ==
        BURST_FLASH_REJECT_NONE_PURE, "bench mode allows unknown speed");
}

static void testSequence() {
  BurstFlashConfigPure c = enabledConfig();
  c.ch[1].count = 3; c.ch[1].onMs = 100; c.ch[1].offMs = 100;
  BurstFlashRuntimePure rt = {};
  check(burstFlashStartPure(c, 1, rt, 0, true, true, 500, false, true, true) ==
        BURST_FLASH_REJECT_NONE_PURE, "rear session started");

  int onFrames = 0, offFrames = 0, finishedCount = 0;
  for (uint32_t t = 0; t <= 2000 && finishedCount == 0; t += 20) {
    BurstFlashStepResultPure r = burstFlashStepPure(c, 1, rt, t, false, true,
                                                    nullptr, 0);
    if (!r.send) continue;
    check(r.canId == 0x3F4 && r.dlc == 8, "rear frame id/dlc");
    if (r.on) {
      onFrames++;
      check(r.data[7] == 0x0E, "ON frame asserts rear lamps");
    } else {
      offFrames++;
      check(r.data[7] == 0x00, "OFF frame clears rear lamps");
    }
    if (r.finished) finishedCount++;
    burstFlashRecordTxPure(rt, true, t, r.data, r.dlc);
  }
  check(onFrames == 3, "exactly three ON frames for count=3");
  check(offFrames == 4, "three OFF pauses plus one final OFF");
  check(finishedCount == 1, "one finished event");
  check(rt.phase == BURST_FLASH_PHASE_IDLE_PURE, "runtime returns to IDLE");
  check(rt.txOk == 7, "all seven frames recorded");
  std::printf("  sequence ON=%d OFF=%d txOk=%u\n", onFrames, offFrames, rt.txOk);
}

static void testAbortPaths() {
  BurstFlashConfigPure c = enabledConfig();
  const uint8_t stock[4] = {0, 0xA0, 0x30, 0x11};

  // Physical input mid-session must emit one final OFF frame.
  BurstFlashRuntimePure rt = {};
  burstFlashStartPure(c, 0, rt, 0, true, true, 500, false, true, true);
  BurstFlashStepResultPure r = burstFlashStepPure(c, 0, rt, 10, true, true, stock, 0);
  check(r.send && r.finished && !r.on, "physical abort sends a closing OFF frame");
  check(r.data[0] == leftStalkChecksumPure(r.data, 0), "abort frame CRC valid");
  check(rt.phase == BURST_FLASH_PHASE_IDLE_PURE, "abort resets state");

  // Watchdog.
  rt = {};
  burstFlashStartPure(c, 1, rt, 0, true, true, 500, false, true, true);
  r = burstFlashStepPure(c, 1, rt, BURST_FLASH_WATCHDOG_MS_PURE + 1,
                         false, true, nullptr, 0);
  check(r.send && r.finished && !r.on, "watchdog sends a closing OFF frame");

  // Total duration cap.
  c.ch[1].count = 12; c.ch[1].onMs = 1000; c.ch[1].offMs = 1000;
  rt = {};
  burstFlashStartPure(c, 1, rt, 0, true, true, 500, false, true, true);
  r = burstFlashStepPure(c, 1, rt, BURST_FLASH_MAX_TOTAL_MS_PURE + 1,
                         false, true, nullptr, 0);
  check(r.send && r.finished && !r.on, "total-duration cap stops the session");

  // No stock template for the front channel means no fabricated frame.
  c = enabledConfig();
  rt = {};
  burstFlashStartPure(c, 0, rt, 0, true, true, 500, false, true, true);
  r = burstFlashStepPure(c, 0, rt, 10, false, true, nullptr, 0);
  check(!r.send, "front never emits without a cached stock frame");
}

static void testLearning() {
  BurstFlashLearnPure l = {};
  burstFlashLearnStartPure(l, BURST_FLASH_FRONT_PURE, 0, 0, 1000);
  check(l.state == BURST_FLASH_LEARN_ARMED_PURE, "front learning armed");
  const uint8_t idle[4] = {0, 0x00, 0x00, 0x00};
  const uint8_t flash[4] = {0, 0x20, 0x00, 0x00};
  check(!burstFlashLearnObservePure(l, 0x249, idle, 4, 1010), "idle frame does not capture");
  check(burstFlashLearnObservePure(l, 0x249, flash, 4, 1100), "real high-beam frame captures");
  check(l.capturedHighBeam == 2, "captured high-beam value 2");
  check(l.state == BURST_FLASH_LEARN_CAPTURED_PURE, "learning state captured");
  BurstFlashConfigPure c = burstFlashDefaultConfigPure();
  check(burstFlashLearnApplyPure(l, c), "front learning applied");
  check(c.ch[0].onVal[1] == 0x20 && c.ch[0].offVal[1] == 0x00, "learned ON/OFF values stored");
  check(c.ch[0].templateValid && c.ch[0].learned, "template now validated");
  check(burstFlashSanitizePure(c), "learned config is valid");

  // Rear learning succeeds only when a lamp status frame actually changes.
  l = {};
  burstFlashLearnStartPure(l, BURST_FLASH_REAR_PURE, 0, 0, 2000);
  check(l.state == BURST_FLASH_LEARN_ARMED_PURE, "rear learning armed");
  const uint8_t st0[8] = {0x00, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t st1[8] = {0x01, 0, 0, 0, 0, 0, 0, 0};
  const uint8_t show[8] = {0x00, 0, 0, 0, 0, 0, 0, 0x0E};
  check(!burstFlashLearnObservePure(l, 0x3F4, show, 8, 2010), "probe frame alone not enough");
  check(!burstFlashLearnObservePure(l, 0x3E2, st0, 8, 2020), "unchanged lamp status not enough");
  check(burstFlashLearnObservePure(l, 0x3E2, st1, 8, 2030), "lamp state change captures");
  check(l.have3F4 && l.peakBrake == 1, "rear evidence recorded");
  c = burstFlashDefaultConfigPure();
  check(burstFlashLearnApplyPure(l, c) && c.ch[1].templateValid,
        "rear learning validates the template");

  // Timeout.
  l = {};
  burstFlashLearnStartPure(l, BURST_FLASH_FRONT_PURE, 0, 0, 0);
  check(burstFlashLearnTimedOutPure(l, BURST_FLASH_LEARN_TIMEOUT_MS_PURE + 1),
        "learning times out");
  check(l.state == BURST_FLASH_LEARN_TIMEOUT_PURE, "timeout state set");
  c = burstFlashDefaultConfigPure();
  check(!burstFlashLearnApplyPure(l, c), "timed-out learning cannot validate a template");
}

static void testPhysicalObserve() {
  uint32_t until = 0;
  const uint8_t stalkIdle[4] = {0, 0x00, 0x00, 0x00};
  const uint8_t stalkActive[4] = {0, 0x20, 0x01, 0x00};
  const uint8_t brakeIdle[8] = {0x00};
  const uint8_t brakeActive[8] = {0x01};
  check(burstFlashPhysicalObservePure(0x249, stalkIdle, 4, 1000, until) == 0,
        "idle stalk does not claim physical input");
  until = burstFlashPhysicalObservePure(0x249, stalkActive, 4, 1000, until);
  check(until == 1000 + BURST_FLASH_PHYSICAL_HOLD_MS_PURE, "active stalk holds the gate");
  check(burstFlashPhysicalObservePure(0x3E2, brakeIdle, 8, 1100, until) == until,
        "idle brake status leaves the deadline");
  const uint32_t later = burstFlashPhysicalObservePure(0x3E2, brakeActive, 8, 1200, until);
  check(later == 1200 + BURST_FLASH_PHYSICAL_HOLD_MS_PURE, "brake extends the deadline");
  check(burstFlashPhysicalObservePure(0x123, stalkActive, 4, 1300, later) == later,
        "unrelated CAN id ignored");
}

int main() {
  std::printf("burst_flash_pure host tests\n");
  testDefaults();
  testSanitize();
  testStalkComposition();
  testGenericComposition();
  testAdmission();
  testSequence();
  testAbortPaths();
  testLearning();
  testPhysicalObserve();
  std::printf("%d checks, %d failures\n", g_checks, g_fails);
  return g_fails == 0 ? 0 : 1;
}
