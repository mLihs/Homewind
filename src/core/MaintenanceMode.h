/**
 * @file MaintenanceMode.h
 * @brief One-way maintenance lifecycle (design §7.4)
 * 
 * Maintenance mode blocks all non-whitelisted API endpoints with 503 responses.
 * Once OTA download, factory reset or manual maintenance is admitted, BLE work,
 * discovery, reconnects and fan control stay stopped until reboot.
 *
 * Admission (request*), abort and readers may run on any task; loop() runs the
 * stop sequence on the loop task before BLERelayManager::loop() and the fan loop.
 */

#ifndef HOMEWIND_MAINTENANCE_MODE_H
#define HOMEWIND_MAINTENANCE_MODE_H

#include <Arduino.h>
#include <atomic>
#include "MaintenanceLifecycle.h"

/**
 * @namespace MaintenanceMode
 * @brief Global maintenance mode state management
 */
namespace MaintenanceMode {
  /**
   * Admit maintenance. Same reason while not IDLE → ALREADY (success, no-op);
   * a different reason → CONFLICT (409).
   */
  hw::MaintAdmit request(hw::MaintReason reason);

  /**
   * OTA download admission: lifecycle IDLE→REQUESTED and firmware
   * READY_TO_UPDATE→QUIESCING in one critical section.
   */
  hw::MaintAdmit requestDownload(std::atomic<uint8_t>& fwPhase);

  /** Download job could not be enqueued after admission: FAILED + restart_required. */
  void failAdmission();

  /** Firmware abort: QUIESCING→ABORTED only. */
  bool abortQuiescing(std::atomic<uint8_t>& fwPhase);

  /** OTA worker: wait for any terminal result (never called with a lock held). */
  void waitForAck(uint32_t timeoutMs);

  /** OTA worker: true only on ACK and a successful QUIESCING→DOWNLOADING CAS. */
  bool workerMayDownload(std::atomic<uint8_t>& fwPhase);

  /**
   * Documented no-op: maintenance is one-way until reboot.
   * @return always false (HTTP 409)
   */
  bool end();
  
  /** Lifecycle ≠ IDLE (gate for non-whitelisted endpoints). */
  bool isActive();

  /** BLE/fan queues and deferred inits reject from REQUESTED on. */
  bool admissionClosed();
  
  hw::MaintReason reason();
  
  /**
   * Get current maintenance reason as string
   * @return "ota", "factory_reset", "manual" or "none"
   */
  const char* reasonString();
  const char* stateString();
  const char* resultString();

  bool restartRequired();
  void setRestartRequired();

  /** Reboot from loop() after a 250 ms grace (HTTP handler replies first). */
  void requestRestart();

  /** WiFi reset from loop(); false (409) unless the lifecycle is IDLE. */
  bool requestWifiReset();

  /** App wiring (captureless functions; nullptr = not available). */
  void setWifiResetHook(void (*hook)());
  void setFactoryResetHooks(bool (*clearSettings)(), bool (*clearWifi)());

  /** Read and clear the RTC factory-reset report once at boot. */
  void captureBootReport();

  /** Factory reset report from the previous boot (attempted, failedMask bit0 settings, bit1 WiFi). */
  bool factoryResetReport(uint32_t& attempted, uint32_t& failedMask);

  /** Loop task, before BLERelayManager::loop() and the fan loop. */
  void loop();
}

#endif // HOMEWIND_MAINTENANCE_MODE_H
