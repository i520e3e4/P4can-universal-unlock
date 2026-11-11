#pragma once
#include <stdlib.h>
#include "burst_flash_pure.h"
#include "nvs_checked.h"
#include "json_writer_arduino.h"

// BURST FLASH - RUNTIME
//
// Included after can_core.h / vehicle_logic.h and before web_api.h /
// can_runtime.h, because it consumes their static helpers inside the same
// translation unit (the whole firmware is one .ino TU).
//
// Safety contract:
//   * masterEnabled defaults to false; channel.enabled and templateValid also
//     default to false. Three independent conditions must all be true before
//     any frame can leave the device.
//   * 0x249 is never fabricated. A live stock frame must have been observed,
//     and stalkFrameSnapshot() is called ONLY when a frame is actually about
//     to be transmitted (it advances the shared rolling counter).
//   * Speed gate: stationary by default, with an explicit opt-out for bench
//     work (allowUnknownSpeed / requireStationary).
//   * Physical stalk / brake input always aborts a running session and forces
//     one final OFF frame.
//   * Hard caps on count, phase duration, total duration and watchdog.

static constexpr char BURST_FLASH_NVS_NAMESPACE[] = "burstflash";
static constexpr char BURST_FLASH_NVS_KEY[] = "cfg";
static constexpr uint32_t BURST_FLASH_LEARN_PROBE_ON_MS = 160u;
static constexpr uint32_t BURST_FLASH_LEARN_PROBE_DELAY_MS = 100u;

static BurstFlashConfigPure burstCfg = burstFlashDefaultConfigPure();
static BurstFlashRuntimePure burstRt[BURST_FLASH_CHANNEL_COUNT_PURE];
static BurstFlashLearnPure burstLearn;
static portMUX_TYPE burstMux = portMUX_INITIALIZER_UNLOCKED;
static bool burstLoaded = false;

static volatile uint32_t burstPhysicalUntilMs = 0;
static volatile uint32_t burstSelfTxGuardUntilMs = 0;
static bool burstHaveStock249 = false;
static uint8_t burstStock249[4] = {};
static uint8_t burstBaselineBrake = 0;
static uint32_t burstLast249Ms = 0;
static uint32_t burstLast3E2Ms = 0;
static uint32_t burstLearnProbeStartMs = 0;
static bool burstLearnProbeOnSent = false;
static bool burstLearnProbeOffSent = false;
static uint8_t burstLastRejectFront = 0;
static uint8_t burstLastRejectRear = 0;

// ── Persistence ─────────────────────────────────────────────────────────────
static bool burstFlashSaveConfigLocked(const BurstFlashConfigPure &c) {
  return nvsCheckedWriteBlob(BURST_FLASH_NVS_NAMESPACE, BURST_FLASH_NVS_KEY,
                             &c, sizeof(c));
}

static void burstFlashInit() {
  BurstFlashConfigPure c = burstFlashDefaultConfigPure();
  bool found = false;
  if (nvsCheckedReadBlob(BURST_FLASH_NVS_NAMESPACE, BURST_FLASH_NVS_KEY,
                         &c, sizeof(c), found) && found &&
      burstFlashSanitizePure(c)) {
    portENTER_CRITICAL(&burstMux);
    burstCfg = c;
    portEXIT_CRITICAL(&burstMux);
  } else {
    // A corrupt or stale record must never leave an enabled channel behind.
    c = burstFlashDefaultConfigPure();
    portENTER_CRITICAL(&burstMux);
    burstCfg = c;
    portEXIT_CRITICAL(&burstMux);
    burstFlashSaveConfigLocked(c);
  }
  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++)
    burstFlashResetRuntimePure(burstRt[i]);
  memset(&burstLearn, 0, sizeof(burstLearn));
  burstLoaded = true;
}

static inline void burstFlashEnsureLoaded() {
  if (!burstLoaded) burstFlashInit();
}

// ── Small helpers ───────────────────────────────────────────────────────────
static void burstFlashHexPure(const uint8_t *data, uint8_t len, char *out,
                              size_t outLen) {
  static const char kHex[] = "0123456789ABCDEF";
  if (!out || outLen < (size_t)len * 2u + 1u) return;
  for (uint8_t i = 0; i < len; i++) {
    out[i * 2u] = kHex[(data[i] >> 4) & 0x0Fu];
    out[i * 2u + 1u] = kHex[data[i] & 0x0Fu];
  }
  out[len * 2u] = '\0';
}

static bool burstFlashParseHex(const String &s, uint8_t *out, uint8_t len) {
  if (!out) return false;
  String t = s;
  t.replace("0x", ""); t.replace(" ", ""); t.replace(":", "");
  t.toUpperCase();
  if ((int)len * 2 != t.length()) return false;
  for (uint8_t i = 0; i < len; i++) {
    int hi = -1, lo = -1;
    const char a = t.charAt((unsigned int)i * 2u);
    const char b = t.charAt((unsigned int)i * 2u + 1u);
    hi = (a >= '0' && a <= '9') ? a - '0' : (a >= 'A' && a <= 'F' ? a - 'A' + 10 : -1);
    lo = (b >= '0' && b <= '9') ? b - '0' : (b >= 'A' && b <= 'F' ? b - 'A' + 10 : -1);
    if (hi < 0 || lo < 0) return false;
    out[i] = (uint8_t)((hi << 4) | lo);
  }
  return true;
}

static const char *burstFlashRejectName(uint8_t reason) {
  switch (reason) {
    case BURST_FLASH_REJECT_MASTER_DISABLED_PURE: return "MASTER_DISABLED";
    case BURST_FLASH_REJECT_CHANNEL_DISABLED_PURE: return "CHANNEL_DISABLED";
    case BURST_FLASH_REJECT_TEMPLATE_INVALID_PURE: return "TEMPLATE_INVALID";
    case BURST_FLASH_REJECT_BAD_PARAMS_PURE: return "BAD_PARAMS";
    case BURST_FLASH_REJECT_NOT_STATIONARY_PURE: return "NOT_STATIONARY";
    case BURST_FLASH_REJECT_SPEED_UNKNOWN_PURE: return "SPEED_UNKNOWN";
    case BURST_FLASH_REJECT_PHYSICAL_INPUT_PURE: return "PHYSICAL_INPUT";
    case BURST_FLASH_REJECT_ALREADY_RUNNING_PURE: return "ALREADY_RUNNING";
    case BURST_FLASH_REJECT_CAN_NOT_READY_PURE: return "CAN_NOT_READY";
    case BURST_FLASH_REJECT_NO_STOCK_TEMPLATE_PURE: return "NO_STOCK_TEMPLATE";
    default: return "NONE";
  }
}

static const char *burstFlashLearnName(uint8_t state) {
  switch (state) {
    case BURST_FLASH_LEARN_ARMED_PURE: return "ARMED";
    case BURST_FLASH_LEARN_CAPTURED_PURE: return "CAPTURED";
    case BURST_FLASH_LEARN_TIMEOUT_PURE: return "TIMEOUT";
    default: return "IDLE";
  }
}

// ── Bus transmit ────────────────────────────────────────────────────────────
// Both lighting command frames live on DBC bus VEH == P4-CAN BODY. On a
// standard Model 3/Y that is CAN A (native A); on topologies where CAN A is
// not Body, fall back to the TWAI/CAN B path.
static bool burstFlashTransmit(uint8_t channel, uint16_t canId, uint8_t dlc,
                               const uint8_t baseFrame[8], bool on) {
  uint8_t data[8] = {};
  memcpy(data, baseFrame, 8u);

  BurstFlashChannelConfigPure ch;
  portENTER_CRITICAL(&burstMux);
  ch = burstCfg.ch[channel];
  portEXIT_CRITICAL(&burstMux);

  if (canId == BURST_FLASH_FRONT_ID_PURE) {
    // This is the ONLY place stalkFrameSnapshot() is called. It advances the
    // shared rolling counter, so it must not be polled from the service tick.
    uint8_t counter = 0;
    uint8_t stock[4] = {};
    if (!stalkFrameSnapshot(counter, stock)) return false;
    if (!burstFlashComposeStalkPure(stock, counter, ch, on, data)) return false;
  }

  const uint32_t txEpoch = canTxEpochSnapshot();
  esp_err_t err = ESP_FAIL;
  bool ok = false;
  if (activeCanAIsBody()) {
    struct CanFrame out = {};
    out.can_id = canId;
    out.can_dlc = dlc;
    memcpy(out.data, data, 8u);
    ok = canTxNativeASendTagged(&out, txEpoch, CAN_TX_TRACE_SOURCE_DEFAULT,
                                err, nullptr);
    ok = ok && err == ESP_OK;
  } else {
    twai_message_t out = {};
    out.identifier = canId;
    out.data_length_code = dlc;
    out.flags = 0;
    memcpy(out.data, data, 8u);
    err = canTxTwaiTransmitTagged(&out, txEpoch, CAN_TX_TRACE_SOURCE_DEFAULT);
    ok = err == ESP_OK;
  }
  if (ok) burstSelfTxGuardUntilMs = (uint32_t)millis() + 80u;
  return ok;
}

// ── Observation hook (called from both CAN RX tasks) ────────────────────────
void burstFlashObserveRx(uint16_t canId, const uint8_t *data, uint8_t dlc,
                         uint8_t bus, uint32_t nowMs) {
  (void)bus;
  burstFlashEnsureLoaded();
  if (!data || dlc == 0u) return;

  // Ignore our own echo for a short window so an injected high-beam frame is
  // never mistaken for a real driver input.
  const bool selfEcho = nowMs < burstSelfTxGuardUntilMs;
  if (!selfEcho) {
    const uint32_t until = burstFlashPhysicalObservePure(
        canId, data, dlc, nowMs, burstPhysicalUntilMs);
    burstPhysicalUntilMs = until;
  }

  if (canId == BURST_FLASH_FRONT_ID_PURE && dlc >= 4u) {
    portENTER_CRITICAL(&burstMux);
    memcpy(burstStock249, data, 4u);
    burstHaveStock249 = true;
    burstLast249Ms = nowMs;
    portEXIT_CRITICAL(&burstMux);
  } else if (canId == BURST_FLASH_VCLEFT_STATUS_ID_PURE && dlc >= 1u) {
    portENTER_CRITICAL(&burstMux);
    burstLast3E2Ms = nowMs;
    portEXIT_CRITICAL(&burstMux);
  }

  portENTER_CRITICAL(&burstMux);
  bool needSave = false;
  if (burstLearn.state == BURST_FLASH_LEARN_ARMED_PURE) {
    const bool captured = burstFlashLearnObservePure(burstLearn, canId, data,
                                                     dlc, nowMs);
    if (captured) {
      BurstFlashConfigPure next = burstCfg;
      if (burstFlashLearnApplyPure(burstLearn, next) &&
          burstFlashSanitizePure(next)) {
        burstCfg = next;
        needSave = true;
      } else {
        burstLearn.state = BURST_FLASH_LEARN_TIMEOUT_PURE;
      }
    }
  }
  portEXIT_CRITICAL(&burstMux);
  // NVS writes touch flash and must never run inside a critical section.
  if (needSave) {
    portENTER_CRITICAL(&burstMux);
    const BurstFlashConfigPure snapshot = burstCfg;
    portEXIT_CRITICAL(&burstMux);
    (void)burstFlashSaveConfigLocked(snapshot);
  }
}

// ── Trigger / stop ──────────────────────────────────────────────────────────
static bool burstFlashSpeedSnapshot(uint32_t nowMs, bool &valid, bool &fresh,
                                    uint16_t &raw) {
  portENTER_CRITICAL(&nagCtxMux);
  valid = nagCtx.vehicleSpeedValid;
  raw = nagCtx.vehicleSpeedRaw;
  const uint32_t lastMs = nagCtx.lastVehicleSpeedMs;
  portEXIT_CRITICAL(&nagCtxMux);
  fresh = lastMs != 0u &&
          (uint32_t)(nowMs - lastMs) <= BURST_FLASH_SPEED_FRESH_MS_PURE;
  return true;
}

bool burstFlashTriggerFromButton(uint8_t channel);  // forward for s3xy_ble.h

static uint8_t burstFlashTrigger(uint8_t channel) {
  burstFlashEnsureLoaded();
  if (channel >= BURST_FLASH_CHANNEL_COUNT_PURE)
    return BURST_FLASH_REJECT_BAD_PARAMS_PURE;
  const uint32_t nowMs = (uint32_t)millis();
  bool speedValid = false, speedFresh = false;
  uint16_t speedRaw = 0;
  burstFlashSpeedSnapshot(nowMs, speedValid, speedFresh, speedRaw);
  const bool physical = nowMs < burstPhysicalUntilMs;
  const bool canReady = activeCanAIsBody() ? nativeAReady : twaiReady;
  bool haveStock = false;
  portENTER_CRITICAL(&burstMux);
  haveStock = burstHaveStock249;
  portEXIT_CRITICAL(&burstMux);

  uint8_t reject;
  portENTER_CRITICAL(&burstMux);
  reject = (uint8_t)burstFlashStartPure(burstCfg, channel, burstRt[channel],
                                        nowMs, speedValid, speedFresh, speedRaw,
                                        physical, canReady, haveStock);
  portEXIT_CRITICAL(&burstMux);
  if (channel == BURST_FLASH_FRONT_PURE) burstLastRejectFront = reject;
  else burstLastRejectRear = reject;
  return reject;
}

bool burstFlashTriggerFromButton(uint8_t channel) {
  return burstFlashTrigger(channel) == BURST_FLASH_REJECT_NONE_PURE;
}

static void burstFlashStop() {
  bool wasActive[BURST_FLASH_CHANNEL_COUNT_PURE] = {false, false};
  portENTER_CRITICAL(&burstMux);
  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++) {
    wasActive[i] = burstRt[i].phase != BURST_FLASH_PHASE_IDLE_PURE;
    if (wasActive[i]) {
      burstRt[i].pendingSend = false;
      burstRt[i].aborted = true;
      burstRt[i].lastReject = BURST_FLASH_REJECT_BAD_PARAMS_PURE;
      burstFlashResetRuntimePure(burstRt[i]);
    }
  }
  portEXIT_CRITICAL(&burstMux);
  // Only a channel that was actually driving lamps needs a closing OFF frame.
  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++) {
    if (!wasActive[i]) continue;
    BurstFlashChannelConfigPure ch;
    portENTER_CRITICAL(&burstMux); ch = burstCfg.ch[i]; portEXIT_CRITICAL(&burstMux);
    uint8_t off[8] = {};
    (void)burstFlashComposeGenericPure(ch, false, off);
    (void)burstFlashTransmit(i, ch.canId, ch.dlc, off, false);
  }
}

// ── Learning mode ───────────────────────────────────────────────────────────
static uint8_t burstFlashLearnStart(uint8_t channel) {
  burstFlashEnsureLoaded();
  if (channel >= BURST_FLASH_CHANNEL_COUNT_PURE)
    return BURST_FLASH_REJECT_BAD_PARAMS_PURE;
  const uint32_t nowMs = (uint32_t)millis();
  bool speedValid = false, speedFresh = false;
  uint16_t speedRaw = 0;
  burstFlashSpeedSnapshot(nowMs, speedValid, speedFresh, speedRaw);

  portENTER_CRITICAL(&burstMux);
  if (!burstCfg.masterEnabled) {
    portEXIT_CRITICAL(&burstMux);
    return BURST_FLASH_REJECT_MASTER_DISABLED_PURE;
  }
  if (burstCfg.requireStationary &&
      !burstFlashStationaryPure(speedValid, speedFresh, speedRaw)) {
    portEXIT_CRITICAL(&burstMux);
    return speedValid && speedFresh ? BURST_FLASH_REJECT_NOT_STATIONARY_PURE
                                   : BURST_FLASH_REJECT_SPEED_UNKNOWN_PURE;
  }
  const uint8_t baselineHighBeam =
      burstHaveStock249 ? (uint8_t)((burstStock249[1] >> 4) & 0x03u) : 0u;
  burstBaselineBrake = 0u;  // refreshed by the next 0x3E2/0x3E3 observation
  burstFlashLearnStartPure(burstLearn, channel, baselineHighBeam,
                           burstBaselineBrake, nowMs);
  burstLearnProbeStartMs = nowMs;
  burstLearnProbeOnSent = false;
  burstLearnProbeOffSent = false;
  portEXIT_CRITICAL(&burstMux);
  return BURST_FLASH_REJECT_NONE_PURE;
}

static void burstFlashLearnService(uint32_t nowMs) {
  portENTER_CRITICAL(&burstMux);
  const uint8_t state = burstLearn.state;
  const uint8_t channel = burstLearn.channel;
  const uint32_t startMs = burstLearnProbeStartMs;
  bool onSent = burstLearnProbeOnSent;
  bool offSent = burstLearnProbeOffSent;
  const bool timedOut = burstFlashLearnTimedOutPure(burstLearn, nowMs);
  portEXIT_CRITICAL(&burstMux);

  if (state != BURST_FLASH_LEARN_ARMED_PURE || channel != BURST_FLASH_REAR_PURE)
    return;
  if (timedOut) return;

  // A single, explicit ON/OFF probe pair. It is gated by masterEnabled +
  // stationary above and never repeats itself.
  const uint32_t elapsed = nowMs - startMs;
  if (!onSent && elapsed >= BURST_FLASH_LEARN_PROBE_DELAY_MS) {
    BurstFlashChannelConfigPure ch;
    portENTER_CRITICAL(&burstMux); ch = burstCfg.ch[channel]; portEXIT_CRITICAL(&burstMux);
    uint8_t on[8] = {};
    (void)burstFlashComposeGenericPure(ch, true, on);
    (void)burstFlashTransmit(channel, ch.canId, ch.dlc, on, true);
    portENTER_CRITICAL(&burstMux);
    burstLearnProbeOnSent = true;
    portEXIT_CRITICAL(&burstMux);
  } else if (onSent && !offSent &&
             elapsed >= BURST_FLASH_LEARN_PROBE_DELAY_MS +
                        BURST_FLASH_LEARN_PROBE_ON_MS) {
    BurstFlashChannelConfigPure ch;
    portENTER_CRITICAL(&burstMux); ch = burstCfg.ch[channel]; portEXIT_CRITICAL(&burstMux);
    uint8_t off[8] = {};
    (void)burstFlashComposeGenericPure(ch, false, off);
    (void)burstFlashTransmit(channel, ch.canId, ch.dlc, off, false);
    portENTER_CRITICAL(&burstMux);
    burstLearnProbeOffSent = true;
    portEXIT_CRITICAL(&burstMux);
  }
}

// ── Service tick (called from canSupervisorTask, ~20 ms cadence) ─────────────
void burstFlashTick(uint32_t nowMs) {
  burstFlashEnsureLoaded();
  bool speedValid = false, speedFresh = false;
  uint16_t speedRaw = 0;
  burstFlashSpeedSnapshot(nowMs, speedValid, speedFresh, speedRaw);
  const bool physical = nowMs < burstPhysicalUntilMs &&
                        nowMs >= burstSelfTxGuardUntilMs;
  const bool canReady = activeCanAIsBody() ? nativeAReady : twaiReady;

  burstFlashLearnService(nowMs);

  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++) {
    BurstFlashStepResultPure r = {};
    portENTER_CRITICAL(&burstMux);
    const uint8_t *stock = burstHaveStock249 ? burstStock249 : nullptr;
    r = burstFlashStepPure(burstCfg, i, burstRt[i], nowMs, physical, canReady,
                           stock, 0u);
    portEXIT_CRITICAL(&burstMux);

    if (!r.send || r.dlc == 0u || r.dlc > 8u) continue;
    const bool ok = burstFlashTransmit(i, r.canId, r.dlc, r.data, r.on);
    portENTER_CRITICAL(&burstMux);
    burstFlashRecordTxPure(burstRt[i], ok, nowMs, r.data, r.dlc);
    portEXIT_CRITICAL(&burstMux);
  }
}

// ── JSON ────────────────────────────────────────────────────────────────────
static String burstFlashStatusJson() {
  burstFlashEnsureLoaded();
  BurstFlashConfigPure c;
  BurstFlashRuntimePure rt[BURST_FLASH_CHANNEL_COUNT_PURE];
  BurstFlashLearnPure learn;
  bool haveStock;
  portENTER_CRITICAL(&burstMux);
  c = burstCfg;
  rt[0] = burstRt[0]; rt[1] = burstRt[1];
  learn = burstLearn;
  haveStock = burstHaveStock249;
  portEXIT_CRITICAL(&burstMux);

  bool speedValid = false, speedFresh = false;
  uint16_t speedRaw = 0;
  burstFlashSpeedSnapshot((uint32_t)millis(), speedValid, speedFresh, speedRaw);

  String s; s.reserve(1800);
  JsonWriterArduino jw(s);
  jw.boolean("masterEnabled", c.masterEnabled);
  jw.boolean("requireStationary", c.requireStationary);
  jw.boolean("allowUnknownSpeed", c.allowUnknownSpeed);
  jw.boolean("stationary", burstFlashStationaryPure(speedValid, speedFresh, speedRaw));
  jw.boolean("speedValid", speedValid);
  jw.boolean("speedFresh", speedFresh);
  jw.boolean("physicalInput", (uint32_t)millis() < burstPhysicalUntilMs);
  jw.boolean("canReady", activeCanAIsBody() ? nativeAReady : twaiReady);
  jw.boolean("stock249", haveStock);
  jw.string("learnState", burstFlashLearnName(learn.state));
  jw.u32("learnChannel", learn.channel);
  jw.u32("learnObs249", learn.obs249);
  jw.u32("learnObs3F4", learn.obs3F4);
  jw.u32("learnObsStatus", learn.obsStatus);
  jw.u32("learnPeakBrake", learn.peakBrake);
  jw.u32("learnCapturedHighBeam", learn.capturedHighBeam);
  jw.u32("lastRejectFront", burstLastRejectFront);
  jw.u32("lastRejectRear", burstLastRejectRear);
  jw.string("lastRejectFrontName", burstFlashRejectName(burstLastRejectFront));
  jw.string("lastRejectRearName", burstFlashRejectName(burstLastRejectRear));

  // JsonWriterArduino has no "append bare array element" helper, so the
  // channels array is assembled as a finished JSON value and injected with
  // raw(). Passing a null key would dereference a null char* in prefix().
  String channels;
  channels.reserve(900);
  channels += '[';
  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++) {
    const BurstFlashChannelConfigPure &ch = c.ch[i];
    char hex[20];
    if (i) channels += ',';
    String item; item.reserve(420);
    JsonWriterArduino ci(item);
    ci.u32("index", i);
    ci.boolean("enabled", ch.enabled);
    ci.boolean("templateValid", ch.templateValid);
    ci.boolean("learned", ch.learned);
    ci.u32("canId", ch.canId);
    ci.u32("dlc", ch.dlc);
    ci.u32("count", ch.count);
    ci.u32("onMs", ch.onMs);
    ci.u32("offMs", ch.offMs);
    burstFlashHexPure(ch.tmpl, 8, hex, sizeof(hex)); ci.string("tmpl", hex);
    burstFlashHexPure(ch.mask, 8, hex, sizeof(hex)); ci.string("mask", hex);
    burstFlashHexPure(ch.onVal, 8, hex, sizeof(hex)); ci.string("onVal", hex);
    burstFlashHexPure(ch.offVal, 8, hex, sizeof(hex)); ci.string("offVal", hex);
    ci.u32("phase", rt[i].phase);
    ci.u32("remaining", rt[i].remaining);
    ci.u32("txOk", rt[i].txOk);
    ci.u32("txFail", rt[i].txFail);
    ci.u32("blocked", rt[i].blocked);
    ci.u32("sessions", rt[i].sessions);
    ci.finish();
    channels += item;
  }
  channels += ']';
  jw.raw("channels", channels);
  jw.finish();
  return s;
}

// ── HTTP handlers (registered in web_api.h webTask) ─────────────────────────
static void burstFlashHttpConfig() {
  server.send(200, "application/json", burstFlashStatusJson());
}

static void burstFlashHttpStatus() {
  server.send(200, "application/json", burstFlashStatusJson());
}

static void burstFlashHttpMaster() {
  burstFlashEnsureLoaded();
  if (!server.hasArg("enabled")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing enabled\"}");
    return;
  }
  const bool enabled = server.arg("enabled").toInt() != 0;
  BurstFlashConfigPure c;
  portENTER_CRITICAL(&burstMux); c = burstCfg; portEXIT_CRITICAL(&burstMux);
  c.masterEnabled = enabled;
  if (!burstFlashSanitizePure(c)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid config\"}");
    return;
  }
  portENTER_CRITICAL(&burstMux); burstCfg = c; portEXIT_CRITICAL(&burstMux);
  const bool saved = burstFlashSaveConfigLocked(c);
  if (!enabled) burstFlashStop();
  server.send(200, "application/json",
              saved ? burstFlashStatusJson()
                    : "{\"ok\":false,\"error\":\"nvs write failed\"}");
}

static void burstFlashHttpChannel() {
  burstFlashEnsureLoaded();
  if (!server.hasArg("ch")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"missing ch\"}");
    return;
  }
  const int ci = server.arg("ch").toInt();
  if (ci < 0 || ci >= (int)BURST_FLASH_CHANNEL_COUNT_PURE) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad ch\"}");
    return;
  }
  BurstFlashConfigPure c;
  portENTER_CRITICAL(&burstMux); c = burstCfg; portEXIT_CRITICAL(&burstMux);
  BurstFlashChannelConfigPure &ch = c.ch[(uint8_t)ci];
  if (server.hasArg("enabled")) ch.enabled = server.arg("enabled").toInt() != 0;
  if (server.hasArg("count")) ch.count = (uint8_t)server.arg("count").toInt();
  if (server.hasArg("onMs")) ch.onMs = (uint16_t)server.arg("onMs").toInt();
  if (server.hasArg("offMs")) ch.offMs = (uint16_t)server.arg("offMs").toInt();
  if (server.hasArg("requireStationary"))
    c.requireStationary = server.arg("requireStationary").toInt() != 0;
  if (server.hasArg("allowUnknownSpeed"))
    c.allowUnknownSpeed = server.arg("allowUnknownSpeed").toInt() != 0;
  if (!burstFlashSanitizePure(c)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid config\"}");
    return;
  }
  portENTER_CRITICAL(&burstMux); burstCfg = c; portEXIT_CRITICAL(&burstMux);
  const bool saved = burstFlashSaveConfigLocked(c);
  server.send(200, "application/json",
              saved ? burstFlashStatusJson()
                    : "{\"ok\":false,\"error\":\"nvs write failed\"}");
}

// Template editing is an explicit, confirm-gated operator action. The DBC has
// no value semantics, so this path exists only for people who captured their
// own frames and know exactly what they are writing.
static void burstFlashHttpTemplate() {
  burstFlashEnsureLoaded();
  if (!server.hasArg("ch") || !server.hasArg("confirm") ||
      server.arg("confirm") != "1") {
    server.send(400, "application/json",
                "{\"ok\":false,\"error\":\"explicit confirm=1 required\"}");
    return;
  }
  const int ci = server.arg("ch").toInt();
  if (ci < 0 || ci >= (int)BURST_FLASH_CHANNEL_COUNT_PURE) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"bad ch\"}");
    return;
  }
  BurstFlashConfigPure c;
  portENTER_CRITICAL(&burstMux); c = burstCfg; portEXIT_CRITICAL(&burstMux);
  BurstFlashChannelConfigPure &ch = c.ch[(uint8_t)ci];
  if (server.hasArg("id")) ch.canId = (uint16_t)strtol(server.arg("id").c_str(), nullptr, 0);
  if (server.hasArg("dlc")) ch.dlc = (uint8_t)server.arg("dlc").toInt();
  uint8_t tmp[8];
  if (server.hasArg("tmpl") && burstFlashParseHex(server.arg("tmpl"), tmp, 8)) memcpy(ch.tmpl, tmp, 8);
  if (server.hasArg("mask") && burstFlashParseHex(server.arg("mask"), tmp, 8)) memcpy(ch.mask, tmp, 8);
  if (server.hasArg("onVal") && burstFlashParseHex(server.arg("onVal"), tmp, 8)) memcpy(ch.onVal, tmp, 8);
  if (server.hasArg("offVal") && burstFlashParseHex(server.arg("offVal"), tmp, 8)) memcpy(ch.offVal, tmp, 8);
  ch.learned = false;
  ch.templateValid = true;
  if (!burstFlashSanitizePure(c)) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"invalid template\"}");
    return;
  }
  portENTER_CRITICAL(&burstMux); burstCfg = c; portEXIT_CRITICAL(&burstMux);
  const bool saved = burstFlashSaveConfigLocked(c);
  server.send(200, "application/json",
              saved ? burstFlashStatusJson()
                    : "{\"ok\":false,\"error\":\"nvs write failed\"}");
}

static void burstFlashHttpTrigger() {
  const int ci = server.hasArg("ch") ? server.arg("ch").toInt() : 0;
  const uint8_t reject = burstFlashTrigger((uint8_t)ci);
  if (reject != BURST_FLASH_REJECT_NONE_PURE) {
    String body = "{\"ok\":false,\"error\":\"";
    body += burstFlashRejectName(reject);
    body += "\"}";
    server.send(409, "application/json", body);
    return;
  }
  server.send(200, "application/json", burstFlashStatusJson());
}

static void burstFlashHttpStop() {
  burstFlashStop();
  server.send(200, "application/json", burstFlashStatusJson());
}

static void burstFlashHttpLearn() {
  const int ci = server.hasArg("ch") ? server.arg("ch").toInt() : 0;
  const uint8_t reject = burstFlashLearnStart((uint8_t)ci);
  if (reject != BURST_FLASH_REJECT_NONE_PURE) {
    String body = "{\"ok\":false,\"error\":\"";
    body += burstFlashRejectName(reject);
    body += "\"}";
    server.send(409, "application/json", body);
    return;
  }
  server.send(200, "application/json", burstFlashStatusJson());
}

static void burstFlashRegisterRoutes() {
  server.on("/api/burst-flash/config", HTTP_GET, burstFlashHttpConfig);
  server.on("/api/burst-flash/status", HTTP_GET, burstFlashHttpStatus);
  server.on("/api/burst-flash/master", HTTP_POST, burstFlashHttpMaster);
  server.on("/api/burst-flash/channel", HTTP_POST, burstFlashHttpChannel);
  server.on("/api/burst-flash/template", HTTP_POST, burstFlashHttpTemplate);
  server.on("/api/burst-flash/trigger", HTTP_POST, burstFlashHttpTrigger);
  server.on("/api/burst-flash/stop", HTTP_POST, burstFlashHttpStop);
  server.on("/api/burst-flash/learn", HTTP_POST, burstFlashHttpLearn);
}
