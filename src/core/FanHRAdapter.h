/**
 * @file FanHRAdapter.h
 * @brief HR-driven fan control orchestrator
 * 
 * Implements proportional fan control from BLE Heart Rate with:
 * - HR smoothing (moving average)
 * - Speed hysteresis (minimum change threshold)
 * - Per-fan inclusion state (ACTIVE/INACTIVE/ERROR)
 * - Deterministic priority order
 * - Status API for future display integration
 */

#ifndef HOMEWIND_FAN_HR_ADAPTER_H
#define HOMEWIND_FAN_HR_ADAPTER_H

#include "../app/Config.h"

#if HW_ENABLE_FANS && HW_ENABLE_BLE

#include <Arduino.h>

// Forward declarations
class FanController;
class HeartRateSettings;

// Compile-time constants
#define FANHR_SMOOTH_N 5              // Ring buffer size for HR smoothing
#define SPEED_DEADBAND_PERCENT 3       // Minimum speed change to trigger update (%)
#define FANHR_UPDATE_INTERVAL_MS 3000  // Default throttle interval (ms)
#define HR_PLAUSIBLE_MIN 30            // Minimum plausible HR (bpm)
#define HR_PLAUSIBLE_MAX 250           // Maximum plausible HR (bpm)

/**
 * @enum FanHrMode
 * @brief Per-fan HR control inclusion state
 */
enum class FanHrMode {
  ACTIVE = 0,   // Included in HR-based control
  INACTIVE = 1, // Excluded from HR control
  ERROR = 2     // Excluded from HR control (treated like INACTIVE)
};

/**
 * @struct FanHRStatus
 * @brief Immutable status snapshot for UI/WS/Display
 */
struct FanHRStatus {
  bool hrValid;              // HR sensor connected and valid
  uint16_t hrRaw;            // Last raw HR sample (bpm)
  uint16_t hrUsed;           // Smoothed HR used for control (bpm)
  uint16_t minHR;            // Current minHR setting
  uint16_t maxHR;            // Current maxHR setting
  bool globalTargetPower;     // Global target power state (ON/OFF)
  uint8_t globalTargetSpeedPct; // Global target speed (0-HW_FAN_MAX_SPEED_PERCENT)
  uint32_t lastUpdateMs;     // Last control update timestamp
  uint32_t lastHrMs;         // Last HR sample timestamp
  uint8_t flags;             // Status flags bitfield
};

/**
 * @class FanHRAdapter
 * @brief HR-driven fan control orchestrator
 * 
 * Handles all HR-to-fan control logic with smoothing, hysteresis,
 * and per-fan exclusion. Designed to be a minimal orchestration layer
 * that doesn't duplicate existing fan control primitives.
 */
class FanHRAdapter {
public:
  FanHRAdapter();
  ~FanHRAdapter();
  
  /**
   * Initialize adapter
   * @param fanController Fan controller instance
   * @param hrSettings Heart rate settings instance
   * @return true on success
   */
  bool begin(FanController* fanController, HeartRateSettings* hrSettings);
  
  /**
   * Check if adapter is initialized
   * @return true if initialized
   */
  bool ready() const { return _initialized; }

  /**
   * Notify adapter that post-connection control sequence completed.
   * This sequence powers fans ON → sets speed → powers OFF.
   * Reset tracking so HR-driven control resumes immediately.
   */
  void onPostConnectControlComplete();
  
  /**
   * HR input event: new sample received
   * @param hrBpm Heart rate in bpm
   * @param nowMs Current timestamp (millis())
   */
  void onHeartRateSample(uint16_t hrBpm, uint32_t nowMs);
  
  /**
   * HR input event: sensor disconnected
   * @param nowMs Current timestamp (millis())
   */
  void onHeartRateDisconnected(uint32_t nowMs);
  
  /**
   * Main control loop (call from App::loop())
   * @param nowMs Current timestamp (millis())
   */
  void loop(uint32_t nowMs);
  
  /**
   * Get current status snapshot (for UI/WS/Display)
   * @return Immutable status struct
   */
  FanHRStatus getStatus() const;
  
  /**
   * Set per-fan HR control mode (runtime only, not persisted)
   * @param fanIndex Fan index (0-based)
   * @param mode ACTIVE/INACTIVE/ERROR
   */
  void setFanHrMode(size_t fanIndex, FanHrMode mode);
  
  /**
   * Get per-fan HR control mode
   * @param fanIndex Fan index (0-based)
   * @return Current mode
   */
  FanHrMode getFanHrMode(size_t fanIndex) const;

private:
  bool _initialized;
  FanController* _fanController;
  HeartRateSettings* _hrSettings;
  
  // HR smoothing state
  uint16_t _hrBuf[FANHR_SMOOTH_N];
  uint8_t _hrCount;        // Number of valid samples in buffer
  uint8_t _hrIndex;        // Next write index (ring buffer)
  bool _hrValid;            // HR sensor connected and valid
  uint16_t _hrRaw;          // Last raw HR sample
  uint16_t _hrUsed;         // Smoothed HR used for control
  uint32_t _lastHrMs;       // Last HR sample timestamp
  
  // Control state
  uint32_t _lastControlMs;  // Last control update timestamp
  uint32_t _lastCommandSentMs; // Last time a command was actually sent (for rate limiting)
  bool _globalTargetPower;  // Global target power state
  uint8_t _globalTargetSpeedPct; // Global target speed
  bool _postConnectResetPending; // Set when post-connect sequence resets tracking
  
  // Per-fan runtime state (indexed by fan index)
  static const uint8_t MAX_FANS = 4;
  uint8_t _lastSentSpeedPct[MAX_FANS];  // Last sent speed per fan
  bool _lastSentPowerOn[MAX_FANS];       // Last sent power state per fan
  FanHrMode _fanHrModes[MAX_FANS];      // Per-fan HR mode
  
  // Max speed latch: prevents repeated max-speed commands that can cause timeouts
  // Set to true when HW_FAN_MAX_SPEED_PERCENT is successfully sent,
  // reset when speed drops below HW_FAN_MAX_SPEED_PERCENT
  bool _maxSpeedLatch;
  
  // Internal methods
  void clearHrBuffer();
  void pushHrSample(uint16_t hr);
  uint16_t computeSmoothedHr() const;
  void updateGlobalTarget();
  void applyControl(uint32_t nowMs);
  bool isPlausibleHr(uint16_t hr) const;
};

#endif // HW_ENABLE_FANS && HW_ENABLE_BLE

#endif // HOMEWIND_FAN_HR_ADAPTER_H
