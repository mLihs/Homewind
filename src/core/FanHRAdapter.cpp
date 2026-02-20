/**
 * @file FanHRAdapter.cpp
 * @brief HR-driven fan control orchestrator implementation
 */

#include "FanHRAdapter.h"
#include "../core/DebugLog.h"
#include "../core/FanController.h"
#include "../core/HeartRateSettings.h"

// Module-specific debug logging (gated by HW_DEBUG_FANHR)
// These logs are very verbose (HR samples, speed calculations) and can be disabled separately
#if HW_ENABLE_DEBUG_LOGS && HW_DEBUG_FANHR
  #define FANHR_DEBUG_PRINT(x) Serial.print(x)
  #define FANHR_DEBUG_PRINTLN(x) Serial.println(x)
  #define FANHR_DEBUG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
  #define FANHR_DEBUG_PRINT(x) ((void)0)
  #define FANHR_DEBUG_PRINTLN(x) ((void)0)
  #define FANHR_DEBUG_PRINTF(fmt, ...) ((void)0)
#endif

#if HW_ENABLE_FANS && HW_ENABLE_BLE

// Include SmartMiFanAsync for participation state
#include <SmartMiFanAsync.h>

FanHRAdapter::FanHRAdapter()
  : _initialized(false)
  , _fanController(nullptr)
  , _hrSettings(nullptr)
  , _hrCount(0)
  , _hrIndex(0)
  , _hrValid(false)
  , _hrRaw(0)
  , _hrUsed(0)
  , _lastHrMs(0)
  , _lastControlMs(0)
  , _lastCommandSentMs(0)
  , _globalTargetPower(false)
  , _globalTargetSpeedPct(0)
  , _maxSpeedLatch(false)
  , _postConnectResetPending(false)
{
  clearHrBuffer();
  memset(_lastSentSpeedPct, 0, sizeof(_lastSentSpeedPct));
  memset(_lastSentPowerOn, 0, sizeof(_lastSentPowerOn));
  // Initialize all fans to ACTIVE by default (backward compatibility)
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    _fanHrModes[i] = FanHrMode::ACTIVE;
  }
}

FanHRAdapter::~FanHRAdapter() {
  // No dynamic allocations to clean up
}

bool FanHRAdapter::begin(FanController* fanController, HeartRateSettings* hrSettings) {
  if (_initialized) {
    return true;
  }
  
  if (!fanController || !hrSettings) {
    HW_ERROR_PRINTLN("[FanHRAdapter] Invalid parameters");
    return false;
  }
  
  _fanController = fanController;
  _hrSettings = hrSettings;
  
  HW_DEBUG_PRINTLN("[FanHRAdapter] Initialized");
  _initialized = true;
  
  return true;
}

void FanHRAdapter::onHeartRateSample(uint16_t hrBpm, uint32_t nowMs) {
  if (!_initialized) {
    return;
  }
  
  // Only accept plausible HR values
  if (!isPlausibleHr(hrBpm)) {
    HW_DEBUG_PRINTF("[FanHRAdapter] Rejected implausible HR: %u bpm\n", hrBpm);
    return;
  }
  
  _hrRaw = hrBpm;
  _lastHrMs = nowMs;
  _hrValid = true;
  
  // Push to smoothing buffer
  pushHrSample(hrBpm);
  
  // Update smoothed HR
  _hrUsed = computeSmoothedHr();
  
  FANHR_DEBUG_PRINTF("[FanHRAdapter] HR sample: raw=%u, smoothed=%u\n", _hrRaw, _hrUsed);
}

void FanHRAdapter::onHeartRateDisconnected(uint32_t nowMs) {
  if (!_initialized) {
    return;
  }
  
  HW_DEBUG_PRINTLN("[FanHRAdapter] HR sensor disconnected");
  
  _hrValid = false;
  _hrRaw = 0;
  _hrUsed = 0;
  _lastHrMs = nowMs;
  
  // Clear smoothing buffer
  clearHrBuffer();
  
  // Global target becomes OFF
  _globalTargetPower = false;
  _globalTargetSpeedPct = 0;
  
  // Reset max speed latch so 100% will be sent again when HR reconnects
  _maxSpeedLatch = false;
}

void FanHRAdapter::onPostConnectControlComplete() {
  if (!_initialized) {
    return;
  }

  // Post-connection sequence turns fans OFF; reset tracking so HR control can
  // re-assert power/speed on the next update.
  for (uint8_t i = 0; i < MAX_FANS; i++) {
    _lastSentPowerOn[i] = false;
    _lastSentSpeedPct[i] = 0;
  }
  _maxSpeedLatch = false;
  _lastCommandSentMs = 0;
  _lastControlMs = 0; // allow immediate update
  _postConnectResetPending = true;

  HW_DEBUG_PRINTF("[FanHRAdapter] Post-connect control complete: reset HR tracking (t=%lums)\n",
                  (unsigned long)millis());
}

void FanHRAdapter::loop(uint32_t nowMs) {
  if (!_initialized || !_fanController || !_fanController->ready()) {
    return;
  }
  
  // OPTIMIZATION: If HR sensor not connected, skip expensive calculations
  // Only periodic fail-safe check: re-assert fans OFF every 5s (handles edge cases)
  if (!_hrValid) {
    // HR disconnected: skip updateGlobalTarget() entirely to save CPU
    // applyControl() handles:
    //   - Immediate turn-off on disconnect (state change)
    //   - Periodic fail-safe re-assert OFF every 5s (handles manual control, network issues, etc.)
    const uint32_t FAILSAFE_CHECK_INTERVAL_MS = 5000;  // Periodic fail-safe check interval
    static uint32_t lastFailsafeCheck = 0;
    if (nowMs - lastFailsafeCheck >= FAILSAFE_CHECK_INTERVAL_MS) {
      lastFailsafeCheck = nowMs;
      applyControl(nowMs);
    }
    return;
  }
  
  // HR valid: normal operation
  // Update global target from current HR state
  updateGlobalTarget();
  
  // Apply control (with throttling and hysteresis)
  applyControl(nowMs);
}

FanHRStatus FanHRAdapter::getStatus() const {
  FanHRStatus status = {};
  
  if (!_initialized) {
    return status;
  }
  
  status.hrValid = _hrValid;
  status.hrRaw = _hrRaw;
  status.hrUsed = _hrUsed;
  
  if (_hrSettings) {
    status.minHR = _hrSettings->getMin();
    status.maxHR = _hrSettings->getMax();
  } else {
    status.minHR = 80;  // Defaults
    status.maxHR = 160;
  }
  
  status.globalTargetPower = _globalTargetPower;
  status.globalTargetSpeedPct = _globalTargetSpeedPct;
  status.lastUpdateMs = _lastControlMs;
  status.lastHrMs = _lastHrMs;
  status.flags = 0;  // Reserved for future use
  
  return status;
}

void FanHRAdapter::setFanHrMode(size_t fanIndex, FanHrMode mode) {
  if (fanIndex >= MAX_FANS) {
    return;
  }
  
  _fanHrModes[fanIndex] = mode;
  HW_DEBUG_PRINTF("[FanHRAdapter] Set fan[%zu] HR mode: %d\n", fanIndex, static_cast<int>(mode));
}

FanHrMode FanHRAdapter::getFanHrMode(size_t fanIndex) const {
  if (fanIndex >= MAX_FANS) {
    return FanHrMode::INACTIVE;
  }
  
  return _fanHrModes[fanIndex];
}

void FanHRAdapter::clearHrBuffer() {
  _hrCount = 0;
  _hrIndex = 0;
  memset(_hrBuf, 0, sizeof(_hrBuf));
}

void FanHRAdapter::pushHrSample(uint16_t hr) {
  _hrBuf[_hrIndex] = hr;
  _hrIndex = (_hrIndex + 1) % FANHR_SMOOTH_N;
  
  if (_hrCount < FANHR_SMOOTH_N) {
    _hrCount++;
  }
}

uint16_t FanHRAdapter::computeSmoothedHr() const {
  if (_hrCount == 0) {
    return 0;
  }
  
  uint32_t sum = 0;
  for (uint8_t i = 0; i < _hrCount; i++) {
    sum += _hrBuf[i];
  }
  
  // Round to nearest integer
  return static_cast<uint16_t>((sum + _hrCount / 2) / _hrCount);
}

void FanHRAdapter::updateGlobalTarget() {
  if (!_hrSettings) {
    _globalTargetPower = false;
    _globalTargetSpeedPct = 0;
    static bool loggedNull = false;
    if (!loggedNull) {
      HW_DEBUG_PRINTLN("[FanHRAdapter] updateGlobalTarget: _hrSettings is null!");
      loggedNull = true;
    }
    return;
  }
  
  uint16_t minHR = _hrSettings->getMin();
  uint16_t maxHR = _hrSettings->getMax();
  
  // DEBUG: Log HR range values (only when they change to avoid spam)
  static uint16_t lastLoggedMin = 0;
  static uint16_t lastLoggedMax = 0;
  if (minHR != lastLoggedMin || maxHR != lastLoggedMax) {
    FANHR_DEBUG_PRINTF("[FanHRAdapter] Using HR range: minHR=%u, maxHR=%u\n", minHR, maxHR);
    lastLoggedMin = minHR;
    lastLoggedMax = maxHR;
  }
  
  if (!_hrValid || _hrUsed == 0) {
    // HR disconnected or invalid: OFF
    bool wasOff = (!_globalTargetPower && _globalTargetSpeedPct == 0);
    _globalTargetPower = false;
    _globalTargetSpeedPct = 0;
    if (!wasOff) {
      HW_DEBUG_PRINTLN("[FanHRAdapter] HR invalid/disconnected: OFF");
    }
    return;
  }
  
  // Track previous state to only log when it changes
  static uint16_t lastLoggedHrUsed = 0;
  static uint8_t lastLoggedSpeed = 255;
  static bool lastLoggedPower = false;
  
  if (_hrUsed < minHR) {
    // Below minimum: OFF
    bool powerChanged = (_globalTargetPower != false);
    bool speedChanged = (_globalTargetSpeedPct != 0);
    bool hrChanged = (_hrUsed != lastLoggedHrUsed);
    _globalTargetPower = false;
    _globalTargetSpeedPct = 0;
    if (powerChanged || speedChanged || hrChanged) {
      FANHR_DEBUG_PRINTF("[FanHRAdapter] HR %u < minHR %u: OFF\n", _hrUsed, minHR);
      lastLoggedHrUsed = _hrUsed;
      lastLoggedSpeed = 0;
      lastLoggedPower = false;
    }
  } else if (_hrUsed >= minHR && _hrUsed <= maxHR) {
    // In range: linear mapping 0..HW_FAN_MAX_SPEED_PERCENT
    // Map from [minHR, maxHR] to [0, HW_FAN_MAX_SPEED_PERCENT]
    int32_t speed = ((int32_t)(_hrUsed - minHR) * (int32_t)HW_FAN_MAX_SPEED_PERCENT) / (maxHR - minHR);
    if (speed < 0) speed = 0;
    if (speed > (int32_t)HW_FAN_MAX_SPEED_PERCENT) speed = (int32_t)HW_FAN_MAX_SPEED_PERCENT;
    // When power is ON, minimum speed should be 1% (fan hardware minimum)
    uint8_t newSpeed = static_cast<uint8_t>((speed == 0) ? 1 : speed);
    
    // Check for changes BEFORE updating values
    bool powerChanged = (_globalTargetPower != true);
    bool speedChanged = (newSpeed != lastLoggedSpeed);
    bool hrChanged = (_hrUsed != lastLoggedHrUsed);
    
    _globalTargetPower = true;
    _globalTargetSpeedPct = newSpeed;
    
    // Only log when values actually change
    if (powerChanged || speedChanged || hrChanged) {
      FANHR_DEBUG_PRINTF("[FanHRAdapter] HR %u in range [%u-%u]: speed = ((%u-%u)*%u)/(%u-%u) = %u%%\n",
                     _hrUsed, minHR, maxHR, _hrUsed, minHR, (unsigned)HW_FAN_MAX_SPEED_PERCENT, maxHR, minHR, _globalTargetSpeedPct);
      lastLoggedHrUsed = _hrUsed;
      lastLoggedSpeed = _globalTargetSpeedPct;
      lastLoggedPower = true;
    }
  } else {
    // Above maximum: HW_FAN_MAX_SPEED_PERCENT
    bool powerChanged = (_globalTargetPower != true);
    bool speedChanged = (_globalTargetSpeedPct != HW_FAN_MAX_SPEED_PERCENT);
    bool hrChanged = (_hrUsed != lastLoggedHrUsed);
    _globalTargetPower = true;
    _globalTargetSpeedPct = HW_FAN_MAX_SPEED_PERCENT;
    if (powerChanged || speedChanged || hrChanged) {
      FANHR_DEBUG_PRINTF("[FanHRAdapter] HR %u > maxHR %u: %u%%\n", _hrUsed, maxHR, (unsigned)HW_FAN_MAX_SPEED_PERCENT);
      lastLoggedHrUsed = _hrUsed;
      lastLoggedSpeed = HW_FAN_MAX_SPEED_PERCENT;
      lastLoggedPower = true;
    }
  }
}

void FanHRAdapter::applyControl(uint32_t nowMs) {
  // Track HR valid state changes to avoid repeated logging and command sending
  static bool lastHrValidState = true;
  static bool everHadHr = false;
  static bool reassertPending = false;
  static bool reassertSent = false;
  static uint32_t disconnectAtMs = 0;
  
  // OPTIMIZATION: If HR disconnected, handle fail-safe immediately without expensive fan discovery
  if (!_hrValid) {
    // Only enforce fail-safe OFF when we actually lost HR (was connected before).
    // This avoids constantly shutting down fans that are used in Smart Home mode
    // when Homewind is running but no HR sensor is connected.
    if (lastHrValidState != _hrValid) {
      lastHrValidState = _hrValid; // now disconnected
      if (everHadHr) {
        HW_DEBUG_PRINTLN("[FanHRAdapter] HR disconnected: turning off fans (fail-safe)");
        SmartMiFanAsync_setPowerAllOrchestrated(false); // immediate OFF
        for (size_t i = 0; i < MAX_FANS; i++) {
          _lastSentSpeedPct[i] = 0;
          _lastSentPowerOn[i] = false;
        }
        // Schedule exactly one re-assert after 10 seconds
        reassertPending = true;
        reassertSent = false;
        disconnectAtMs = nowMs;
      } else {
        // HR was never connected since boot → don't touch fans
        reassertPending = false;
        reassertSent = false;
        disconnectAtMs = 0;
      }
    } else if (reassertPending && !reassertSent) {
      const uint32_t FAILSAFE_REASSERT_DELAY_MS = 10000;
      if (nowMs - disconnectAtMs >= FAILSAFE_REASSERT_DELAY_MS) {
        SmartMiFanAsync_setPowerAllOrchestrated(false); // one-time re-assert
        reassertSent = true;
        reassertPending = false;
      }
    }
    return;
  }
  
  // Update the static state tracker when HR becomes valid again
  if (lastHrValidState != _hrValid) {
    lastHrValidState = _hrValid;
    everHadHr = true;
    reassertPending = false;
    reassertSent = false;
    disconnectAtMs = 0;
  }
  
  // HR valid: normal operation - get discovered fans (expensive operation)
  // Use SmartMiFanAsync as Single Source of Truth for fan state
  size_t discoveredCount = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(discoveredCount);
  
  if (!discovered || discoveredCount == 0) {
    // No fans discovered yet (SmartConnect still running or failed)
    // Don't log every cycle - only on state change
    static bool noFansLogged = false;
    if (!noFansLogged) {
      HW_DEBUG_PRINTLN("[FanHRAdapter] No discovered fans - waiting for SmartConnect");
      noFansLogged = true;
    }
    return;
  }
  
  // Log once when fans become available
  static bool fansReadyLogged = false;
  if (!fansReadyLogged) {
    HW_DEBUG_PRINTF("[FanHRAdapter] SmartConnect ready - %zu fan(s) discovered\n", discoveredCount);
    fansReadyLogged = true;
  }
  
  // Throttle: only update if interval elapsed
  if (nowMs - _lastControlMs < FANHR_UPDATE_INTERVAL_MS) {
    return;
  }
  
  _lastControlMs = nowMs;
  
  // Priority 2: Apply HR control to ACTIVE fans using orchestrated functions
  // SmartMiFanAsync holds the authoritative fan states (ACTIVE/INACTIVE/ERROR)
  // No need to sync - states are already set via setFanControlState() or NVS sync
  
  // Check if any ACTIVE fan needs update (hysteresis check)
  bool needsPowerUpdate = false;
  bool needsSpeedUpdate = false;
  uint8_t targetSpeed = (_globalTargetSpeedPct == 0) ? 1 : _globalTargetSpeedPct;
  
  // Track fan states for debug output
  struct FanState {
    uint8_t index;
    uint8_t lastSpeed;
    int16_t diff;
    uint8_t deadband;
  } fanStates[MAX_FANS];
  uint8_t activeFanCount = 0;
  
  for (size_t i = 0; i < discoveredCount && i < MAX_FANS; i++) {
    const auto& fan = discovered[i];
    if (!_fanController->isFanActiveForHr(i, fan)) {
      // Reset tracking for non-ACTIVE fans so they get power ON when re-activated
      // This ensures that when a fan switches from INACTIVE back to ACTIVE,
      // the HR adapter will see _lastSentPowerOn[i] == false and send power ON
      _lastSentPowerOn[i] = false;
      _lastSentSpeedPct[i] = 0;
      continue;  // Skip INACTIVE and ERROR fans
    }
    
    if (!_globalTargetPower) {
      // Target is OFF: needs power OFF
      if (_lastSentPowerOn[i]) {
        needsPowerUpdate = true;
      }
    } else {
      // Target is ON: check power and speed hysteresis
      if (!_lastSentPowerOn[i]) {
        needsPowerUpdate = true;
      }
      
      // Check hysteresis per fan
      int16_t speedDiff = static_cast<int16_t>(_globalTargetSpeedPct) - static_cast<int16_t>(_lastSentSpeedPct[i]);
      if (speedDiff < 0) speedDiff = -speedDiff;  // abs
      
      // Special case: if target is very low (0-3%), use smaller deadband (1%)
      uint8_t deadband = (_globalTargetSpeedPct <= 3 || _lastSentSpeedPct[i] <= 3) ? 1 : SPEED_DEADBAND_PERCENT;
      
      // Store fan state for debug output
      if (activeFanCount < MAX_FANS) {
        fanStates[activeFanCount].index = static_cast<uint8_t>(i);
        fanStates[activeFanCount].lastSpeed = _lastSentSpeedPct[i];
        fanStates[activeFanCount].diff = speedDiff;
        fanStates[activeFanCount].deadband = deadband;
        activeFanCount++;
      }
      
      if (speedDiff >= deadband) {
        needsSpeedUpdate = true;
        FANHR_DEBUG_PRINTF("[FanHRAdapter] Fan[%zu] needs speed update: target=%u%%, last=%u%%, diff=%d >= deadband=%u\n",
                       i, _globalTargetSpeedPct, _lastSentSpeedPct[i], speedDiff, deadband);
      }
    }
  }
  
  // Apply Power Update using orchestrated function
  if (needsPowerUpdate) {
    if (_postConnectResetPending && _globalTargetPower) {
      HW_DEBUG_PRINTF("[FanHRAdapter] HR reassert after post-connect: power ON (t=%lums, target=%u%%)\n",
                      (unsigned long)nowMs,
                      _globalTargetSpeedPct);
    }
    FANHR_DEBUG_PRINTF("[FanHRAdapter] Calling setPowerAllOrchestrated(%s)\n", _globalTargetPower ? "ON" : "OFF");
    bool result = SmartMiFanAsync_setPowerAllOrchestrated(_globalTargetPower);
    FANHR_DEBUG_PRINTF("[FanHRAdapter] Power %s (orchestrated, result=%d)\n", _globalTargetPower ? "ON" : "OFF", result);
    
    // Update tracking for all HR-active fans (includes soft-error fans)
    for (size_t i = 0; i < discoveredCount && i < MAX_FANS; i++) {
      const auto& fan = discovered[i];
      if (_fanController->isFanActiveForHr(i, fan)) {
        _lastSentPowerOn[i] = _globalTargetPower;
        if (!_globalTargetPower) {
          _lastSentSpeedPct[i] = 0;
        }
      }
    }
  }
  
  // Apply Speed Update using orchestrated function (with rate limiting)
  if (_globalTargetPower && needsSpeedUpdate) {
    // Debug counters to verify "max HR → max speed sent once"
    // These are only used for logging/diagnostics.
    static uint32_t s_maxSpeedSendCount = 0;
    static uint32_t s_maxSpeedSkipCount = 0;

    // MAX SPEED LATCH: Prevent repeated max-speed commands that can cause timeouts
    // When HR > maxHR, HW_FAN_MAX_SPEED_PERCENT is sent ONCE and latched until HR drops below maxHR
    // This prevents the timeout→ERROR→recovery→timeout loop at max speed
    if (targetSpeed == HW_FAN_MAX_SPEED_PERCENT && _maxSpeedLatch) {
      // Already at max and successfully sent - don't send again
      s_maxSpeedSkipCount++;
      HW_DEBUG_PRINTF("[FanHRAdapter] Max speed latched: SKIP #%lu (t=%lums, target=%u%%, HR=%u, smoothed=%u)\n",
                      (unsigned long)s_maxSpeedSkipCount,
                      (unsigned long)nowMs,
                      (unsigned)HW_FAN_MAX_SPEED_PERCENT,
                      (unsigned)_hrRaw,
                      (unsigned)_hrUsed);
      return;
    }
    
    // Reset latch when speed drops below max
    if (targetSpeed < HW_FAN_MAX_SPEED_PERCENT && _maxSpeedLatch) {
      _maxSpeedLatch = false;
      HW_DEBUG_PRINTF("[FanHRAdapter] Max speed latch released (speed < %u%%)\n", (unsigned)HW_FAN_MAX_SPEED_PERCENT);
    }
    
    // Check rate limiting: only send if at least 1000ms since last command
    const uint32_t MIN_COMMAND_INTERVAL_MS = 1000;
    bool canSendNow = (nowMs - _lastCommandSentMs >= MIN_COMMAND_INTERVAL_MS);
    
    if (!canSendNow) {
      FANHR_DEBUG_PRINTF("[FanHRAdapter] Rate limited: speed %u%% queued (last command %lums ago)\n",
                     targetSpeed, (unsigned long)(nowMs - _lastCommandSentMs));
      return; // Wait for next cycle
    }
    
    FANHR_DEBUG_PRINTF("[FanHRAdapter] Calling setSpeedAllOrchestrated(%u%%)\n", targetSpeed);
    bool result = SmartMiFanAsync_setSpeedAllOrchestrated(targetSpeed);
    
    if (result) {
      _lastCommandSentMs = nowMs;
      FANHR_DEBUG_PRINTF("[FanHRAdapter] Speed set to %u%% (orchestrated, HR=%u, smoothed=%u)\n",
                     targetSpeed, _hrRaw, _hrUsed);

      if (_postConnectResetPending) {
        HW_DEBUG_PRINTF("[FanHRAdapter] HR reassert after post-connect: speed %u%% sent (t=%lums)\n",
                        targetSpeed, (unsigned long)nowMs);
        _postConnectResetPending = false;
      }
      
      // Set max speed latch when HW_FAN_MAX_SPEED_PERCENT is successfully sent
      if (targetSpeed == HW_FAN_MAX_SPEED_PERCENT) {
        s_maxSpeedSendCount++;
        _maxSpeedLatch = true;
        HW_DEBUG_PRINTF("[FanHRAdapter] Max speed SEND #%lu (t=%lums, target=%u%%, HR=%u, smoothed=%u) → latch SET\n",
                        (unsigned long)s_maxSpeedSendCount,
                        (unsigned long)nowMs,
                        (unsigned)HW_FAN_MAX_SPEED_PERCENT,
                        (unsigned)_hrRaw,
                        (unsigned)_hrUsed);
      }
      
      // Update tracking for all HR-active fans (includes soft-error fans)
      for (size_t i = 0; i < discoveredCount && i < MAX_FANS; i++) {
        const auto& fan = discovered[i];
        if (_fanController->isFanActiveForHr(i, fan)) {
          _lastSentSpeedPct[i] = targetSpeed;
        }
      }
    } else {
      FANHR_DEBUG_PRINTF("[FanHRAdapter] Speed command failed: %u%% (no ACTIVE fans?)\n", targetSpeed);
      // Note: Don't set latch on failure - will retry next cycle
    }
  } else if (_globalTargetPower && !needsSpeedUpdate && activeFanCount > 0) {
    // Display fan states when speed is suppressed (only if we have active fans)
    FANHR_DEBUG_PRINTF("[FanHRAdapter] Speed suppressed (hysteresis): target=%u%%, fans[%u]: ", 
                   _globalTargetSpeedPct, activeFanCount);
    for (uint8_t j = 0; j < activeFanCount; j++) {
      if (j > 0) {
        FANHR_DEBUG_PRINT(", ");
      }
      int16_t actualDiff = static_cast<int16_t>(_globalTargetSpeedPct) - static_cast<int16_t>(fanStates[j].lastSpeed);
      FANHR_DEBUG_PRINTF("Fan[%u]=%u%%(diff=%d,db=%u)", 
                     fanStates[j].index, 
                     fanStates[j].lastSpeed,
                     actualDiff,
                     fanStates[j].deadband);
    }
    FANHR_DEBUG_PRINTF("\n");
  }
}

bool FanHRAdapter::isPlausibleHr(uint16_t hr) const {
  return (hr >= HR_PLAUSIBLE_MIN && hr <= HR_PLAUSIBLE_MAX);
}

#endif // HW_ENABLE_FANS && HW_ENABLE_BLE
