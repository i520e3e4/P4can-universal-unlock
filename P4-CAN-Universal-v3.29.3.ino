// P4-CAN Universal v3.29.3 - Model 3/Y / Model YL firmware

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <esp_system.h>
#include <esp_heap_caps.h>
#include <freertos/semphr.h>
#include <Update.h>
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEScan.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLESecurity.h>
#if defined(CONFIG_BLUEDROID_ENABLED)
#include <esp_gap_ble_api.h>
#elif defined(CONFIG_NIMBLE_ENABLED)
#include <host/ble_gap.h>
#include <host/ble_store.h>
#include <nimble/nimble_port.h>
#endif
#include "driver/twai.h"
#include "pin_config.h"

// Optional burst-flash hooks. Definitions live in burst_flash.h, which is
// included later in this translation unit; s3xy_ble.h is included earlier and
// needs these declarations to dispatch the two button actions.
bool burstFlashTriggerFromButton(uint8_t channel);
void burstFlashObserveRx(uint16_t canId, const uint8_t *data, uint8_t dlc,
                         uint8_t bus, uint32_t nowMs);
void burstFlashTick(uint32_t nowMs);
// burstFlashInit() is static in burst_flash.h and called from setup(), which
// is defined after all includes, so it needs no forward declaration here.

#ifndef P4CAN_SERIAL_DIAGNOSTICS
#define P4CAN_SERIAL_DIAGNOSTICS 0
#endif
#include "serial_diag.h"
#if P4CAN_SERIAL_DIAGNOSTICS
#error "USB CAN logger requires diagnostics disabled to preserve protocol framing"
#endif
#if !ARDUINO_USB_MODE || !ARDUINO_USB_CDC_ON_BOOT
#error "USB CAN logger requires Hardware CDC and JTAG with CDC on boot"
#endif
#include "index_html.h"
#include "lab_fonts.h"
#include "dashboard_icon.h"
#include "vehicle_profile.h"
#include "p4_can.h"
#include "esp32-hal-hosted.h"
#include "p4_network.h"
#include "summon_state_pure.h"
#include "auto_blinker_pure.h"
#include "burst_flash_pure.h"
#include "runtime_gate_pure.h"
#include "can_task_diagnostics_pure.h"
#include "can_research_capture_pure.h"
#include "das_status_pure.h"
#include "ap_display_hold_pure.h"
#include "mode_h_pure.h"
#include "pedal_map_session_pure.h"
#include "ulc_compositor_pure.h"
#include "feature_config_migration_pure.h"
#include "can_busoff_persistence_pure.h"
#include "auto_lane_change_enable_pure.h"
#include "country_override_pure.h"
#include "nvs_keep_ble_reset_pure.h"
#include "tsl9_input_scheduler_pure.h"
#include "driver_window_lab_pure.h"
#include "driver_monitor_capture_pure.h"
#include "r79_fixed_policy_pure.h"
#include "r79_dms_composition_pure.h"
#include "tsl9_hands_on_0x399_pure.h"
#include "fixed_point_pure.h"
#include "text_format_arduino.h"
#include "json_writer_arduino.h"

#define FW_VERSION "v3.29.3"

#include "continuous_ap_fsm_pure.h"
#include "p4can_core_state.h"
#include "p4can_forward.h"
#include "can_usb_logger.h"
#include "can_research_capture.h"
#include "driver_monitor_capture.h"
#include "s3xy_ble.h"
#include "can_core.h"
#include "can_busoff_persistence.h"
#include "vehicle_logic.h"
#include "burst_flash.h"
#include "continuous_ap_runtime.h"
#include "web_api.h"
#include "can_runtime.h"
#include "can_usb_logger_task.h"

void setup() {
  bootTime = millis();
  Serial.begin(115200);
  delay(300);
  if (!p4NetworkPrepare())
    Serial.println("[P4-CAN WiFi] HOSTED_PINS_FAILED: WiFi/BLE initialization blocked");
  canUsbLoadMode();
  setCanTxAdministrativeHold(false);
  delay(100); // Boot settle retained for behavior compatibility; CAN startup is not held here

  rtcBootCount++;
#if P4CAN_SERIAL_DIAGNOSTICS
  esp_reset_reason_t reset_reason = esp_reset_reason();
  Serial.printf("\n=== P4-CAN Unified BOOT ===\n");
  Serial.printf("Reset reason: %d (%s)\n", reset_reason, resetReasonName(reset_reason));
  Serial.printf("RTC boot count: %lu\n", (unsigned long)rtcBootCount);
  if (reset_reason == ESP_RST_BROWNOUT) {
    Serial.println("WARNING: Brownout detected!");
  }
  Serial.printf("IDF version: %s\n", esp_get_idf_version());
#endif

  // NVS init. Universal v3.x never auto-erases a configured profile because of an
  // initialization error. An explicit first-Universal migration or Factory
  // Reset is the only path that erases the full NVS partition.
  esp_err_t err = nvs_flash_init();
  if (err != ESP_OK) {
    vehicleProfileNvsError = true;
    vehicleProfileSetupMode = true;
    P4CAN_SERIAL_PRINTF("NVS: Init failed %d (%s) -> SAFE SETUP MODE, no CAN/BLE\n",
                  (int)err, esp_err_to_name(err));
  } else {
    bool keepBleResetGuardActive = false;
    const bool keepBleResetGuardReadOk =
        resetNvsKeepBleGuardRead(keepBleResetGuardActive);
    const NvsKeepBleGuardBootActionPure keepBleBootAction =
        nvsKeepBleGuardBootActionPure(
            keepBleResetGuardReadOk, keepBleResetGuardActive);
    if (keepBleBootAction == NVS_KEEP_BLE_GUARD_SAFE_ERROR_PURE) {
      vehicleProfileNvsError = true;
      vehicleProfileSetupMode = true;
      P4CAN_SERIAL_PRINTLN("BLE-preserving reset guard unreadable -> SAFE SETUP MODE");
    } else if (keepBleBootAction == NVS_KEEP_BLE_GUARD_RECOVER_PURE) {
      if (!resetNvsKeepBleRecoverIfNeeded()) {
        vehicleProfileNvsError = true;
        P4CAN_SERIAL_PRINTLN("BLE-preserving reset recovery failed -> SAFE SETUP MODE");
      } else {
        P4CAN_SERIAL_PRINTLN("BLE-preserving reset recovered after interrupted reset");
      }
      vehicleProfileSetupMode = true;
    } else {
      CheckedPreferences profileProbe;
      bool universalInitialized = false;
      uint8_t storedProfile = VEHICLE_PROFILE_NONE;
      const bool profileProbeOpened =
          profileProbe.begin(VEHICLE_PROFILE_NAMESPACE, true);
      if (profileProbeOpened) {
        universalInitialized = profileProbe.getBool(VEHICLE_UNIVERSAL_INIT_KEY, false);
        storedProfile = profileProbe.getUChar(VEHICLE_PROFILE_KEY, VEHICLE_PROFILE_NONE);
        vehicleProfileMigrationNotice = profileProbe.getBool(VEHICLE_MIGRATION_NOTICE_KEY, false);
      }
      const bool profileProbeReadOk = profileProbeOpened && profileProbe.ok();
      profileProbe.end();

      if (!profileProbeReadOk) {
        vehicleProfileNvsError = true;
        vehicleProfileSetupMode = true;
        P4CAN_SERIAL_PRINTLN("Vehicle profile read failed -> SAFE SETUP MODE, preserving NVS");
      } else if (!vehicleProfileValid(storedProfile)) {
        if (!universalInitialized) {
          P4CAN_SERIAL_PRINTLN("Universal first boot: erasing previous NVS configuration...");
          if (nvs_flash_erase() == ESP_OK && nvs_flash_init() == ESP_OK &&
              vehicleProfileWriteBootstrapMarker(true)) {
            vehicleProfileMigrationNotice = true;
          } else {
            vehicleProfileNvsError = true;
            P4CAN_SERIAL_PRINTLN("Universal migration erase/re-init failed -> SAFE SETUP MODE");
          }
        }
        vehicleProfileSetupMode = true;
      } else if (!vehicleProfileLoadFromNvs()) {
        vehicleProfileSetupMode = true;
        P4CAN_SERIAL_PRINTLN("Vehicle profile invalid/incomplete -> SAFE SETUP MODE");
      }
    }
  }

  canTxBarrierMutex = xSemaphoreCreateMutex();
  if (!canTxBarrierMutex) {
    P4CAN_SERIAL_PRINTLN("CAN TX recovery barrier allocation failed! Rebooting...");
    restartP4CanSafely();
  }

  // No valid profile means fail-closed setup mode: Wi-Fi/Web/OTA/profile
  // selection only. CAN controllers, CAN tasks, recovery supervisor and BLE
  // are not initialized.
  if (vehicleProfileSetupMode || vehicleProfileNvsError) {
    P4CAN_SERIAL_PRINTF("PROFILE SETUP MODE profile=%u nvsError=%s migrationNotice=%s\n",
                  (unsigned)activeVehicleProfile, vehicleProfileNvsError ? "YES" : "NO",
                  vehicleProfileMigrationNotice ? "YES" : "NO");
    wifiApBootStart();
    BaseType_t retWeb = xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, &webTaskHandle, 0);
    if (retWeb != pdPASS) {
      P4CAN_SERIAL_PRINTF("Web task creation failed in setup mode: %d\n", retWeb);
      restartP4CanSafely();
    }
    P4CAN_SERIAL_PRINTLN("BOOT SAFE SETUP MODE");
    return;
  }

  P4CAN_SERIAL_PRINTF("Vehicle Profile=%s CAN A=%s CAN B=%s Turn=%s\n",
                vehicleProfileName(activeVehicleProfile),
                activeProfileCanAName(),
                activeProfileCanBName(),
                turnSignalVariantName(activeTurnSignalVariant));

  // Load configs. Universal v3.x starts from a full fresh NVS on first Universal boot,
  // so only the Universal schema/default interpretation is required here.
  nvsSchemaRead();
  (void)(featureConfigMigrateToSchema2() && featureConfigMigrateToSchema3());
  featureCfgLoad();
  continuousApControlLoad();
  nagCfgLoad();
  summonCfgLoad();
  ulcCfgLoadAndMigrate();
  retiredLabSpeedSettingsCleanup();
  visionControlCfgLoad();
  autoLaneChangeLabCfgLoadAndMigrate();
  countryOverrideCfgLoad();
  r79CfgLoad();
  s3xyAutoLoadConfig();
  nvsSchemaFinalize();
  canBusOffPersistenceLoad();
  if (labMenuEnabled) {
    researchCaptureInit();
    driverMonitorCaptureInit();
  } else {
    P4CAN_SERIAL_PRINTLN("CAN Research Capture: LAB OFF · resources not allocated");
    P4CAN_SERIAL_PRINTLN("Driver Monitoring Capture: LAB OFF · resources not allocated");
  }

  P4CAN_SERIAL_PRINTF("Nag mode=%u id=0x%03X torqueCount=%u enabled=%u\n",
    nagCfg.mode, nagCfg.targetId, nagCfg.torqueCount, nagCfg.enabled);
  P4CAN_SERIAL_PRINTLN("Summon Monitor=ALWAYS_ON (state/capture/priority only)");
  P4CAN_SERIAL_PRINTF("TLSSC enabled=%s highwayGate=%s (0x3FD mux0 bit38/39)\n",
                tlsscEnabled ? "true" : "false",
                tlsscHighwayGateEnabled ? "true" : "false");
  P4CAN_SERIAL_PRINTF("LAB enabled=%s ULC alcOff=%u blind=%u ulcOff=%u confirmTiming=%u legacyGate=AUTOSTEER policyGate=AP_ACTIVE\n",
                labMenuEnabled ? "ON" : "OFF",
                (unsigned)lab3f8AlcMode, (unsigned)lab3f8UlcBlindMode,
                (unsigned)lab3f8UlcOffHighwayMode, (unsigned)ulcNoConfirmTimingMode);
  P4CAN_SERIAL_PRINTF("R79 mode=%u bit19=0 bit47=%s bit18Mode=%u mode1WaitMode=%u mode1DelayMs=%u mode2Reinject=%u mode2DelayMs=%u policy=DEFAULT_ON_MANUAL_DR_SUSPEND\n",
                (unsigned)r79TransportMode, r79Hw3Active() ? "STOCK" : "1",
                (unsigned)r79Bit18Policy,
                (unsigned)r79Mode1TxWaitMode, (unsigned)r79Mode1DelayMs,
                r79Mode2ReinjectEnabled ? 1u : 0u,
                (unsigned)r79Mode2DelayMs);
  P4CAN_SERIAL_PRINTF("S3XY bluetooth=%s auto-connect=%s registry=%u/%u\n",
                s3xyBluetoothEnabled ? "ON" : "OFF",
                s3xyAutoEnabled ? "true" : "false",
                (unsigned)s3xyRegisteredCount(), (unsigned)S3XY_MAX_DEVICES);

  // Board power-on is treated as the wake signal for RX.
  // Existing per-feature validity gates still control every injection/TX path.
  P4CAN_SERIAL_PRINTLN("Driver-wake power detected. Starting CAN init immediately...");

  // Match the reference's physical controller order, regardless of profile.
  bool allCanReady = true;
  for (uint8_t bus = 0; bus < P4_CAN_COUNT; ++bus) {
    const bool ready = p4CanInstall(bus, canUsbPassive(),
        bus == p4CanBusB() ? TWAI_TX_QUEUE_LEN : 32);
    allCanReady = ready && allCanReady;
    Serial.printf("[P4-CAN] CAN%u %s TWAI%u TX%d RX%d %s: %s\n", bus + 1,
        p4CanNames[bus], bus, p4CanTxPins[bus], p4CanRxPins[bus],
        canUsbPassive() ? "LISTEN_ONLY" : "NORMAL", esp_err_to_name(p4CanInitError[bus]));
  }
  nativeAReady = p4CanOnline[p4CanBusA()];
  twaiReady = p4CanOnline[p4CanBusB()];
  if (!allCanReady) restartP4CanSafely();
  canTwaiResetErrorAlertWindow();
  canInitTime = millis();
  bootCaptureMarkOnce(&bootCapCanInitDoneMs);
  if (!recoveryStartCanTasks()) restartP4CanSafely();
  bootCaptureMarkOnce(&bootCapCanTasksStartedMs);

  BaseType_t retSup = xTaskCreatePinnedToCore(canSupervisorTask, "canSup", 6144, nullptr, 3, &canSupervisorHandle, 0);
  if (retSup != pdPASS) {
    P4CAN_SERIAL_PRINTF("CAN supervisor task creation failed: %d\n", retSup);
    restartP4CanSafely();
  }

  P4CAN_SERIAL_PRINTF("[BOOT] CAN RX tasks started at %lu ms\n", (unsigned long)(millis() - bootTime));

  // Start Wi-Fi/web after the CAN receive path is live.
  wifiApBootStart();
  BaseType_t retWeb = xTaskCreatePinnedToCore(webTask, "web", 8192, nullptr, 1, &webTaskHandle, 0);
  if (retWeb != pdPASS) {
    P4CAN_SERIAL_PRINTF("Web task creation failed: %d\n", retWeb);
    restartP4CanSafely();
  }

  // S3XY BLE task runs independently on core 0. If Bluetooth Master is OFF it
  // never initializes BLEDevice at all. If ON, the mapper remains lazy with no
  // saved target and auto-initializes for saved targets or explicit commands.
  if (s3xyBluetoothEnabled) {
    BaseType_t retS3xy = xTaskCreatePinnedToCore(s3xyMapperTask, "s3xyMap", 8192, nullptr, 1, &s3xyTaskHandle, 0);
    if (retS3xy != pdPASS) {
      P4CAN_SERIAL_PRINTF("S3XY mapper task creation failed: %d\n", retS3xy);
      // Mapping is diagnostic only; do not reboot or disturb proven CAN logic.
    }
  } else {
    s3xyTaskHandle = nullptr;
    P4CAN_SERIAL_PRINTLN("S3XY: OFF · BLE task not started");
  }

  P4CAN_SERIAL_PRINTLN("BOOT OK");
  burstFlashInit();
  canUsbStartTask();
  __atomic_fetch_add(&canUsbControllerEpoch, 1U, __ATOMIC_RELAXED);
}

void loop() {
#if P4CAN_SERIAL_DIAGNOSTICS
  static unsigned long lastBeatLog = 0;
  static uint32_t loopBeat = 0;
  loopBeat++;
  const unsigned long now = millis();

  if (now - lastBeatLog >= 5000) {
    lastBeatLog = now;
    const unsigned long canAgeMs = (lastCanFrameMs == 0) ? 999999 : (now - lastCanFrameMs);
    Serial.printf(
      "[BEAT] uptime=%lu loop=%lu canBeat=%lu canRxTotal=%lu webBeat=%lu canFrames=%lu canAgeMs=%lu nativeATxOk=%lu nativeATxFail=%lu sumTxOk=%lu sumTxFail=%lu heap=%u\n",
      now / 1000,
      (unsigned long)loopBeat,
      (unsigned long)canBeat,
      (unsigned long)canRxTotal(),
      (unsigned long)webBeat,
      (unsigned long)canRxTotal(),
      canAgeMs,
      (unsigned long)nativeATxOk,
      (unsigned long)nativeATxFail,
      (unsigned long)sumTxOk,
      (unsigned long)sumTxFail,
      ESP.getFreeHeap()
    );
  }
#endif
  vTaskDelay(pdMS_TO_TICKS(1000));
}
