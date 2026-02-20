/**
 * @file HeapDiagnostics.h
 * @brief Heap baseline capture + diagnostic printing with marker-based tracking
 *
 * This module provides a marker-based system to track heap consumption at key
 * initialization stages, enabling clear separation of:
 *   - Boot costs (READY_BASELINE - BOOT_RAW)
 *   - Background init costs (FANS_CONNECTED - READY_BASELINE)
 *   - Runtime drift (RUNTIME_TICK - FANS_CONNECTED)
 *
 * Markers (one-time):
 *   BOOT_RAW        - Very beginning of setup() after Serial
 *   READY_BASELINE  - Entry to PH_RUNNING, before BLE/Fans start
 *   BLE_READY       - After BLERelayManager fully initialized
 *   FANS_CONNECTED  - After SmartConnect + Handshake complete
 *   POST_CTRL_DONE  - After POST-CONNECTION CONTROL SEQUENCE COMPLETE
 *   FIRST_WS_CLIENT - First WebSocket client connected
 *
 * Marker (continuous):
 *   RUNTIME_TICK    - Periodic (every N seconds or on significant change)
 */

#ifndef HOMEWIND_HEAP_DIAGNOSTICS_H
#define HOMEWIND_HEAP_DIAGNOSTICS_H

#include <Arduino.h>
#include "../app/Config.h"

namespace HeapDiagnostics {

// ============================================================
// Marker IDs
// ============================================================
enum class Marker : uint8_t {
  BOOT_RAW = 0,       // Very first capture in setup()
  READY_BASELINE,     // PH_RUNNING entry, before background init
  BLE_READY,          // BLERelayManager initialized
  FANS_CONNECTED,     // SmartConnect + Handshake complete
  POST_CTRL_DONE,     // Post-connection control sequence done
  FIRST_WS_CLIENT,    // First WebSocket client connected
  RUNTIME_TICK,       // Periodic runtime snapshot
  _COUNT              // Number of markers (for array sizing)
};

// ============================================================
// Heap Snapshot Structure
// ============================================================
struct HeapSnapshot {
  uint32_t free8bit;       // ESP.getFreeHeap() - 8-bit accessible heap
  uint32_t largest8bit;    // ESP.getMaxAllocHeap() - largest 8-bit block
  uint32_t freeInternal;   // Internal RAM free (MALLOC_CAP_INTERNAL)
  uint32_t largestInternal;// Internal RAM largest block
  uint32_t uptimeMs;       // millis() at capture time
  bool     captured;       // Has this marker been captured?
  
  HeapSnapshot() : free8bit(0), largest8bit(0), freeInternal(0), 
                   largestInternal(0), uptimeMs(0), captured(false) {}
};

// ============================================================
// Operational Status (always defined for API compatibility)
// ============================================================

/**
 * Operational status info for runtime tick logging
 */
struct OperationalStatus {
  uint8_t wsClientCount;        // WebSocket clients connected
  uint8_t bleSensorsConnected;  // BLE sensors currently connected
  uint8_t bleSensorsConfigured; // BLE sensors configured (total)
  uint8_t bleDiscoveryCount;    // Devices found in current/last discovery
  uint8_t fansConnected;        // Fans currently connected
  uint8_t fansConfigured;       // Fans configured (total)
  bool bleScanning;             // BLE discovery scan active
  
  OperationalStatus() 
    : wsClientCount(0), bleSensorsConnected(0), bleSensorsConfigured(0),
      bleDiscoveryCount(0), fansConnected(0), fansConfigured(0), bleScanning(false) {}
};

#if HW_ENABLE_SERIAL_COMMANDS

  // ============================================================
  // Capture Functions (one-time markers)
  // ============================================================
  
  /**
   * Capture BOOT_RAW marker - call at very start of setup() after Serial.begin()
   */
  void hwHeapDiagCaptureBoot();
  
  /**
   * Capture READY_BASELINE marker - call at PH_RUNNING entry, before background init
   */
  void hwHeapDiagCaptureReady();
  
  /**
   * Capture BLE_READY marker - call after BLERelayManager is fully initialized
   */
  void hwHeapDiagCaptureBleReady();
  
  /**
   * Capture FANS_CONNECTED marker - call after SmartConnect + Handshake completes
   */
  void hwHeapDiagCaptureFansConnected();
  
  /**
   * Capture POST_CTRL_DONE marker - call after POST-CONNECTION CONTROL SEQUENCE COMPLETE
   */
  void hwHeapDiagCapturePostCtrlDone();
  
  /**
   * Capture FIRST_WS_CLIENT marker - call on first WebSocket client connect
   */
  void hwHeapDiagCaptureFirstWs();
  
  // ============================================================
  // Runtime Tick (continuous marker)
  // ============================================================
  
  /**
   * Update RUNTIME_TICK marker - call from main loop periodically
   * Logs every HW_HEAP_DIAG_TICK_INTERVAL_MS (default 30s)
   * 
   * @param status Optional operational status for extended logging
   * @return true if tick was logged, false if skipped (interval not reached)
   */
  bool hwHeapDiagRuntimeTick(const OperationalStatus* status = nullptr);
  
  // ============================================================
  // Query Functions
  // ============================================================
  
  bool hwHeapDiagIsMarkerCaptured(Marker marker);
  const HeapSnapshot* hwHeapDiagGetSnapshot(Marker marker);
  
  // ============================================================
  // Print Functions
  // ============================================================
  
  /**
   * Print full heap diagnostic report with all markers and deltas
   */
  void hwHeapDiagPrintHeapInfo(Print& out);
  
  /**
   * Print a single marker line (compact format for runtime logging)
   */
  void hwHeapDiagPrintMarker(Print& out, Marker marker, const char* label = nullptr);
  
  /**
   * Print TICK-format heap info (same format as runtime tick, but immediate)
   * Useful for logging heap state after initialization steps
   */
  void hwHeapDiagPrintTick(const char* label = nullptr);

#else
  // ============================================================
  // No-op stubs when serial commands are disabled
  // ============================================================
  inline void hwHeapDiagCaptureBoot() {}
  inline void hwHeapDiagCaptureReady() {}
  inline void hwHeapDiagCaptureBleReady() {}
  inline void hwHeapDiagCaptureFansConnected() {}
  inline void hwHeapDiagCapturePostCtrlDone() {}
  inline void hwHeapDiagCaptureFirstWs() {}
  inline bool hwHeapDiagRuntimeTick(const OperationalStatus* = nullptr) { return false; }
  inline bool hwHeapDiagIsMarkerCaptured(Marker) { return false; }
  inline const HeapSnapshot* hwHeapDiagGetSnapshot(Marker) { return nullptr; }
  inline void hwHeapDiagPrintHeapInfo(Print&) {}
  inline void hwHeapDiagPrintMarker(Print&, Marker, const char* = nullptr) {}
  inline void hwHeapDiagPrintTick(const char* = nullptr) {}
#endif

} // namespace HeapDiagnostics

#endif // HOMEWIND_HEAP_DIAGNOSTICS_H
