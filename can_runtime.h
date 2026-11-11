#pragma once

// CAN TASKS / RECOVERY SUPERVISOR
// Kept in the same translation unit to preserve proven runtime behavior.

// ═══════════════════════════════════════════════════════════════
// CAN TASKS
// ═══════════════════════════════════════════════════════════════

// Recovery invalidates only observations owned by the controller that reset.
// A new global epoch still cancels in-flight TX, while freshness for an
// unaffected, physically-live bus may cross the local recovery boundary.
static uint8_t canPhysicalFreshMaskNow(uint32_t now) {
  uint8_t mask = SUMMON_BUS_NONE;
  const uint32_t lastA = lastCanAFrameMs;
  const uint32_t lastB = lastCanBFrameMs;
  if (lastA != 0 && (uint32_t)(now - lastA) <= RECOVERY_BUS_FRESH_MS)
    mask = (uint8_t)(mask | SUMMON_BUS_A);
  if (lastB != 0 && (uint32_t)(now - lastB) <= RECOVERY_BUS_FRESH_MS)
    mask = (uint8_t)(mask | SUMMON_BUS_B);
  return mask;
}

static void invalidateCanTxStateInternal(uint8_t invalidatedBusMask) {
  const uint32_t now = (uint32_t)millis();
  const uint8_t physicalFreshMask = canPhysicalFreshMaskNow(now);
  const bool barrierLocked = canTxBarrierMutex &&
                             xSemaphoreTake(canTxBarrierMutex, portMAX_DELAY) == pdTRUE;
  if (barrierLocked) {
    const uint8_t preserved = canTxPreservedFreshMaskPure(
        canTxBarrierState.freshMask, invalidatedBusMask, physicalFreshMask);
    canTxBarrierInvalidatePreservePure(canTxBarrierState, preserved);
    __atomic_store_n(&canTxFreshMaskFast, preserved, __ATOMIC_RELEASE);
  }

  const SummonRoutePure route = activeSummonRoute();
  const SummonInvalidationPure inv =
      summonInvalidationPure(route, invalidatedBusMask);

  portENTER_CRITICAL(&stateMux);
  if (inv.invalidateDas || inv.invalidateTemplate) {
    r79ApGateSession = {};
    __atomic_add_fetch(&r79ApGateGeneration, 1u, __ATOMIC_ACQ_REL);
  }
  if (inv.invalidateDas) {
    gateAPActive = false;
    gateNOAActive = false;
    dasAutopilotStateValid = false;
    dasAutopilotState4 = 0xFF;
    dasAutoLaneChangeState = 0xFF;
    dasAutoLaneChangeStateValid = false;
    lastDASStatusMillis = 0;
  }
  if (inv.invalidateGear) {
    // Recovery clears fresh gear evidence. v3.6 R79 remains default-on until
    // a new valid D/R + manual state positively proves manual driving.
    gateParked = true;
    lastAca = false;
    acaValid = false;
    lastAcaMillis = 0;
    last280Millis = 0;
    gear118State = -1;
    gear186State = -1;
    gear118Raw = TESLA_GEAR_INVALID;
    gear186Raw = TESLA_GEAR_INVALID;
    gear118Ms = 0;
    gear186Ms = 0;
    summonGearSource = SUMMON_GEAR_NONE;
    summonGearObservedMs = 0;
  }
  if (inv.invalidateSpr) {
    sprSeen = false;
    sprValid = false;
    lastSprRaw = 0;
    lastSprMillis = 0;
  }
  if (inv.invalidateGear || inv.invalidateSpr) {
    gateSummoning = false;
  }
  if (inv.invalidateGear || inv.invalidateDas) {
    // Recovery of the signals that prove manual driving fails R79 back ON.
    r79ManualSuppression = {};
  }
  // Keep Summon monitor and the R79 manual latch coherent after source reset.
  refreshSummonDerivedStateLocked(now, barrierLocked);
  portEXIT_CRITICAL(&stateMux);

  if (inv.invalidateDas) {
    portENTER_CRITICAL(&nagTsl9Mux);
    nagTsl9State = {};
    portEXIT_CRITICAL(&nagTsl9Mux);
  }

  if (inv.invalidateTemplate) {
    portENTER_CRITICAL(&r79LabMux);
    r79LabStockValid = false;
    r79LabLastStockMs = 0;
    r79FixedQuietState = {};
    r79Mode2DelayedState = {};
    r79RetryPending = false;
    r79RetryIndex = 0;
    r79RetryOriginKind = R79LAB_TX_NONE;
    r79RetryDueMs = 0;
    r79RetryGeneration = 0u;
    r79LabLastTxValid = false;
    memset(r79LabLastStockRaw, 0, sizeof(r79LabLastStockRaw));
    portEXIT_CRITICAL(&r79LabMux);
  }
  // Keep an owned CENTER cleanup pending across controller recovery. The
  // scheduler will retry only after a fresh route-local MUX1 template arrives.
  tsl9InputRequestCancel(TSL9_INPUT_FAILURE_CAN_UNAVAILABLE_PURE, true);
  continuousApResetForEpoch(barrierLocked ? canTxBarrierState.epoch : 0u);
  driverWindowLabResetRuntimeUnderTxBarrier();
  summonHeartbeatOverrideResetUnderTxBarrier();

  // These transient feature requests are inexpensive to restart and are cleared
  // at either controller recovery so they cannot cross a changed CAN epoch.
  portENTER_CRITICAL(&blinkAMux);
  autoBlinkerNoaSessionResetPure(autoBlinkerNoaSessionState);
  autoBlinkerCancelPauseResetPure(autoBlinkerCancelPauseState);
  autoBlinkerClearPendingLocked();
  oneShotTurn = STALK_IDLE;
  oneShotDirect = false;
  oneShotSource = BLINKER_TX_SOURCE_NONE_PURE;
  oneShotUntil = 0;
  oneShotReleaseAt = 0;
  blinkerTxRequestState = {};
  activeTurn = STALK_IDLE;
  lastReqDir = 0;
  autoRequestLastSeenMs = 0;
  autoRetryCount = 0;
  visualBehaviorType = 0;
  visualDebugLastMs = 0;
  seen249 = false;
  realDlc = 0;
  cksumSelfTest = false;
  memset(realRaw249, 0, sizeof(realRaw249));
  portEXIT_CRITICAL(&blinkAMux);

  portENTER_CRITICAL(&ulcSnoozeMux);
  ulcSnoozePending = false;
  ulcSnoozeExpireMs = 0;
  portEXIT_CRITICAL(&ulcSnoozeMux);

  portENTER_CRITICAL(&s3xyMux);
  s3xyClearPendingActionsLocked(S3XY_PENDING_CAN_RECOVERY);
  portEXIT_CRITICAL(&s3xyMux);

  portENTER_CRITICAL(&pedalMapMux);
  pedalMapStockValid = false;
  pedalMapStockDataValid = false;
  pedalMapStockRaw = PEDAL_MAP_RAW_STOCK;
  pedalMapStockMs = 0;
  memset(pedalMapStockData, 0, sizeof(pedalMapStockData));
  // Keep the user-selected drive-session target across a transient CAN
  // recovery. The next fresh 0x334 re-establishes the template; confirmed
  // Park, explicit STOCK, or reboot owns session release.
  portEXIT_CRITICAL(&pedalMapMux);

  if ((invalidatedBusMask & SUMMON_BUS_A) != 0) {
    portENTER_CRITICAL(&autoLc293Mux);
    uiAutoLaneChangeStockAValid = false;
    uiAutoLaneChangeStockAMs = 0;
    portEXIT_CRITICAL(&autoLc293Mux);
  }
  if ((invalidatedBusMask & SUMMON_BUS_B) != 0) {
    portENTER_CRITICAL(&autoLc293Mux);
    uiAutoLaneChangeStockBValid = false;
    uiAutoLaneChangeStockBMs = 0;
    portEXIT_CRITICAL(&autoLc293Mux);
    portENTER_CRITICAL(&lab3f8Mux);
    uiDriverAssistLastRxMs = 0;
    uiUlcBlindSpotConfig = 0;
    uiAlcOffHighwayEnable = false;
    portEXIT_CRITICAL(&lab3f8Mux);
  }

  if (barrierLocked) xSemaphoreGive(canTxBarrierMutex);
}

static void invalidateNagPartySpeedState() {
  portENTER_CRITICAL(&nagCtxMux);
  nagCtx.vehicleSpeedRaw = 0;
  nagCtx.vehicleSpeedValid = false;
  nagCtx.lastVehicleSpeedMs = 0;
  portEXIT_CRITICAL(&nagCtxMux);
}

static void invalidateCanTxStateForCanARecovery() {
  invalidateNagPartySpeedState();
  invalidateCanTxStateInternal(SUMMON_BUS_A);
  if (nagTsl9Body39BSelected()) {
    portENTER_CRITICAL(&nagTsl9Mux);
    nagTsl9State = {};
    portEXIT_CRITICAL(&nagTsl9Mux);
  }
  nagExactEchoReset();
  nagHumanRuntimeReset(true);
}

static void invalidateCanTxStateForCanBRecovery() {
  const bool invalidatesNagGate = activeProfileNagGateDependsOnCanB();
  invalidateCanTxStateInternal(SUMMON_BUS_B);
  if (invalidatesNagGate) {
    nagExactEchoReset();
    nagHumanRuntimeReset(true);
  }
}

static void invalidateCanTxStateForFullRecovery() {
  p4CanExtraFresh = false;
  invalidateNagPartySpeedState();
  invalidateCanTxStateInternal(SUMMON_BUS_BOTH);
  nagExactEchoReset();
  nagHumanRuntimeReset(true);
}

static void recordTwaiBusOffSnapshot(const twai_status_info_t &st, uint32_t now) {
  CanTwaiRecoverySnapshot snap = {};
  snap.valid = true;
  snap.state = (uint8_t)st.state;
  snap.capturedMs = now;
  const uint32_t lastB = lastCanBFrameMs;
  snap.rxGapMs = lastB ? (uint32_t)(now - lastB) : 0;
  snap.msgsToTx = st.msgs_to_tx;
  snap.msgsToRx = st.msgs_to_rx;
  snap.txErrorCounter = st.tx_error_counter;
  snap.rxErrorCounter = st.rx_error_counter;
  snap.txFailedCount = st.tx_failed_count;
  snap.rxMissedCount = st.rx_missed_count;
  snap.rxOverrunCount = st.rx_overrun_count;
  snap.arbLostCount = st.arb_lost_count;
  snap.busErrorCount = st.bus_error_count;
  portENTER_CRITICAL(&canRecoveryMux);
  if (canTwaiBusOffAlertEvidence.valid) {
    snap.alertSeenMask = canTwaiBusOffAlertEvidence.seenMask;
    snap.alertBatchMask = canTwaiBusOffAlertEvidence.batchMask;
    snap.txFailedAlertAgeMs = canTwaiBusOffAlertEvidence.txFailedAgeMs;
    snap.errPassAlertAgeMs = canTwaiBusOffAlertEvidence.errPassAgeMs;
    snap.busErrorAlertAgeMs = canTwaiBusOffAlertEvidence.busErrorAgeMs;
  }
  canTwaiLastBusOffSnapshot = snap;
  portEXIT_CRITICAL(&canRecoveryMux);
}

static void canTwaiResetErrorAlertWindow() {
  portENTER_CRITICAL(&canRecoveryMux);
  canTwaiErrorAlertWindow = {};
  portEXIT_CRITICAL(&canRecoveryMux);
}

static void canTwaiObserveErrorAlerts(uint32_t errorMask, bool busOff,
                                      uint32_t now) {
  portENTER_CRITICAL(&canRecoveryMux);
  canTwaiErrorAlertWindow.seenMask |= errorMask;
  if (errorMask & TWAI_ALERT_TX_FAILED)
    canTwaiErrorAlertWindow.lastTxFailedMs = now;
  if (errorMask & TWAI_ALERT_ERR_PASS)
    canTwaiErrorAlertWindow.lastErrPassMs = now;
  if (errorMask & TWAI_ALERT_BUS_ERROR)
    canTwaiErrorAlertWindow.lastBusErrorMs = now;
  if (busOff) {
    canTwaiBusOffAlertEvidence.valid = true;
    canTwaiBusOffAlertEvidence.seenMask = canTwaiErrorAlertWindow.seenMask;
    canTwaiBusOffAlertEvidence.batchMask = errorMask;
    canTwaiBusOffAlertEvidence.txFailedAgeMs =
        (canTwaiErrorAlertWindow.seenMask & TWAI_ALERT_TX_FAILED)
            ? (uint32_t)(now - canTwaiErrorAlertWindow.lastTxFailedMs)
            : 999999UL;
    canTwaiBusOffAlertEvidence.errPassAgeMs =
        (canTwaiErrorAlertWindow.seenMask & TWAI_ALERT_ERR_PASS)
            ? (uint32_t)(now - canTwaiErrorAlertWindow.lastErrPassMs)
            : 999999UL;
    canTwaiBusOffAlertEvidence.busErrorAgeMs =
        (canTwaiErrorAlertWindow.seenMask & TWAI_ALERT_BUS_ERROR)
            ? (uint32_t)(now - canTwaiErrorAlertWindow.lastBusErrorMs)
            : 999999UL;
  }
  portEXIT_CRITICAL(&canRecoveryMux);
}

static void canBTraceFreezeBusOff(uint32_t now) {
  portENTER_CRITICAL(&canBTxTraceMux);
  const uint8_t count = canBTxTraceLiveCount;
  const uint8_t start = (uint8_t)((canBTxTraceLiveHead + CAN_B_TX_TRACE_CAPACITY - count) % CAN_B_TX_TRACE_CAPACITY);
  for (uint8_t i = 0; i < count; i++)
    canBTxTraceFrozen[i] = canBTxTraceLive[(uint8_t)((start + i) % CAN_B_TX_TRACE_CAPACITY)];
  for (uint8_t i = count; i < CAN_B_TX_TRACE_CAPACITY; i++) canBTxTraceFrozen[i] = {};
  canBTxTraceFrozenCount = count;
  canBTxTraceFrozenMs = now;
  canBTxTraceFrozenBusOffOrdinal = canTwaiBusOffCount + 1U;
  portEXIT_CRITICAL(&canBTxTraceMux);
}

static void canATraceFreezeBusOff(uint32_t now, uint8_t controllerState, uint8_t txFailConsecutive) {
  uint32_t overflowCount = 0, overflowLastMs = 0;
  uint8_t overflowFlags = 0;
  nativeARxOverflowSnapshot(overflowCount, overflowLastMs, overflowFlags);
  (void)overflowLastMs;
  (void)overflowFlags;
  const uint32_t ordinal = __atomic_add_fetch(&canANativeABusOffCount, 1U, __ATOMIC_RELAXED);
  const uint32_t lastA = lastCanAFrameMs;

  portENTER_CRITICAL(&canATxTraceMux);
  const uint8_t count = canATxTraceLiveCount;
  const uint8_t start = (uint8_t)((canATxTraceLiveHead + CAN_A_TX_TRACE_CAPACITY - count) % CAN_A_TX_TRACE_CAPACITY);
  for (uint8_t i = 0; i < count; i++)
    canATxTraceFrozen[i] = canATxTraceLive[(uint8_t)((start + i) % CAN_A_TX_TRACE_CAPACITY)];
  for (uint8_t i = count; i < CAN_A_TX_TRACE_CAPACITY; i++) canATxTraceFrozen[i] = {};
  canATxTraceFrozenCount = count;
  canATxTraceFrozenMs = now;
  canATxTraceFrozenBusOffOrdinal = ordinal;
  canALastBusOffSnapshot.valid = true;
  canALastBusOffSnapshot.capturedMs = now;
  canALastBusOffSnapshot.controllerState = controllerState;
  canALastBusOffSnapshot.txFailConsecutive = txFailConsecutive;
  canALastBusOffSnapshot.rxAgeMs = lastA ? (uint32_t)(now - lastA) : 0;
  canALastBusOffSnapshot.rxOverflowCount = overflowCount;
  canALastBusOffSnapshot.txOk = nativeATxOk;
  canALastBusOffSnapshot.txFail = nativeATxFail;
  portEXIT_CRITICAL(&canATxTraceMux);
  canBusOffPersistenceMarkDirty(CAN_BUS_OFF_BUS_A_PURE);
}

// Initialize the explicit A-route TWAI handle and publish readiness after start.
static bool nativeAInitChecked() {
  nativeAReady = p4CanReinstall(p4CanBusA(), canUsbPassive());
  if (nativeAReady) { nativeATxFailConsecutive = 0; nativeAState = 0; }
  else nativeAState = 2;
  return nativeAReady;
}

static bool nativeAReinit() {
  return nativeAInitChecked();
}

static void canTwaiHandleAlerts() {
  uint32_t twaiAlerts = 0;
  if (twai_read_alerts_v2(p4CanHandleB(), &twaiAlerts, 0) != ESP_OK || twaiAlerts == 0) return;
  const uint32_t errorAlerts = twaiAlerts &
      (TWAI_ALERT_TX_FAILED | TWAI_ALERT_ERR_PASS | TWAI_ALERT_BUS_ERROR);
  const bool busOff = (twaiAlerts & TWAI_ALERT_BUS_OFF) != 0;
  if (!(errorAlerts || busOff)) return;
  const uint32_t alertNow = (uint32_t)millis();
  canTwaiObserveErrorAlerts(errorAlerts, busOff, alertNow);
  if (busOff) {
    twai_status_info_t alertSt = {};
    canBTraceFreezeBusOff(alertNow);
    if (twai_get_status_info_v2(p4CanHandleB(), &alertSt) == ESP_OK)
      recordTwaiBusOffSnapshot(alertSt, alertNow);
  }
}

// Third controller: Party on standard profiles, Chassis on YL. YL Chassis
// remains observable; existing YL feature routes stay on Party and Body.
static void p4CanDrainExtra() {
  CanFrame frame;
  for (uint8_t count = 0; count < 32; ++count) {
    if (p4CanReadFrame(p4CanBusExtra(), &frame) != ESP_OK) break;
    __atomic_store_n(&lastCanFrameMs, (unsigned long)millis(), __ATOMIC_RELAXED);
    canUsbObserve(frame.bus, frame.can_id & 0x1fffffffUL,
        ((frame.can_id & 0x80000000UL) ? 1 : 0) |
        ((frame.can_id & 0x40000000UL) ? 2 : 0), frame.can_dlc, frame.data);
    if ((frame.can_id & 0xc0000000UL) != 0) continue;
    if (p4CanOnline[p4CanBusExtra()]) p4CanExtraFresh = true;
    researchCaptureObserve(RESEARCH_CAPTURE_BUS_EXTRA, frame.can_id, frame.can_dlc, frame.data);
    driverMonitorCaptureObserve(2, frame.can_id, frame.can_dlc, frame.data);
    if (!canUsbPassive() && !activeProfileIsYl()) nagProcessNativeAFrame(frame);
  }
}

static void p4CanServiceNativeStatus(uint8_t bus, bool routeA) {
  const twai_handle_t handle = p4CanHandles[bus];
  if (!handle) return;
  uint32_t alerts = 0;
  twai_read_alerts_v2(handle, &alerts, 0);
  twai_status_info_t st = {};
  if (twai_get_status_info_v2(handle, &st) != ESP_OK) {
    p4CanOnline[bus] = false;
    if (routeA) nativeAReady = false;
    return;
  }
  p4CanPublishStatus(bus, st);
  if (routeA) {
    nativeALastControllerState = (uint8_t)st.state;
    nativeALastControllerStateMs = millis();
    // Overflow telemetry is a controller counter, never an emulated register.
    nativeARxOverflowCount = st.rx_missed_count + st.rx_overrun_count;
    canARxDiagnostics.controllerOverrunObservations = st.rx_overrun_count;
    if (alerts & TWAI_ALERT_RX_QUEUE_FULL) {
      nativeARxOverflowLastMs = millis();
      canARxDiagnosticsObserveOverflowPure(canARxDiagnostics, millis(), 0, true, false);
    }
  }
  if (st.state == TWAI_STATE_BUS_OFF) {
    if (p4CanOnline[bus]) {
      p4CanOnline[bus] = false;
      __atomic_add_fetch(&p4CanBusOffCount[bus], 1U, __ATOMIC_RELAXED);
      if (routeA) {
        nativeAReady = false;
        canATraceFreezeBusOff(millis(), (uint8_t)st.state, nativeATxFailConsecutive);
        invalidateCanTxStateForCanARecovery();
      } else {
        p4CanExtraFresh = false;
        invalidateCanTxStateForFullRecovery();
      }
    }
    if (twai_initiate_recovery_v2(handle) == ESP_OK) {
      p4CanRecoveries[bus]++;
    } else {
      p4CanRecoveryFailures[bus]++;
      requestCanSubsystemRestart(CAN_SUP_HARD_STALE, CAN_REC_NATIVE_A_REINIT_FAIL);
    }
  } else if (st.state == TWAI_STATE_STOPPED && !canMaintenanceActive()) {
    if (twai_start_v2(handle) == ESP_OK) p4CanOnline[bus] = true;
    else requestCanSubsystemRestart(CAN_SUP_HARD_STALE, CAN_REC_NATIVE_A_REINIT_FAIL);
  } else p4CanOnline[bus] = st.state == TWAI_STATE_RUNNING;
  if (routeA) {
    nativeAReady = p4CanOnline[bus];
    nativeAState = nativeAReady ? ((st.tx_error_counter >= 96 || st.rx_error_counter >= 96) ? 1 : 0) : 2;
  }
}

static void canTaskNativeA(void* arg) {
  P4CAN_SERIAL_PRINTLN("[CAN A] TWAI task started");
  canTaskDiagnosticsResetPure(canTaskNativeADiagnostics);
  for (;;) {
    const uint32_t loopStartMs = (uint32_t)millis();
    canTaskNativeAHeartbeatMs = loopStartMs;
    canTaskDiagnosticsHeartbeatPure(canTaskNativeADiagnostics, loopStartMs,
                                    (uint32_t)micros());
    if (canTasksStopping || canMaintenanceActive()) {
      canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                       CAN_TASK_STAGE_QUIESCE,
                                       (uint32_t)millis());
      canTaskNativeAQuiesced = true;
      while (canTasksStopping || canMaintenanceActive()) vTaskDelay(pdMS_TO_TICKS(5));
      canTaskNativeAQuiesced = false;
      continue;
    }
    p4CanDrainExtra();
    // ── BOUNDED READ LOOP ──
    // Process each TWAI frame before reading the next. The 32-frame yield
    // budget and task priority remain unchanged; saved settings cannot batch RX.
    static constexpr uint8_t NATIVE_A_PREFETCH_CAPACITY = canARxReadBatchBudgetPure();
    const uint8_t readBatchBudget = canARxReadBatchBudgetPure();
    struct CanFrame prefetched[NATIVE_A_PREFETCH_CAPACITY];
    uint32_t prefetchedRxMs[NATIVE_A_PREFETCH_CAPACITY];
    uint8_t processed = 0;
    bool noMoreFrames = false;
    canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                     CAN_TASK_STAGE_RECEIVE,
                                     (uint32_t)millis());
    while (processed < NATIVE_A_RX_BUDGET && !noMoreFrames) {
      const uint32_t countryRxEpoch = canTxEpochSnapshot();
      uint8_t batch = 0;
      while (batch < readBatchBudget &&
             processed + batch < NATIVE_A_RX_BUDGET &&
             p4CanReadFrameA(&prefetched[batch]) == ESP_OK) {
        prefetchedRxMs[batch] = (uint32_t)millis();
        const struct CanFrame &raw = prefetched[batch];
        canUsbObserve(p4CanBusA(), raw.can_id & ((raw.can_id & 0x80000000UL) ? 0x1FFFFFFFUL : 0x7FFUL),
            ((raw.can_id & 0x80000000UL) ? 1 : 0) | ((raw.can_id & 0x40000000UL) ? 2 : 0),
            raw.can_dlc, raw.data);
        batch++;
      }
      if (batch == 0) break;
      noMoreFrames = batch < readBatchBudget;
      for (uint8_t bi = 0; bi < batch; ++bi) {
      canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                       CAN_TASK_STAGE_PROCESS,
                                       (uint32_t)millis());
      const struct CanFrame &rxf = prefetched[bi];
      processed++;
      const uint32_t frameNow = (uint32_t)millis();
      lastCanAFrameMs = frameNow;
      nativeARxCount++;
      canRxObserve(CAN_RX_BUS_PARTY, frameNow);
      if (canUsbPassive()) continue;
      // All downstream Model YL decoders and TX decisions expect standard
      // 11-bit DATA frames. Aggregate telemetry still counts rejected frames.
      if ((rxf.can_id & 0xC0000000UL) != 0) continue;
      const uint16_t partyId = (uint16_t)(rxf.can_id & 0x7FF);
      continuousApObserveStock(0, partyId, rxf.data, rxf.can_dlc, countryRxEpoch, prefetchedRxMs[bi]);
      canTxMarkFresh(CAN_TX_FRESH_PARTY); // physical CAN A fresh in this recovery epoch
      bootCaptureObservePartyFrame(partyId, rxf.can_dlc, rxf.data);
      researchCaptureObserveParty(partyId, rxf.can_dlc, rxf.data);
      burstFlashObserveRx(partyId, rxf.data, rxf.can_dlc, 0u, frameNow);
      driverMonitorCaptureObserve(DRIVER_MONITOR_BUS_A, partyId, rxf.can_dlc, rxf.data);
      if (partyId == UI_CHASSIS_CONTROL_ID && rxf.can_dlc >= 8)
        uiAutoLaneChangeObserveAndInjectCanA(rxf);
      if (partyId == 0x7FF && rxf.can_dlc == 8)
        countryOverrideObserve7ffCanA(rxf, countryRxEpoch);
      if (partyId == 0x238 && rxf.can_dlc == 8)
        countryOverrideObserve238CanA(rxf, countryRxEpoch);
      if (partyId == 0x3FD && rxf.can_dlc == 8)
        visionControlObserveBody(rxf, countryRxEpoch, prefetchedRxMs[bi]);
      if (activeCanAIsParty()) {
        // Party CAN on CAN A: Nag Killer is topology-gated. Keep YL-only
        // DAS/Summon/visual-debug behavior explicitly model-gated so selecting
        // Party + Chassis on Standard 3/Y cannot accidentally enable YL paths.
        nagProcessNativeAFrame(rxf);
        if (activeProfileIsYl() && partyId == VISUAL_DEBUG_ID && rxf.can_dlc >= 8) {
          visualBehaviorType = (uint8_t)readBitsLE(rxf.data, 56, 2);
          visualDebugRxCount++;
          visualDebugLastMs = frameNow;
          evaluateAutoBlinker();
        }
      } else if (activeCanAIsBody()) {
        // Standard 3/Y Body CAN: physical turn controls and front-interior
        // door-open switch used by the optional lane-change cancel action.
        if (nagTsl9Body39BSelected())
          (void)nagProcessTsl9NativeA(rxf);
        if (partyId == UI_POWERTRAIN_ID && rxf.can_dlc == 8 && activeProfilePedalMapSupported())
          handlePedalMap334OnCanA(rxf);
        if (partyId == LEFTSTALK_ID && rxf.can_dlc >= 3 && activeTurnSignalVariant == TURN_SIGNAL_STALK)
          handle249OnCanA(rxf.data, rxf.can_dlc);
        if (partyId == VCLEFT_SWITCH_ID && rxf.can_dlc >= 8 && activeTurnSignalVariant == TURN_SIGNAL_STALKLESS)
          handle3C2OnCanA(rxf);
        if (partyId == VCLEFT_SWITCH_ID && rxf.can_dlc >= 8 &&
            activeProfileTsl9InputOnBodyCanA()) {
          tsl9InputObserveCanA(rxf);
        }
        if (partyId == DOOR_SWITCH_ID && rxf.can_dlc >= 4)
          handle102LaneChangeCancel(rxf.data, rxf.can_dlc);
      }
      }
      canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                       CAN_TASK_STAGE_RECEIVE,
                                       (uint32_t)millis());
    }

    canARxDiagnosticsCompleteLoopPure(canARxDiagnostics, processed,
                                      NATIVE_A_RX_BUDGET);
    canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                     CAN_TASK_STAGE_SERVICE,
                                     (uint32_t)millis());
    continuousApService(ContApRoute::BodyA);
    tsl9InputServiceCanA();

    // ── STATUS CHECK / RECOVERY (100 ms, as in the reference) ──
    canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                     CAN_TASK_STAGE_STATUS,
                                     (uint32_t)millis());
    unsigned long now = millis();
    if (now - lastNativeAStatusMs >= 100) {
      lastNativeAStatusMs = now;

      p4CanServiceNativeStatus(p4CanBusA(), true);
      p4CanServiceNativeStatus(p4CanBusExtra(), false);
    }

    canTaskDiagnosticsEnterStagePure(canTaskNativeADiagnostics,
                                     CAN_TASK_STAGE_DELAY,
                                     (uint32_t)millis());
    canTaskDiagnosticsFinishLoopPure(canTaskNativeADiagnostics,
                                     (uint32_t)micros());
    vTaskDelay(1);
  }
}

static void canTaskTwai(void* arg) {
  P4CAN_SERIAL_PRINTLN("[CAN B] TWAI task started");
  unsigned long lastTwaiStatusMs = 0;
  unsigned long lastNoCanWarn = 0;
  uint32_t lastQueueStatusMs = 0;

  canTaskDiagnosticsResetPure(canTaskTwaiDiagnostics);
  for (;;) {
    const uint32_t loopStartMs = (uint32_t)millis();
    canTaskTwaiHeartbeatMs = loopStartMs;
    canTaskDiagnosticsHeartbeatPure(canTaskTwaiDiagnostics, loopStartMs,
                                    (uint32_t)micros());
    if (canTasksStopping || canMaintenanceActive()) {
      canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                       CAN_TASK_STAGE_QUIESCE,
                                       (uint32_t)millis());
      canTaskTwaiQuiesced = true;
      while (canTasksStopping || canMaintenanceActive()) vTaskDelay(pdMS_TO_TICKS(5));
      canTaskTwaiQuiesced = false;
      continue;
    }
    // Drain completion/error alerts before the potentially busy RX batch so
    // TX_SUCCESS observation latency stays bounded by one task loop.
    canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                     CAN_TASK_STAGE_ALERTS,
                                     (uint32_t)millis());
    canTwaiHandleAlerts();
    twai_message_t f;
    uint8_t rxBudget = 0;
    canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                     CAN_TASK_STAGE_RECEIVE,
                                     (uint32_t)millis());
    uint32_t countryRxEpoch = canTxEpochSnapshot();
    esp_err_t rxResult = p4CanReceiveRaw(p4CanBusB(), &f);
    uint32_t stockRxMs = (uint32_t)millis();
    while (rxBudget < TWAI_RX_DRAIN_BUDGET && rxResult == ESP_OK) {
      rxBudget++;
      canUsbObserve(p4CanBusB(), f.identifier, (f.extd ? 1 : 0) | (f.rtr ? 2 : 0),
                    f.data_length_code, f.data);
      canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                       CAN_TASK_STAGE_PROCESS,
                                       (uint32_t)millis());
      // v3.6d2 R79 fast path: this is deliberately before millis(), RX-gap
      // accounting, capture, and normal decoding. Preserve the existing d1
      // fail-open/manual-latch authorization policy inside the fast function.
      if (!canUsbPassive() && !f.extd && !f.rtr && f.data_length_code <= 8 && f.identifier <= 0x7ff)
        continuousApObserveStock(1, (uint16_t)f.identifier, f.data, f.data_length_code, countryRxEpoch, stockRxMs);
      bool mux1StockClaimed = false;
      if (!canUsbPassive() && !f.extd && !f.rtr && f.data_length_code <= 8 && f.identifier <= 0x7ff && f.identifier == 0x3FD && f.data_length_code >= 8) {
        const uint8_t timingMux = readMuxID(f.data);
        if (f.data_length_code == 8u && timingMux == 1u)
          visionControlCacheStock(VISION_CONTROL_CHASSIS_PURE, f.data, countryRxEpoch, stockRxMs);
        const uint32_t r79FrameNowMs = (uint32_t)millis();
        mux1StockClaimed = r79ProcessStockFrame(f, timingMux, r79FrameNowMs);
      }
      // Keep the supervisor heartbeat alive even under sustained CAN B traffic.
      const uint32_t frameNow = (uint32_t)millis();
      canTaskTwaiHeartbeatMs = frameNow;
      canTaskDiagnosticsPulsePure(canTaskTwaiDiagnostics, frameNow);
      const uint32_t previousCanBFrameMs = lastCanBFrameMs;
      if (previousCanBFrameMs != 0) {
        const uint32_t rxGapMs = (uint32_t)(frameNow - previousCanBFrameMs);
        canBLastRxGapMs = rxGapMs;
        if (rxGapMs > canBMaxRxGapMs) canBMaxRxGapMs = rxGapMs;
      }
      lastCanBFrameMs = frameNow;
      canRxObserve(CAN_RX_BUS_VH, frameNow);
      // Only standard 11-bit DATA frames may reach Tesla decoders or TX paths.
      if (!canUsbPassive() && !f.extd && !f.rtr && f.data_length_code <= 8 && f.identifier <= 0x7ff) {
        canTxMarkFresh(CAN_TX_FRESH_VH);
        bootCaptureObserveVhFrame(f.identifier, f.data_length_code);
        researchCaptureObserveVh((uint16_t)f.identifier, f.data_length_code, f.data);
        burstFlashObserveRx((uint16_t)f.identifier, f.data, f.data_length_code, 1u, frameNow);
        driverMonitorCaptureObserve(DRIVER_MONITOR_BUS_B, (uint16_t)f.identifier, f.data_length_code, f.data);
        switch (f.identifier) {
        case UI_CHASSIS_CONTROL_ID:
          uiAutoLaneChangeObserveAndInjectCanB(f);
          break;
        case UI_POWERTRAIN_ID:
          // Model Y L carries the pedal-map frame on VH / CAN B. Standard 3/Y
          // Body+Chassis uses the CAN-A Body copy instead and must ignore any
          // numeric 0x334 that happens to exist on Chassis CAN.
          if (activeProfileIsYl() && activeProfilePedalMapSupported()) handlePedalMap334OnCanB(f);
          break;
        case LEFTSTALK_ID:
          if (activeProfileIsYl() && activeTurnSignalVariant == TURN_SIGNAL_STALK && f.data_length_code >= 3)
            handle249OnCanB(f.data, f.data_length_code);
          break;
        case VCLEFT_SWITCH_ID:
          if (activeProfileIsYl() && f.data_length_code >= 8)
            handleDriverWindowLab3C2CanB(f);
          if (activeProfileTsl9InputSupported() &&
              !activeProfileTsl9InputOnBodyCanA() && f.data_length_code >= 8) {
            tsl9InputObserveCanB(f);
          }
          break;
        case DOOR_SWITCH_ID:
          if (activeProfileIsYl() && f.data_length_code >= 4)
            handle102LaneChangeCancel(f.data, f.data_length_code);
          break;
        case VISUAL_DEBUG_ID:
          if (activeCanBIsChassis() && f.data_length_code >= 8) {
            visualBehaviorType = (uint8_t)readBitsLE(f.data, 56, 2);
            visualDebugRxCount++;
            visualDebugLastMs = frameNow;
            if (activeProfileAdvancedEapSupported()) evaluateAutoBlinker();
          }
          break;
        case 280:
          if (activeCanBIsChassis() && f.data_length_code >= 7) handle280(f.data);
          break;
        case 390:
          if (activeCanBIsChassis() && f.data_length_code >= 8) handle390(f.data);
          break;
        case 921:
          if (activeCanBIsChassis() && f.data_length_code >= 6) {
            (void)nagProcessTsl9Twai399(f);
            handle921(f.data, f.data_length_code);
          }
          break;
        case 0x331:
          if (activeCanBIsChassis()) doInjectTlsscRestore(f);
          break;
        // Model YL/public-DBC reference: UI_driverAssistMapData road context.
        case 0x238:
          if (f.data_length_code >= 5) handleRoadContext238(f.data, f.data_length_code);
          countryOverrideObserve238CanB(f, countryRxEpoch);
          break;
        case 0x7FF:
          countryOverrideObserve7ffCanB(f, countryRxEpoch);
          break;

        // 1016 (SPR) is read on CAN B for both models.
        case DRIVER_ASSIST_ID:
          // Always retain stock telemetry before applying the production/research overlay.
          handle1016(f.data, f.data_length_code);
          lab3f8FrameRxMs = stockRxMs;
          lab3f8FrameRxEpoch = countryRxEpoch;
          injectDriverAssistControl(f);
          break;
        case 1021:
          if (f.data_length_code >= 8) {
            uint8_t mux = readMuxID(f.data);
            if (mux == 1) {
              r79LabObserve3fdMux1(f.data, f.data_length_code);
              const bool ulcCloneClaimed = injectUlcSnooze3fdMux1(f);
              visionControlObserveStock(f, countryRxEpoch, mux1StockClaimed || ulcCloneClaimed, stockRxMs);
            } else if (mux == 0) injectTLSSC(f);
          }
          break;

        default:
          break;
        }
      }
      canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                       CAN_TASK_STAGE_RECEIVE,
                                       (uint32_t)millis());
      countryRxEpoch = canTxEpochSnapshot();
      rxResult = rxBudget < TWAI_RX_DRAIN_BUDGET ? p4CanReceiveRaw(p4CanBusB(), &f) : ESP_ERR_TIMEOUT;
      stockRxMs = (uint32_t)millis();
    }

    // Refresh Summon evidence before R79 retry/periodic servicing. The 5 s
    // V2.6 PARK fallback remains gate-compatible, but d1 queue priority uses
    // fresh real gear rather than gateParked.
    canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                     CAN_TASK_STAGE_SERVICE,
                                     (uint32_t)millis());
    refreshSummonState();
    driverWindowLabServiceTick();
    r79TransportTick();
    // R79 immediate/retry/periodic work always gets first access to the CAN-B
    // TX queue. TSL9 input assistance is intentionally lower priority.
    continuousApService(ContApRoute::ChassisB);
    tsl9InputServiceCanB();
    const uint32_t captureNow = (uint32_t)millis();
    researchCaptureTick(captureNow);
    driverMonitorCaptureTick(captureNow);
    burstFlashTick(captureNow);
    const uint32_t queueNow = (uint32_t)millis();
    if ((uint32_t)(queueNow - lastQueueStatusMs) >= TWAI_QUEUE_TELEMETRY_PERIOD_MS) {
      lastQueueStatusMs = queueNow;
      twaiReadQueueStatus();
    }
    blinkATxTick();

    // Read alerts again after RX/tick work. BUS_OFF snapshot behavior is
    // preserved; d3 additionally consumes TX_IDLE/TX_SUCCESS for diagnostics.
    canTwaiHandleAlerts();

    // TWAI status / recovery. Recovery ends in STOPPED, so explicitly
    // restart the driver instead of leaving CAN B silent after BUS_OFF.
    canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                     CAN_TASK_STAGE_STATUS,
                                     (uint32_t)millis());
    unsigned long now = millis();
    if (now - lastTwaiStatusMs >= 100) {
      lastTwaiStatusMs = now;
      twai_status_info_t st = {};
      if (twai_get_status_info_v2(p4CanHandleB(), &st) == ESP_OK) {
        p4CanCacheBStatus(st);
        p4CanPublishStatus(p4CanBusB(), st);
        if (st.state == TWAI_STATE_RUNNING) {
          twaiReady = true; p4CanOnline[p4CanBusB()] = true;
        } else if (st.state == TWAI_STATE_BUS_OFF) {
          twaiReady = false; p4CanOnline[p4CanBusB()] = false;
          canTwaiBusOffCount++;
          __atomic_add_fetch(&p4CanBusOffCount[p4CanBusB()], 1U, __ATOMIC_RELAXED);
          canTwaiLastEventReason = CAN_REC_TWAI_BUS_OFF;
          canTwaiLastEventMs = (uint32_t)now;
          uint32_t frozenOrdinal = 0, frozenMs = 0;
          portENTER_CRITICAL(&canBTxTraceMux);
          frozenOrdinal = canBTxTraceFrozenBusOffOrdinal;
          frozenMs = canBTxTraceFrozenMs;
          portEXIT_CRITICAL(&canBTxTraceMux);
          bool snapshotValid = false;
          uint32_t snapshotCapturedMs = 0;
          portENTER_CRITICAL(&canRecoveryMux);
          snapshotValid = canTwaiLastBusOffSnapshot.valid;
          snapshotCapturedMs = canTwaiLastBusOffSnapshot.capturedMs;
          portEXIT_CRITICAL(&canRecoveryMux);
          bool snapshotMatchesFrozen = snapshotValid &&
              snapshotCapturedMs == frozenMs &&
              frozenOrdinal == canTwaiBusOffCount;
          if (frozenOrdinal != canTwaiBusOffCount) {
            // Alert delivery is normally immediate. If it was missed, preserve
            // the old poll-based behavior as a fail-safe and freeze now.
            canTwaiObserveErrorAlerts(0u, true, (uint32_t)now);
            canBTraceFreezeBusOff((uint32_t)now);
            frozenMs = (uint32_t)now;
            snapshotMatchesFrozen = false;
          }
          if (!snapshotMatchesFrozen) {
            // The alert path may have frozen trace/evidence while its immediate
            // status read failed. Complete that snapshot from this poll.
            recordTwaiBusOffSnapshot(st, frozenMs);
          }
          canBusOffPersistenceMarkDirty(CAN_BUS_OFF_BUS_B_PURE);
          P4CAN_SERIAL_PRINTLN("[CAN B] TWAI bus-off -> recovery started");
          invalidateCanTxStateForCanBRecovery();
          const esp_err_t recoveryErr = twai_initiate_recovery_v2(p4CanHandleB());
          if (recoveryErr == ESP_OK) {
            p4CanRecoveries[p4CanBusB()]++;
            canTwaiLocalRecoveryStartCount++;
          } else {
            canTwaiRecoveryStartFailCount++;
            p4CanRecoveryFailures[p4CanBusB()]++;
            canTwaiLastEventReason = CAN_REC_TWAI_RECOVERY_FAIL;
            canTwaiLastEventMs = (uint32_t)now;
            P4CAN_SERIAL_PRINTF("[CAN B] TWAI recovery start failed: %s\n", esp_err_to_name(recoveryErr));
            requestCanSubsystemRestart(CAN_SUP_HARD_STALE, CAN_REC_TWAI_RECOVERY_FAIL);
          }
        } else if (st.state == TWAI_STATE_STOPPED) {
          twaiReady = false; p4CanOnline[p4CanBusB()] = false;
          canTwaiStoppedCount++;
          if (canTwaiLastEventReason == CAN_REC_NONE) {
            canTwaiLastEventReason = CAN_REC_TWAI_STOPPED;
            canTwaiLastEventMs = (uint32_t)now;
          }
          invalidateCanTxStateForCanBRecovery();
          esp_err_t rs = twai_start_v2(p4CanHandleB());
          if (rs == ESP_OK) {
            canTwaiRestartOkCount++;
            canTwaiResetErrorAlertWindow();
            twaiReady = true; p4CanOnline[p4CanBusB()] = true;
            P4CAN_SERIAL_PRINTLN("[CAN B] TWAI recovery complete -> restarted");
          } else {
            canTwaiRestartFailCount++;
            canTwaiLastEventReason = CAN_REC_TWAI_RESTART_FAIL;
            canTwaiLastEventMs = (uint32_t)now;
            P4CAN_SERIAL_PRINTF("[CAN B] TWAI restart failed: %s\n", esp_err_to_name(rs));
            requestCanSubsystemRestart(CAN_SUP_HARD_STALE, CAN_REC_TWAI_RESTART_FAIL);
          }
        } else {
          twaiReady = false; p4CanOnline[p4CanBusB()] = false;
        }
      }
    }

    // No-CAN warning (shared counter)
    if ((millis() - bootTime) > 20000 && canRxTotal() == 0) {
      if (millis() - lastNoCanWarn > 5000) {
        P4CAN_SERIAL_PRINTLN("No CAN frames yet on the monitored buses, staying alive.");
        lastNoCanWarn = millis();
      }
    }

    canTaskDiagnosticsEnterStagePure(canTaskTwaiDiagnostics,
                                     CAN_TASK_STAGE_DELAY,
                                     (uint32_t)millis());
    canTaskDiagnosticsFinishLoopPure(canTaskTwaiDiagnostics,
                                     (uint32_t)micros());
    vTaskDelay(1);
  }
}


// ═══════════════════════════════════════════════════════════════
// RECOVERY-ONLY CAN SUBSYSTEM SUPERVISOR
// ═══════════════════════════════════════════════════════════════

static inline bool recoveryFresh(uint32_t now, uint32_t ts, uint32_t timeoutMs) {
  return ts != 0 && (uint32_t)(now - ts) <= timeoutMs;
}

static void recordCanTaskHeartbeatTimeout(uint32_t now, bool aDead, bool bDead) {
  const uint8_t cause = aDead
      ? (bDead ? CAN_TASK_HEARTBEAT_BOTH : CAN_TASK_HEARTBEAT_A)
      : (bDead ? CAN_TASK_HEARTBEAT_B : CAN_TASK_HEARTBEAT_NONE);
  if (cause == CAN_TASK_HEARTBEAT_NONE) return;
  const uint8_t stateA = canTaskStateCode(canTaskNativeAHandle);
  const uint8_t stateB = canTaskStateCode(canTaskTwaiHandle);
  const uint32_t stackA = canTaskNativeAHandle
      ? (uint32_t)uxTaskGetStackHighWaterMark(canTaskNativeAHandle) : 0u;
  const uint32_t stackB = canTaskTwaiHandle
      ? (uint32_t)uxTaskGetStackHighWaterMark(canTaskTwaiHandle) : 0u;
  const CanTaskTimeoutSnapshotPure snapshotA = canTaskDiagnosticsSnapshotPure(
      canTaskNativeADiagnostics, now, stateA, stackA);
  const CanTaskTimeoutSnapshotPure snapshotB = canTaskDiagnosticsSnapshotPure(
      canTaskTwaiDiagnostics, now, stateB, stackB);
  portENTER_CRITICAL(&canRecoveryMux);
  // Snapshot once for the restart request that is about to be queued. The
  // supervisor may loop again before reinitialization starts.
  if (canSupervisorCommand < CAN_SUP_HARD_STALE) {
    canTaskHeartbeatLastCause = cause;
    canTaskHeartbeatLastAgeAms = snapshotA.heartbeatAgeMs;
    canTaskHeartbeatLastAgeBms = snapshotB.heartbeatAgeMs;
    canTaskHeartbeatLastSnapshotA = snapshotA;
    canTaskHeartbeatLastSnapshotB = snapshotB;
    if (cause == CAN_TASK_HEARTBEAT_A) canTaskHeartbeatTimeoutCountA++;
    else if (cause == CAN_TASK_HEARTBEAT_B) canTaskHeartbeatTimeoutCountB++;
    else canTaskHeartbeatTimeoutCountBoth++;
  }
  portEXIT_CRITICAL(&canRecoveryMux);
}

static void requestCanSubsystemRestart(uint8_t reason, uint8_t diagReason) {
  portENTER_CRITICAL(&canRecoveryMux);
  // Keep the first cause at a given supervisor priority. A higher-priority
  // request (for example MANUAL over STALE) replaces both command and cause.
  if (reason > canSupervisorCommand) {
    canSupervisorCommand = reason;
    canPendingHardDiagReason = diagReason;
  }
  portEXIT_CRITICAL(&canRecoveryMux);
}

static bool recoveryNativeAColdInit() {
  nativeAReady = false;
  p4CanExtraFresh = false;
  const bool a = nativeAInitChecked();
  const bool c = p4CanReinstall(p4CanBusExtra(), canUsbPassive());
  return a && c;
}

static bool recoveryTwaiInstallFresh() {
  twaiReady = p4CanInstall(p4CanBusB(), canUsbPassive(), TWAI_TX_QUEUE_LEN);
  if (twaiReady) {
    canTwaiResetErrorAlertWindow();
    __atomic_fetch_add(&canUsbControllerEpoch, 1U, __ATOMIC_RELAXED);
  }
  return twaiReady;
}

static bool recoveryTwaiFullReinit() {
  twaiReady = p4CanReinstall(p4CanBusB(), canUsbPassive(), TWAI_TX_QUEUE_LEN);
  if (twaiReady) {
    canTwaiResetErrorAlertWindow();
    __atomic_fetch_add(&canUsbControllerEpoch, 1U, __ATOMIC_RELAXED);
  }
  return twaiReady;
}

// Maintenance never deletes tasks while they may own controller/state locks and never
// starts a controller. OTA must not touch flash until every owner has parked.
static bool prepareCanForMaintenance() {
  __atomic_store_n(&canMaintenanceRequested, true, __ATOMIC_RELEASE);
  const bool supervisorSelf = canSupervisorHandle &&
      xTaskGetCurrentTaskHandle() == canSupervisorHandle;
  if (supervisorSelf)
    __atomic_store_n(&canMaintenanceSupervisorParked, true, __ATOMIC_RELEASE);
  bool expected = false;
  if (!__atomic_compare_exchange_n(&canMaintenancePreparing, &expected, true,
                                  false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return false;
  const auto finish = [](bool ok) {
    __atomic_store_n(&canMaintenancePreparing, false, __ATOMIC_RELEASE);
    return ok;
  };
  if (__atomic_load_n(&canMaintenanceStopped, __ATOMIC_ACQUIRE)) return finish(true);
  canMaintenanceObservedCanRuntime = canMaintenanceObservedCanRuntime ||
      nativeAReady || twaiReady || canSupervisorHandle ||
      canTaskNativeAHandle || canTaskTwaiHandle;
  // Publish the hold first; then drain any sender that already owns the barrier.
  canTxAdministrativeHold = true;
  if (canTxBarrierMutex) {
    if (xSemaphoreTake(canTxBarrierMutex, pdMS_TO_TICKS(500)) != pdTRUE) return finish(false);
    canTxAdministrativeHold = true;
    xSemaphoreGive(canTxBarrierMutex);
  }
  uint32_t started = (uint32_t)millis();
  while (canSupervisorHandle &&
         !__atomic_load_n(&canMaintenanceSupervisorParked, __ATOMIC_ACQUIRE)) {
    if ((uint32_t)((uint32_t)millis() - started) >= 2000u) return finish(false);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  started = (uint32_t)millis();
  while ((canTaskNativeAHandle && !canTaskNativeAQuiesced) ||
         (canTaskTwaiHandle && !canTaskTwaiQuiesced)) {
    if ((uint32_t)((uint32_t)millis() - started) >= 500u) return finish(false);
    vTaskDelay(pdMS_TO_TICKS(5));
  }
  // Both RX tasks are parked; their three physical controllers can be stopped.
  nativeAReady = false;
  twaiReady = false; p4CanOnline[p4CanBusB()] = false;
  p4CanExtraFresh = false;
  if (!p4CanStopAll()) return finish(false);
  invalidateCanTxStateForFullRecovery();
  __atomic_store_n(&canMaintenanceStopped, true, __ATOMIC_RELEASE);
  return finish(true);
}

static void restartP4CanSafely() {
  // A failed handshake never falls through to an uncoordinated reboot.
  while (!prepareCanForMaintenance()) vTaskDelay(pdMS_TO_TICKS(100));
  ESP.restart();
}

static bool recoveryStopCanTasks() {
  if (!canTasksStopping) {
    canTaskNativeAQuiesced = false;
    canTaskTwaiQuiesced = false;
  }
  canTasksStopping = true;

  uint32_t start = (uint32_t)millis();
  while (((canTaskNativeAHandle && !canTaskNativeAQuiesced) ||
          (canTaskTwaiHandle && !canTaskTwaiQuiesced)) &&
         (uint32_t)((uint32_t)millis() - start) < 1000) {
    vTaskDelay(pdMS_TO_TICKS(5));
  }

  if ((canTaskNativeAHandle && !canTaskNativeAQuiesced) ||
      (canTaskTwaiHandle && !canTaskTwaiQuiesced)) return false;
  TaskHandle_t a = canTaskNativeAHandle;
  TaskHandle_t b = canTaskTwaiHandle;
  canTaskNativeAHandle = nullptr;
  canTaskTwaiHandle = nullptr;
  if (a) vTaskDelete(a);
  if (b) vTaskDelete(b);

  canTasksStopping = false;
  canTaskNativeAQuiesced = false;
  canTaskTwaiQuiesced = false;
  canTaskNativeAHeartbeatMs = 0;
  canTaskTwaiHeartbeatMs = 0;
  canTaskDiagnosticsResetPure(canTaskNativeADiagnostics);
  canTaskDiagnosticsResetPure(canTaskTwaiDiagnostics);
  vTaskDelay(pdMS_TO_TICKS(RECOVERY_TASK_STOP_SETTLE_MS));
  return true;
}

static bool recoveryStartCanTasks() {
  BaseType_t a = xTaskCreatePinnedToCore(canTaskNativeA, "canA", 8192, nullptr, 5, &canTaskNativeAHandle, 1);
  if (a != pdPASS) {
    canTaskNativeAHandle = nullptr;
    return false;
  }
  BaseType_t b = xTaskCreatePinnedToCore(canTaskTwai, "canB", 8192, nullptr, 4, &canTaskTwaiHandle, 1);
  if (b != pdPASS) {
    canTasksStopping = true;
    canTaskTwaiHandle = nullptr;
    return false;
  }
  return true;
}

static bool recoveryHardReinitialize(uint8_t reason, uint8_t diagReason) {
  if (canSubsystemBusy) return false;
  canSubsystemBusy = true;
  const int8_t bootCapHardIdx = bootCaptureHardStart(reason);
  canLastHardReinitReason = reason;
  canLastHardDiagReason = diagReason;
  canHardReinitCount++;
  P4CAN_SERIAL_PRINTF("[CAN SUP] hard CAN reinitialize #%lu reason=%u diag=%s\n",
                (unsigned long)canHardReinitCount, (unsigned)reason,
                canRecoveryDiagnosticReasonName(diagReason));

  // Preserve only the dashboard presentation. invalidateCanTxStateForFullRecovery()
  // still closes every functional AP/NOA gate before the controllers restart.
  portENTER_CRITICAL(&stateMux);
  apDisplayBeginHardInitPure(apDisplayHold, (uint32_t)millis());
  portEXIT_CRITICAL(&stateMux);

  if (!recoveryStopCanTasks()) {
    canHardReinitFailCount++;
    canSubsystemBusy = false;
    bootCaptureHardFinish(bootCapHardIdx, false);
    return false;
  }
  invalidateCanTxStateForFullRecovery();
  bool aOk = recoveryNativeAColdInit();
  bool bOk = recoveryTwaiFullReinit();
  bool tasksOk = aOk && bOk && recoveryStartCanTasks();

  lastCanAFrameMs = 0;
  lastCanBFrameMs = 0;
  canInitTime = millis();
  recoveryOneBusStaleStartMs = 0;
  recoveryWakeAcquireStartMs = (reason == CAN_SUP_HARD_ACQUIRE || recoveryEverBothActive)
                                 ? (uint32_t)millis() : 0;
  canSubsystemBusy = false;

  if (!(aOk && bOk && tasksOk)) {
    bootCaptureHardFinish(bootCapHardIdx, false);
    canHardReinitFailCount++;
    P4CAN_SERIAL_PRINTF("[CAN SUP] hard CAN reinitialize FAILED A=%u B=%u tasks=%u\n",
                  aOk ? 1 : 0, bOk ? 1 : 0, tasksOk ? 1 : 0);
    return false;
  }
  bootCaptureHardFinish(bootCapHardIdx, true);
  P4CAN_SERIAL_PRINTLN("[CAN SUP] hard CAN reinitialize complete");
  return true;
}

static void canRecoverySupervisorTick(uint32_t now) {
  canBusOffPersistenceService(now);
}

// Hardware mode changes have one owner: the CAN supervisor. USB waits for the
// acknowledgement and starts a capture only after both controllers are ready.
static void canUsbModeService() {
  if (!__atomic_exchange_n(&canUsbModePending, false, __ATOMIC_ACQ_REL)) return;
  bool ok = false;
  if (!canMaintenanceActive() && !canSubsystemBusy) {
    canSubsystemBusy = true;
    canTasksStopping = true;
    const uint32_t started = (uint32_t)millis();
    while ((!canTaskNativeAQuiesced || !canTaskTwaiQuiesced) &&
           (uint32_t)((uint32_t)millis() - started) < 1000U)
      vTaskDelay(pdMS_TO_TICKS(5));
    // Never delete a task that may still access or touch the TWAI driver.
    if (canTaskNativeAQuiesced && canTaskTwaiQuiesced) {
      if (canTaskNativeAHandle) vTaskDelete(canTaskNativeAHandle);
      if (canTaskTwaiHandle) vTaskDelete(canTaskTwaiHandle);
      canTaskNativeAHandle = canTaskTwaiHandle = nullptr;
      invalidateCanTxStateForFullRecovery();
      const bool aOk = recoveryNativeAColdInit();
      const bool bOk = recoveryTwaiFullReinit();
      canTasksStopping = false;
      canTaskNativeAQuiesced = canTaskTwaiQuiesced = false;
      lastCanAFrameMs = lastCanBFrameMs = 0;
      canInitTime = (uint32_t)millis();
      ok = aOk && bOk && recoveryStartCanTasks();
      // Leaving PASSIVE is persisted only after the hardware transition succeeds.
      if (ok && !canUsbPassive()) ok = canUsbSaveMode(false);
    }
    canSubsystemBusy = false;
  }
  if (ok && canTxBarrierMutex && xSemaphoreTake(canTxBarrierMutex, pdMS_TO_TICKS(500)) == pdTRUE) {
    __atomic_store_n(&canUsbTransitionHeld, false, __ATOMIC_RELEASE);
    canTxAdministrativeHold = canTxAdministrativeRequested ||
                              canMaintenanceActive() || canUsbTxHeld();
    xSemaphoreGive(canTxBarrierMutex);
  } else ok = false;
  __atomic_store_n(&canUsbModeResult, ok, __ATOMIC_RELEASE);
  __atomic_store_n(&canUsbModeFinished, true, __ATOMIC_RELEASE);
}

static bool canUsbRequestMode(bool passive) {
  if (canMaintenanceActive() || !nativeAReady || !twaiReady || !canSupervisorHandle ||
      !canTaskNativeAHandle || !canTaskTwaiHandle ||
      __atomic_load_n(&canUsbModePending, __ATOMIC_ACQUIRE)) return false;
  if (passive == canUsbPassive() &&
      !__atomic_load_n(&canUsbTransitionHeld, __ATOMIC_ACQUIRE)) return true;
  if (!canTxBarrierMutex || xSemaphoreTake(canTxBarrierMutex, pdMS_TO_TICKS(500)) != pdTRUE)
    return false;
  __atomic_store_n(&canUsbTransitionHeld, true, __ATOMIC_RELEASE);
  canTxAdministrativeHold = true;
  xSemaphoreGive(canTxBarrierMutex);
  if (passive && !canUsbSaveMode(true)) return false;
  __atomic_store_n(&canUsbPassiveMode, passive, __ATOMIC_RELEASE);
  __atomic_store_n(&canUsbModeFinished, false, __ATOMIC_RELEASE);
  __atomic_store_n(&canUsbModeResult, false, __ATOMIC_RELEASE);
  __atomic_store_n(&canUsbModePending, true, __ATOMIC_RELEASE);
  const uint32_t start = (uint32_t)millis();
  while (!__atomic_load_n(&canUsbModeFinished, __ATOMIC_ACQUIRE) &&
         (uint32_t)((uint32_t)millis() - start) < 5000U)
    vTaskDelay(pdMS_TO_TICKS(10));
  return __atomic_load_n(&canUsbModeFinished, __ATOMIC_ACQUIRE) &&
         __atomic_load_n(&canUsbModeResult, __ATOMIC_ACQUIRE);
}

static void canSupervisorTask(void* arg) {
  P4CAN_SERIAL_PRINTLN("[CAN SUP] recovery-only supervisor started");
  for (;;) {
    uint32_t now = (uint32_t)millis();
    if (canMaintenanceActive()) {
      __atomic_store_n(&canMaintenanceSupervisorParked, true, __ATOMIC_RELEASE);
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    canUsbModeService();
    if (__atomic_load_n(&canUsbTransitionHeld, __ATOMIC_ACQUIRE)) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    canRecoverySupervisorTick(now);

    // BUS-OFF persistence performs synchronous NVS write/read-back work.
    // Re-sample time afterwards, then use half-range age handling so a
    // concurrently published heartbeat can never look almost 2^32 ms old.
    now = (uint32_t)millis();

    if (!canSubsystemBusy) {
      // Independent task heartbeat: still advances while the vehicle is asleep.
      // Therefore silence on the CAN wires is not confused with a wedged task.
      bool graceDone = (uint32_t)(now - canInitTime) >= RECOVERY_TASK_START_GRACE_MS;
      bool aTaskDead = canTaskNativeAHandle && graceDone &&
                       canTaskHeartbeatTimedOutPure(
                           now, canTaskNativeAHeartbeatMs,
                           RECOVERY_TASK_HEARTBEAT_TIMEOUT_MS);
      bool bTaskDead = canTaskTwaiHandle && graceDone &&
                       canTaskHeartbeatTimedOutPure(
                           now, canTaskTwaiHeartbeatMs,
                           RECOVERY_TASK_HEARTBEAT_TIMEOUT_MS);
      if (aTaskDead || bTaskDead) {
        P4CAN_SERIAL_PRINTF("[CAN SUP] task heartbeat stale A=%u B=%u\n", aTaskDead ? 1 : 0, bTaskDead ? 1 : 0);
        recordCanTaskHeartbeatTimeout(now, aTaskDead, bTaskDead);
        requestCanSubsystemRestart(CAN_SUP_HARD_STALE, CAN_REC_TASK_HEARTBEAT_TIMEOUT);
      }

      bool aFresh = recoveryFresh(now, lastCanAFrameMs, RECOVERY_BUS_FRESH_MS);
      bool bFresh = recoveryFresh(now, lastCanBFrameMs, RECOVERY_BUS_FRESH_MS);
      bool extraFresh = recoveryFresh(now, p4CanLastRxMs[p4CanBusExtra()], RECOVERY_BUS_FRESH_MS);
      bool bothFresh = aFresh && bFresh && extraFresh;
      bool anyFresh = aFresh || bFresh || extraFresh;

      if (canUsbPassive()) {
        // A single connected bus or sleeping vehicle is normal in survey mode.
        recoveryOneBusStaleStartMs = 0;
        recoveryWakeAcquireStartMs = 0;
      } else if (bothFresh) {
        if (!recoveryEverBothActive || recoverySleeping || recoveryWakeAcquireStartMs != 0) {
          P4CAN_SERIAL_PRINTLN("[CAN SUP] BODY+CHASSIS+PARTY active");
        }
        recoveryEverBothActive = true;
        recoverySleeping = false;
        recoveryWakeAcquireStartMs = 0;
        recoveryOneBusStaleStartMs = 0;
        recoveryLastBothActiveMs = now;
        recoveryColdRetryCount = 0;
        recoveryColdRetriesExhausted = false;
      } else if (recoveryEverBothActive) {
        // Once a real active session has been observed, all three buses going quiet
        // together is treated as normal Tesla sleep, never as a CAN failure.
        if (!anyFresh) {
          recoveryOneBusStaleStartMs = 0;
          if (!recoverySleeping && recoveryLastBothActiveMs != 0 &&
              (uint32_t)(now - recoveryLastBothActiveMs) >= RECOVERY_SLEEP_QUIET_MS) {
            recoverySleeping = true;
            recoveryWakeAcquireStartMs = 0;
            canRecoverySleepCount++;
            P4CAN_SERIAL_PRINTF("[CAN SUP] vehicle CAN sleep #%lu -> passive wait\n",
                          (unsigned long)canRecoverySleepCount);
          }
        } else {
          if (recoverySleeping) {
            recoverySleeping = false;
            recoveryWakeAcquireStartMs = now;
            canRecoveryWakeCount++;
            P4CAN_SERIAL_PRINTF("[CAN SUP] vehicle CAN wake #%lu -> acquire other bus\n",
                          (unsigned long)canRecoveryWakeCount);
          }

          if (recoveryWakeAcquireStartMs == 0) {
            if (recoveryOneBusStaleStartMs == 0) recoveryOneBusStaleStartMs = now;
            if ((uint32_t)(now - recoveryOneBusStaleStartMs) >= RECOVERY_ONE_BUS_STALE_MS &&
                (recoveryLastHardRequestMs == 0 ||
                 (uint32_t)(now - recoveryLastHardRequestMs) >= RECOVERY_HARD_COOLDOWN_MS)) {
              recoveryLastHardRequestMs = now;
              recoveryOneBusStaleStartMs = 0;
              requestCanSubsystemRestart(CAN_SUP_HARD_STALE, CAN_REC_ONE_BUS_STALE);
            }
          }
        }

        if (!recoverySleeping && recoveryWakeAcquireStartMs != 0 &&
            (uint32_t)(now - recoveryWakeAcquireStartMs) >= RECOVERY_WAKE_ACQUIRE_MS &&
            (recoveryLastHardRequestMs == 0 ||
             (uint32_t)(now - recoveryLastHardRequestMs) >= RECOVERY_HARD_COOLDOWN_MS)) {
          recoveryLastHardRequestMs = now;
          recoveryWakeAcquireStartMs = now;
          requestCanSubsystemRestart(CAN_SUP_HARD_ACQUIRE, CAN_REC_WAKE_ACQUIRE_TIMEOUT);
        }
      } else {
        // Cold boot / P4-CAN reset before a full A+B acquisition.
        // Retry hard initialization only a bounded number of times. If the
        // vehicle is simply asleep, stop tearing controllers down and wait for
        // a real CAN frame to wake the acquisition path.
        if (anyFresh && recoveryWakeAcquireStartMs == 0) recoveryWakeAcquireStartMs = now;

        uint32_t sinceInit = (uint32_t)(now - canInitTime);
        bool acquisitionTimedOut = (recoveryWakeAcquireStartMs != 0)
          ? ((uint32_t)(now - recoveryWakeAcquireStartMs) >= RECOVERY_COLD_FIRST_ACQUIRE_MS)
          : (sinceInit >= RECOVERY_COLD_FIRST_ACQUIRE_MS);

        uint32_t interval = (recoveryColdRetryCount == 0)
          ? RECOVERY_COLD_FIRST_ACQUIRE_MS : RECOVERY_COLD_RETRY_INTERVAL_MS;
        bool intervalPassed = (recoveryLastHardRequestMs == 0) ||
                              ((uint32_t)(now - recoveryLastHardRequestMs) >= interval);

        if (acquisitionTimedOut && intervalPassed &&
            recoveryColdRetryCount < RECOVERY_COLD_MAX_RETRIES) {
          recoveryColdRetryCount++;
          recoveryLastHardRequestMs = now;
          recoveryWakeAcquireStartMs = 0;
          P4CAN_SERIAL_PRINTF("[CAN SUP] cold acquire retry %u/%u\n",
                        (unsigned)recoveryColdRetryCount,
                        (unsigned)RECOVERY_COLD_MAX_RETRIES);
          requestCanSubsystemRestart(CAN_SUP_HARD_ACQUIRE, CAN_REC_COLD_ACQUIRE_TIMEOUT);
        } else if (recoveryColdRetryCount >= RECOVERY_COLD_MAX_RETRIES &&
                   !recoveryColdRetriesExhausted) {
          recoveryColdRetriesExhausted = true;
          recoverySleeping = true;
          P4CAN_SERIAL_PRINTLN("[CAN SUP] no RX after bounded retries -> passive sleep/wake wait");
        }

        // If a real frame arrives after passive wait, resume bounded acquisition.
        if (recoveryColdRetriesExhausted && anyFresh) {
          recoveryColdRetriesExhausted = false;
          recoverySleeping = false;
          recoveryColdRetryCount = 0;
          recoveryWakeAcquireStartMs = now;
        }
      }
    }

    uint8_t cmd = CAN_SUP_NONE;
    uint8_t diagReason = CAN_REC_NONE;
    portENTER_CRITICAL(&canRecoveryMux);
    cmd = canSupervisorCommand;
    diagReason = canPendingHardDiagReason;
    canSupervisorCommand = CAN_SUP_NONE;
    canPendingHardDiagReason = CAN_REC_NONE;
    portEXIT_CRITICAL(&canRecoveryMux);

    if (cmd != CAN_SUP_NONE && !canSubsystemBusy) {
      if (!recoveryHardReinitialize(cmd, diagReason)) {
        P4CAN_SERIAL_PRINTLN("[CAN SUP] subsystem recovery failed -> reboot P4-CAN");
        restartP4CanSafely();
      }
    }

    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

// ═══════════════════════════════════════════════════════════════
// SETUP / LOOP
// ═══════════════════════════════════════════════════════════════
