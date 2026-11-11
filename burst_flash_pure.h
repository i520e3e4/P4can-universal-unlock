#pragma once
#include <stdint.h>
#include <string.h>
#include "auto_blinker_pure.h"

// BURST FLASH (front high-beam / rear brake-tail) - PURE LOGIC
//
// This header has no Arduino/ESP-IDF dependency and no global state. It is the
// testable core of the optional burst-flash feature.
//
// Design boundary (important):
//   The Model Y 2026.26.6.5 DBC gives authoritative *bit layout* for the
//   lighting frames, but every ValueDescription is null. We therefore never
//   hardcode "value X turns the lamp on" as a fact. Frame composition uses a
//   parameterised mask/value pair that is either learned in-car or explicitly
//   supplied by the operator. Nothing is transmitted until a channel is
//   enabled AND its template has been validated.
//
//   Front: 0x249 SCCM_leftStalk, SCCM_highBeamStalkStatus = byte1 bit4-5.
//          leftStalkChecksumPure() from auto_blinker_pure.h already takes
//          (frame[1] & 0xF0) as CRC input, so writing those two bits is
//          automatically covered by the existing CRC. No checksum change.
//   Rear:  0x3F4 LS_lightShow, MUX=0, byte7 bit1 brake + bit2/3 tail L/R.
//          CycleTime=0 (event triggered), no checksum, no periodic owner.
//
//   Status frames 0x3E2 / 0x3E3 (brakeLightStatus) are READ ONLY and are used
//   to verify that an injected rear request actually reached the lamp control.

static constexpr uint8_t  BURST_FLASH_CONFIG_VERSION_PURE   = 1u;
static constexpr uint8_t  BURST_FLASH_CHANNEL_COUNT_PURE    = 2u;
static constexpr uint8_t  BURST_FLASH_FRONT_PURE            = 0u;
static constexpr uint8_t  BURST_FLASH_REAR_PURE             = 1u;

static constexpr uint16_t BURST_FLASH_FRONT_ID_PURE         = 0x249u;
static constexpr uint16_t BURST_FLASH_REAR_ID_PURE          = 0x3F4u;
static constexpr uint16_t BURST_FLASH_VCLEFT_STATUS_ID_PURE = 0x3E2u;
static constexpr uint16_t BURST_FLASH_VCRIGHT_STATUS_ID_PURE= 0x3E3u;
static constexpr uint16_t BURST_FLASH_UI_CTRL3_ID_PURE      = 0x294u;

static constexpr uint8_t  BURST_FLASH_HIGHBEAM_MASK_PURE    = 0x30u;  // 0x249 byte1 bit4-5
static constexpr uint8_t  BURST_FLASH_REAR_MASK_PURE        = 0x0Eu;  // 0x3F4 byte7 bit1-3
static constexpr uint8_t  BURST_FLASH_MUX_MASK_PURE         = 0x1Fu;  // 0x3F4 byte0 bit0-4

// Hard safety limits. Sanitising clamps into these ranges; a config that needs
// more than this is deliberately rejected rather than silently accepted.
static constexpr uint8_t  BURST_FLASH_MIN_COUNT_PURE        = 1u;
static constexpr uint8_t  BURST_FLASH_MAX_COUNT_PURE        = 12u;
static constexpr uint16_t BURST_FLASH_MIN_PHASE_MS_PURE     = 40u;
static constexpr uint16_t BURST_FLASH_MAX_PHASE_MS_PURE     = 1000u;
static constexpr uint32_t BURST_FLASH_MAX_TOTAL_MS_PURE     = 15000u;
static constexpr uint32_t BURST_FLASH_WATCHDOG_MS_PURE      = 20000u;
static constexpr uint32_t BURST_FLASH_LEARN_TIMEOUT_MS_PURE = 30000u;
static constexpr uint32_t BURST_FLASH_PHYSICAL_HOLD_MS_PURE = 1500u;
static constexpr uint32_t BURST_FLASH_SPEED_FRESH_MS_PURE   = 2000u;
static constexpr uint16_t BURST_FLASH_STATIONARY_RAW_PURE   = 500u;  // nagCtx speed raw for 0 km/h
static constexpr uint8_t  BURST_FLASH_MAX_DLC_PURE          = 8u;

enum BurstFlashPhasePure : uint8_t {
  BURST_FLASH_PHASE_IDLE_PURE = 0,
  BURST_FLASH_PHASE_ON_PURE = 1,
  BURST_FLASH_PHASE_OFF_PURE = 2
};

enum BurstFlashRejectPure : uint8_t {
  BURST_FLASH_REJECT_NONE_PURE = 0,
  BURST_FLASH_REJECT_MASTER_DISABLED_PURE = 1,
  BURST_FLASH_REJECT_CHANNEL_DISABLED_PURE = 2,
  BURST_FLASH_REJECT_TEMPLATE_INVALID_PURE = 3,
  BURST_FLASH_REJECT_BAD_PARAMS_PURE = 4,
  BURST_FLASH_REJECT_NOT_STATIONARY_PURE = 5,
  BURST_FLASH_REJECT_SPEED_UNKNOWN_PURE = 6,
  BURST_FLASH_REJECT_PHYSICAL_INPUT_PURE = 7,
  BURST_FLASH_REJECT_ALREADY_RUNNING_PURE = 8,
  BURST_FLASH_REJECT_CAN_NOT_READY_PURE = 9,
  BURST_FLASH_REJECT_NO_STOCK_TEMPLATE_PURE = 10
};

enum BurstFlashLearnStatePure : uint8_t {
  BURST_FLASH_LEARN_IDLE_PURE = 0,
  BURST_FLASH_LEARN_ARMED_PURE = 1,
  BURST_FLASH_LEARN_CAPTURED_PURE = 2,
  BURST_FLASH_LEARN_TIMEOUT_PURE = 3
};

struct BurstFlashChannelConfigPure {
  bool enabled;
  bool templateValid;      // learning or explicit operator confirmation only
  bool learned;            // true when values came from an in-car capture
  uint16_t canId;
  uint8_t dlc;
  uint8_t count;
  uint16_t onMs;
  uint16_t offMs;
  uint8_t tmpl[8];         // base frame (unused for 0x249: live stock is used)
  uint8_t mask[8];         // bits this feature owns
  uint8_t onVal[8];        // masked value while ON
  uint8_t offVal[8];       // masked value while OFF
};

struct BurstFlashConfigPure {
  uint8_t version;
  bool masterEnabled;
  bool requireStationary;
  bool allowUnknownSpeed;
  BurstFlashChannelConfigPure ch[BURST_FLASH_CHANNEL_COUNT_PURE];
};

struct BurstFlashRuntimePure {
  uint8_t phase;
  uint8_t remaining;
  bool pendingSend;
  bool aborted;
  uint32_t phaseStartMs;
  uint32_t sessionStartMs;
  uint32_t lastProgressMs;
  uint8_t lastReject;
  uint32_t txOk;
  uint32_t txFail;
  uint32_t blocked;
  uint32_t sessions;
  uint32_t lastTxMs;
  uint8_t lastFrame[8];
  uint8_t lastDlc;
};

struct BurstFlashStepResultPure {
  bool send;
  bool finished;
  bool on;
  uint16_t canId;
  uint8_t dlc;
  uint8_t data[8];
  uint8_t reason;
};

struct BurstFlashLearnPure {
  uint8_t channel;
  uint8_t state;
  uint32_t armMs;
  uint32_t lastChangeMs;
  uint8_t baselineHighBeam;   // 0x249 (byte1 >> 4) & 0x03 at arm time
  uint8_t capturedHighBeam;
  uint8_t baselineBrake;      // 0x3E2 byte0 & 0x03 at arm time
  uint8_t peakBrake;
  uint16_t obs249;
  uint16_t obs3F4;
  uint16_t obsStatus;
  uint16_t obs294;
  uint8_t learned3F4[8];
  bool have3F4;
};

// ── Defaults ────────────────────────────────────────────────────────────────
// All disabled. The bit layout below is evidence-backed; the ON *values* are
// placeholders that are never used until templateValid becomes true.
static inline BurstFlashConfigPure burstFlashDefaultConfigPure() {
  BurstFlashConfigPure c = {};
  c.version = BURST_FLASH_CONFIG_VERSION_PURE;
  c.masterEnabled = false;
  c.requireStationary = true;
  c.allowUnknownSpeed = false;

  BurstFlashChannelConfigPure &f = c.ch[BURST_FLASH_FRONT_PURE];
  f.enabled = false; f.templateValid = false; f.learned = false;
  f.canId = BURST_FLASH_FRONT_ID_PURE; f.dlc = 4u;
  f.count = 3u; f.onMs = 120u; f.offMs = 120u;
  f.mask[1] = BURST_FLASH_HIGHBEAM_MASK_PURE;
  f.onVal[1] = 0x20u;   // PLACEHOLDER - not a verified DBC value
  f.offVal[1] = 0x00u;

  BurstFlashChannelConfigPure &r = c.ch[BURST_FLASH_REAR_PURE];
  r.enabled = false; r.templateValid = false; r.learned = false;
  r.canId = BURST_FLASH_REAR_ID_PURE; r.dlc = 8u;
  r.count = 3u; r.onMs = 160u; r.offMs = 160u;
  r.mask[0] = BURST_FLASH_MUX_MASK_PURE;   // MUX = 0
  r.onVal[0] = 0x00u; r.offVal[0] = 0x00u;
  r.mask[7] = BURST_FLASH_REAR_MASK_PURE;  // brake + tail L + tail R
  r.onVal[7] = BURST_FLASH_REAR_MASK_PURE; // PLACEHOLDER - request bits = 1
  r.offVal[7] = 0x00u;
  return c;
}

static inline uint16_t burstFlashClampU16Pure(uint16_t v, uint16_t lo, uint16_t hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

// Reject out-of-range parameters rather than silently clamping an unsafe
// template. Timing/count are clamped; identity fields are validated.
static inline bool burstFlashSanitizePure(BurstFlashConfigPure &c) {
  if (c.version != BURST_FLASH_CONFIG_VERSION_PURE) return false;
  for (uint8_t i = 0; i < BURST_FLASH_CHANNEL_COUNT_PURE; i++) {
    BurstFlashChannelConfigPure &ch = c.ch[i];
    if (ch.dlc < 1u || ch.dlc > BURST_FLASH_MAX_DLC_PURE) return false;
    if (ch.canId > 0x7FFu) return false;
    if (ch.count < BURST_FLASH_MIN_COUNT_PURE) ch.count = BURST_FLASH_MIN_COUNT_PURE;
    if (ch.count > BURST_FLASH_MAX_COUNT_PURE) ch.count = BURST_FLASH_MAX_COUNT_PURE;
    ch.onMs = burstFlashClampU16Pure(ch.onMs, BURST_FLASH_MIN_PHASE_MS_PURE,
                                     BURST_FLASH_MAX_PHASE_MS_PURE);
    ch.offMs = burstFlashClampU16Pure(ch.offMs, BURST_FLASH_MIN_PHASE_MS_PURE,
                                      BURST_FLASH_MAX_PHASE_MS_PURE);
    // A channel must never claim bits outside its own DLC.
    for (uint8_t b = ch.dlc; b < BURST_FLASH_MAX_DLC_PURE; b++) {
      if (ch.mask[b] || ch.onVal[b] || ch.offVal[b] || ch.tmpl[b]) return false;
    }
    // Values outside the mask are meaningless and make review harder.
    for (uint8_t b = 0; b < ch.dlc; b++) {
      ch.onVal[b] &= ch.mask[b];
      ch.offVal[b] &= ch.mask[b];
    }
    // ON and OFF must actually differ, otherwise the session is a no-op that
    // still occupies the bus.
    bool differs = false;
    for (uint8_t b = 0; b < ch.dlc; b++)
      if ((ch.onVal[b] ^ ch.offVal[b]) & ch.mask[b]) differs = true;
    if (ch.templateValid && !differs) return false;
  }
  return true;
}

// ── Frame composition ───────────────────────────────────────────────────────
// For 0x249 the stock frame observed on the bus is the only legal base:
// byte1 high nibble (high-beam + wash/wipe), byte2 high nibble and byte3 must
// remain exactly as the vehicle produced them. Only the owned mask, the rolling
// counter and the CRC are rewritten.
static inline bool burstFlashComposeStalkPure(const uint8_t stock[4], uint8_t counter,
                                              const BurstFlashChannelConfigPure &ch,
                                              bool on, uint8_t out[8]) {
  if (!stock || !out || ch.dlc < 4u) return false;
  out[0] = 0u; out[1] = stock[1]; out[2] = stock[2]; out[3] = stock[3];
  for (uint8_t b = 4u; b < BURST_FLASH_MAX_DLC_PURE; b++) out[b] = 0u;
  const uint8_t value = on ? ch.onVal[1] : ch.offVal[1];
  out[1] = (uint8_t)((out[1] & (uint8_t)~ch.mask[1]) | (value & ch.mask[1]));
  out[1] = (uint8_t)((out[1] & 0xF0u) | (counter & 0x0Fu));
  out[0] = leftStalkChecksumPure(out, counter);
  return true;
}

static inline bool burstFlashComposeGenericPure(const BurstFlashChannelConfigPure &ch,
                                                bool on, uint8_t out[8]) {
  if (!out) return false;
  for (uint8_t b = 0; b < BURST_FLASH_MAX_DLC_PURE; b++) {
    const uint8_t base = b < ch.dlc ? ch.tmpl[b] : 0u;
    const uint8_t mask = b < ch.dlc ? ch.mask[b] : 0u;
    const uint8_t value = on ? (b < ch.dlc ? ch.onVal[b] : 0u)
                             : (b < ch.dlc ? ch.offVal[b] : 0u);
    out[b] = (uint8_t)((base & (uint8_t)~mask) | (value & mask));
  }
  return true;
}

static inline bool burstFlashComposePure(const BurstFlashChannelConfigPure &ch, bool on,
                                         const uint8_t *stock, uint8_t counter,
                                         uint8_t out[8]) {
  if (ch.canId == BURST_FLASH_FRONT_ID_PURE)
    return burstFlashComposeStalkPure(stock, counter, ch, on, out);
  return burstFlashComposeGenericPure(ch, on, out);
}

// ── Admission gates ─────────────────────────────────────────────────────────
static inline bool burstFlashStationaryPure(bool speedValid, bool speedFresh,
                                            uint16_t speedRaw) {
  return speedValid && speedFresh && speedRaw == BURST_FLASH_STATIONARY_RAW_PURE;
}

static inline BurstFlashRejectPure burstFlashStartPure(
    const BurstFlashConfigPure &cfg, uint8_t channel,
    BurstFlashRuntimePure &rt, uint32_t nowMs,
    bool speedValid, bool speedFresh, uint16_t speedRaw,
    bool physicalInputActive, bool canReady, bool stockTemplateAvailable) {
  if (channel >= BURST_FLASH_CHANNEL_COUNT_PURE)
    return BURST_FLASH_REJECT_BAD_PARAMS_PURE;
  if (rt.phase != BURST_FLASH_PHASE_IDLE_PURE)
    return BURST_FLASH_REJECT_ALREADY_RUNNING_PURE;
  if (!cfg.masterEnabled) return BURST_FLASH_REJECT_MASTER_DISABLED_PURE;
  const BurstFlashChannelConfigPure &ch = cfg.ch[channel];
  if (!ch.enabled) return BURST_FLASH_REJECT_CHANNEL_DISABLED_PURE;
  if (!ch.templateValid) return BURST_FLASH_REJECT_TEMPLATE_INVALID_PURE;
  if (!canReady) return BURST_FLASH_REJECT_CAN_NOT_READY_PURE;
  if (cfg.requireStationary) {
    if (!speedValid || !speedFresh) {
      if (!cfg.allowUnknownSpeed) return BURST_FLASH_REJECT_SPEED_UNKNOWN_PURE;
    } else if (!burstFlashStationaryPure(speedValid, speedFresh, speedRaw)) {
      return BURST_FLASH_REJECT_NOT_STATIONARY_PURE;
    }
  }
  if (physicalInputActive) return BURST_FLASH_REJECT_PHYSICAL_INPUT_PURE;
  if (ch.canId == BURST_FLASH_FRONT_ID_PURE && !stockTemplateAvailable)
    return BURST_FLASH_REJECT_NO_STOCK_TEMPLATE_PURE;

  rt.phase = BURST_FLASH_PHASE_ON_PURE;
  rt.remaining = ch.count;
  rt.pendingSend = true;
  rt.aborted = false;
  rt.phaseStartMs = nowMs;
  rt.sessionStartMs = nowMs;
  rt.lastProgressMs = nowMs;
  rt.lastReject = BURST_FLASH_REJECT_NONE_PURE;
  rt.sessions++;
  return BURST_FLASH_REJECT_NONE_PURE;
}

static inline void burstFlashResetRuntimePure(BurstFlashRuntimePure &rt) {
  const uint32_t txOk = rt.txOk, txFail = rt.txFail, blocked = rt.blocked;
  const uint32_t sessions = rt.sessions, lastTxMs = rt.lastTxMs;
  const uint8_t lastReject = rt.lastReject;
  memset(&rt, 0, sizeof(rt));
  rt.phase = BURST_FLASH_PHASE_IDLE_PURE;
  rt.txOk = txOk; rt.txFail = txFail; rt.blocked = blocked;
  rt.sessions = sessions; rt.lastTxMs = lastTxMs; rt.lastReject = lastReject;
}

// Returns a frame to transmit when a phase boundary is crossed. The caller is
// responsible for the actual bus write and for feeding the result back with
// burstFlashRecordTxPure().
static inline BurstFlashStepResultPure burstFlashStepPure(
    const BurstFlashConfigPure &cfg, uint8_t channel, BurstFlashRuntimePure &rt,
    uint32_t nowMs, bool physicalInputActive, bool canReady,
    const uint8_t *stock, uint8_t counter) {
  BurstFlashStepResultPure r = {};
  r.canId = cfg.ch[channel].canId;
  r.dlc = cfg.ch[channel].dlc;
  if (rt.phase == BURST_FLASH_PHASE_IDLE_PURE) { r.finished = true; return r; }

  const uint32_t elapsed = nowMs - rt.sessionStartMs;
  const bool watchdog = elapsed > BURST_FLASH_WATCHDOG_MS_PURE ||
                        (nowMs - rt.lastProgressMs) > BURST_FLASH_WATCHDOG_MS_PURE;
  const bool totalCap = elapsed > BURST_FLASH_MAX_TOTAL_MS_PURE;
  const bool abort = physicalInputActive || watchdog || totalCap || !canReady ||
                     !cfg.masterEnabled || !cfg.ch[channel].enabled;

  if (abort) {
    // Always leave the lamps in the OFF state, even on an abort path.
    r.send = true; r.finished = true; r.on = false;
    r.reason = physicalInputActive ? BURST_FLASH_REJECT_PHYSICAL_INPUT_PURE
                                   : BURST_FLASH_REJECT_BAD_PARAMS_PURE;
    if (!burstFlashComposePure(cfg.ch[channel], false, stock, counter, r.data))
      r.send = false;  // never emit a fabricated 0x249 frame without live stock
    rt.aborted = true;
    rt.lastReject = r.reason;
    burstFlashResetRuntimePure(rt);
    rt.blocked++;
    return r;
  }

  if (rt.pendingSend) {
    rt.pendingSend = false;
    rt.lastProgressMs = nowMs;
    r.send = true; r.on = (rt.phase == BURST_FLASH_PHASE_ON_PURE);
    if (!burstFlashComposePure(cfg.ch[channel], r.on, stock, counter, r.data))
      r.send = false;
    return r;
  }

  const uint32_t phaseMs = rt.phase == BURST_FLASH_PHASE_ON_PURE
                               ? cfg.ch[channel].onMs : cfg.ch[channel].offMs;
  if ((uint32_t)(nowMs - rt.phaseStartMs) < phaseMs) return r;

  rt.phaseStartMs = nowMs;
  rt.lastProgressMs = nowMs;
  if (rt.phase == BURST_FLASH_PHASE_ON_PURE) {
    rt.phase = BURST_FLASH_PHASE_OFF_PURE;
    r.send = true; r.on = false;
  } else {
    if (rt.remaining > 0u) rt.remaining--;
    if (rt.remaining == 0u) {
      r.send = true; r.on = false; r.finished = true;
      burstFlashResetRuntimePure(rt);
    } else {
      rt.phase = BURST_FLASH_PHASE_ON_PURE;
      r.send = true; r.on = true;
    }
  }
  if (!burstFlashComposePure(cfg.ch[channel], r.on, stock, counter, r.data))
    r.send = false;
  return r;
}

static inline void burstFlashRecordTxPure(BurstFlashRuntimePure &rt, bool ok,
                                          uint32_t nowMs,
                                          const uint8_t *frame, uint8_t dlc) {
  if (ok) rt.txOk++; else rt.txFail++;
  rt.lastTxMs = nowMs;
  if (frame && dlc <= BURST_FLASH_MAX_DLC_PURE) {
    memcpy(rt.lastFrame, frame, dlc);
    rt.lastDlc = dlc;
  }
}

// ── Learning mode ───────────────────────────────────────────────────────────
// "Learn" watches real frames while the operator performs the physical action
// once. It is the only mechanism that can legitimately mark a template valid
// without guessing a DBC value that the source data does not contain.
static inline void burstFlashLearnStartPure(BurstFlashLearnPure &l, uint8_t channel,
                                            uint8_t baselineHighBeam,
                                            uint8_t baselineBrake, uint32_t nowMs) {
  memset(&l, 0, sizeof(l));
  l.channel = channel < BURST_FLASH_CHANNEL_COUNT_PURE ? channel : 0u;
  l.state = BURST_FLASH_LEARN_ARMED_PURE;
  l.armMs = nowMs;
  l.lastChangeMs = nowMs;
  l.baselineHighBeam = (uint8_t)(baselineHighBeam & 0x03u);
  l.baselineBrake = (uint8_t)(baselineBrake & 0x03u);
}

// Returns true exactly once, at the moment a usable value is captured.
static inline bool burstFlashLearnObservePure(BurstFlashLearnPure &l, uint16_t canId,
                                              const uint8_t *data, uint8_t dlc,
                                              uint32_t nowMs) {
  if (l.state != BURST_FLASH_LEARN_ARMED_PURE || !data) return false;
  if ((uint32_t)(nowMs - l.armMs) > BURST_FLASH_LEARN_TIMEOUT_MS_PURE) {
    l.state = BURST_FLASH_LEARN_TIMEOUT_PURE;
    return false;
  }
  bool captured = false;
  if (canId == BURST_FLASH_FRONT_ID_PURE && dlc >= 2u) {
    if (l.obs249 < 0xFFFFu) l.obs249++;
    const uint8_t hb = (uint8_t)((data[1] >> 4) & 0x03u);
    if (hb != 0u && hb != l.baselineHighBeam) {
      l.capturedHighBeam = hb;
      l.lastChangeMs = nowMs;
      if (l.channel == BURST_FLASH_FRONT_PURE) captured = true;
    }
  } else if (canId == BURST_FLASH_REAR_ID_PURE && dlc >= 8u) {
    if (l.obs3F4 < 0xFFFFu) l.obs3F4++;
    memcpy(l.learned3F4, data, 8u);
    l.have3F4 = true;
    l.lastChangeMs = nowMs;
  } else if ((canId == BURST_FLASH_VCLEFT_STATUS_ID_PURE ||
              canId == BURST_FLASH_VCRIGHT_STATUS_ID_PURE) && dlc >= 1u) {
    if (l.obsStatus < 0xFFFFu) l.obsStatus++;
    const uint8_t brake = (uint8_t)(data[0] & 0x03u);
    if (brake > l.peakBrake) l.peakBrake = brake;
    if (brake != 0u && brake != l.baselineBrake) {
      l.lastChangeMs = nowMs;
      // A real lamp state change proves the injection path reached the lamp
      // controller. This is the rear channel's learning success condition.
      if (l.channel == BURST_FLASH_REAR_PURE) captured = true;
    }
  } else if (canId == BURST_FLASH_UI_CTRL3_ID_PURE && dlc >= 2u) {
    if (l.obs294 < 0xFFFFu) l.obs294++;
  }
  if (captured) l.state = BURST_FLASH_LEARN_CAPTURED_PURE;
  return captured;
}

static inline bool burstFlashLearnTimedOutPure(BurstFlashLearnPure &l, uint32_t nowMs) {
  if (l.state == BURST_FLASH_LEARN_ARMED_PURE &&
      (uint32_t)(nowMs - l.armMs) > BURST_FLASH_LEARN_TIMEOUT_MS_PURE) {
    l.state = BURST_FLASH_LEARN_TIMEOUT_PURE;
    return true;
  }
  return false;
}

// Fold a successful capture into the channel config. Front stores the observed
// high-beam value; rear keeps the evidence-backed bit layout and marks the
// template valid only if a lamp state change was actually seen.
static inline bool burstFlashLearnApplyPure(const BurstFlashLearnPure &l,
                                            BurstFlashConfigPure &cfg) {
  if (l.state != BURST_FLASH_LEARN_CAPTURED_PURE ||
      l.channel >= BURST_FLASH_CHANNEL_COUNT_PURE) return false;
  BurstFlashChannelConfigPure &ch = cfg.ch[l.channel];
  if (l.channel == BURST_FLASH_FRONT_PURE) {
    if (l.capturedHighBeam == 0u || l.capturedHighBeam == l.baselineHighBeam)
      return false;
    ch.onVal[1] = (uint8_t)((l.capturedHighBeam << 4) & BURST_FLASH_HIGHBEAM_MASK_PURE);
    ch.offVal[1] = (uint8_t)((l.baselineHighBeam << 4) & BURST_FLASH_HIGHBEAM_MASK_PURE);
    ch.mask[1] = BURST_FLASH_HIGHBEAM_MASK_PURE;
  } else {
    if (l.peakBrake == l.baselineBrake) return false;
    if (l.have3F4) memcpy(ch.tmpl, l.learned3F4, 8u);
    ch.mask[0] = BURST_FLASH_MUX_MASK_PURE;
    ch.onVal[7] = BURST_FLASH_REAR_MASK_PURE;
    ch.offVal[7] = 0x00u;
    ch.mask[7] = BURST_FLASH_REAR_MASK_PURE;
  }
  ch.learned = true;
  ch.templateValid = true;
  return true;
}

// Physical input always wins. Called for every observed RX frame; the returned
// deadline is extended whenever a real driver input is visible on the bus.
static inline uint32_t burstFlashPhysicalObservePure(uint16_t canId,
                                                     const uint8_t *data,
                                                     uint8_t dlc, uint32_t nowMs,
                                                     uint32_t activeUntilMs) {
  if (!data || dlc == 0u) return activeUntilMs;
  bool physical = false;
  if (canId == BURST_FLASH_FRONT_ID_PURE && dlc >= 3u) {
    physical = (data[1] & BURST_FLASH_HIGHBEAM_MASK_PURE) != 0u ||
               (data[2] & 0x0Fu) != 0u;
  } else if ((canId == BURST_FLASH_VCLEFT_STATUS_ID_PURE ||
              canId == BURST_FLASH_VCRIGHT_STATUS_ID_PURE) && dlc >= 1u) {
    physical = (data[0] & 0x03u) != 0u;
  }
  if (!physical) return activeUntilMs;
  const uint32_t deadline = nowMs + BURST_FLASH_PHYSICAL_HOLD_MS_PURE;
  return deadline > activeUntilMs ? deadline : activeUntilMs;
}
