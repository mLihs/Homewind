/**
 * @file FanController.cpp
 * @brief Fan Controller implementation - SmartMiFanAsync integration
 */

#include "../app/Config.h"

#if HW_ENABLE_FANS

#include "FanController.h"
#include "../core/DebugLog.h"
#include "../core/HeapDiagnostics.h"
#include "../core/Crc16.h"
#include "../core/TextUtil.h"  // For ipToBuffer (no heap allocation)
#if HW_ENABLE_BLE
#include "../core/FanHRAdapter.h"
extern FanHRAdapter* g_fanHrAdapter;
#endif

// Include SystemState.h BEFORE SmartMiFanAsync.h to avoid name conflict
// (Homewind uses namespace SystemState, SmartMiFanAsync uses enum class SystemState)
#include "../core/SystemState.h"

// Workaround for SystemState name conflict:
// Temporarily rename SmartMiFanAsync's SystemState enum to avoid conflict
#define SystemState SmartMiFanAsync_SystemState
#include <SmartMiFanAsync.h>
#undef SystemState


#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
#endif

#if HW_ENABLE_DISPLAY
#include "DisplayManager.h"
#endif

// Note: g_fanController is defined in App.cpp, not here
// This file only uses it via extern declaration when needed

FanController::FanController()
  : initialized(false)
  , _maintenanceStopped(false)
  , _postConnectSpeed(HW_FAN_POST_CONNECT_SPEED)
#if HOMEWIND_ENABLE_WEBSOCKET
  , _telemetry(nullptr)
#endif
  , _fanCount(0)
  , _discoveryInProgress(false)
  , _discoveryToken("")
  , _discoveryStartTime(0)
  , _discoveryTokenPtr(nullptr)
  , _discoveryIsRecovery(false)
  , _discoveryRecoveryNvsIndex(-1)
  , _smartConnectInProgress(false)
  , _smartConnectStartTime(0)
  , _postConnectStep(PostConnectStep::IDLE)
  , _postConnectStepStartTime(0)
{
  // g_fanController is set by App.cpp when begin() is called
  memset(_fans, 0, sizeof(_fans));
  memset(_discoveryTokenStorage, 0, sizeof(_discoveryTokenStorage));
  memset(_lastControlStateChangeMs, 0, sizeof(_lastControlStateChangeMs));
  memset(_lastRecoveryDiscoveryAttemptMs, 0, sizeof(_lastRecoveryDiscoveryAttemptMs));
  memset(_autoRecoveryAttempts, 0, sizeof(_autoRecoveryAttempts));
  memset(_autoRecoveryExhausted, 0, sizeof(_autoRecoveryExhausted));
  memset(_autoRecoveryActive, 0, sizeof(_autoRecoveryActive));
  memset(_lastAutoRecoveryAttemptMs, 0, sizeof(_lastAutoRecoveryAttemptMs));
  memset(_unhealthyStreak, 0, sizeof(_unhealthyStreak));
  memset(_softActive, 0, sizeof(_softActive));
}

FanController::~FanController() {
  // g_fanController cleanup is handled by App.cpp
  // No heap allocations to clean up (deterministic storage)
}

#if HOMEWIND_ENABLE_WEBSOCKET
bool FanController::begin(WebSocketTelemetry* telemetry) {
#else
bool FanController::begin() {
#endif
  if (initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[Fans] Initializing Fan Controller (deterministic storage)");
  
#if HOMEWIND_ENABLE_WEBSOCKET
  _telemetry = telemetry;
#endif
  
  // Initialize UDP for SmartMiFanAsync
  _fanUdp.begin(0);
  
  // Reset discovered fans
  SmartMiFanAsync_resetDiscoveredFans();
  
  // Load fans from NVS
  HW_DEBUG_PRINTLN("[Fans] → Loading fans from NVS...");
  if (!loadFansFromNVS()) {
    HW_DEBUG_PRINTLN("[Fans] → No fans found in NVS, starting fresh");
    _fanCount = 0;
  } else {
    HW_DEBUG_PRINTF("[Fans] ✓ Loaded %u fan(s) from NVS\n", _fanCount);
  }
  
  // Start Smart Connect if we have fans
  if (_fanCount > 0) {
    HW_DEBUG_PRINTLN("[Fans] → Starting Smart Connect for loaded fans...");
    startSmartConnect();
  } else {
    HW_DEBUG_PRINTLN("[Fans] → No fans to connect, skipping Smart Connect");
  }
  
  initialized = true;
  HW_DEBUG_PRINTLN("[Fans] Fan Controller initialized");

  markTelemetryDirty();

  return true;
}

void FanController::loop() {
  if (!initialized || _maintenanceStopped) {
    return;
  }
  
  // Update discovery state machine - check library state directly (like BasicAsyncDiscovery example)
  // This ensures discovery updates even if _discoveryInProgress flag gets out of sync
  if (SmartMiFanAsync_isDiscoveryInProgress()) {
    updateDiscovery();
    _discoveryInProgress = true;  // Keep flag in sync with library state
  } else if (_discoveryInProgress) {
    // Discovery ended but flag wasn't cleared - sync it
    _discoveryInProgress = false;
    markTelemetryDirty();
  }
  
  // Update Smart Connect state machine
  if (_smartConnectInProgress) {
    updateSmartConnect();
  }
  
  // Update post-connection control state machine (non-blocking)
  if (_postConnectStep != PostConnectStep::IDLE) {
    updatePostConnectionControl();
  }

  // Auto-recovery: monitor ERROR fans and attempt recovery in background
  updateAutoRecovery();
}

void FanController::stopForMaintenance() {
  if (_maintenanceStopped) {
    return;
  }
  
  _maintenanceStopped = true;
  
  // Cancel any ongoing discovery or smart connect
  if (_discoveryInProgress) {
    SmartMiFanAsync_cancelDiscovery();
    _discoveryInProgress = false;
  }
  
  if (_smartConnectInProgress) {
    SmartMiFanAsync_cancelSmartConnect();
    _smartConnectInProgress = false;
  }
  
  // Prepare SmartMiFanAsync for sleep
  SmartMiFanAsync_prepareForSleep(true, true);
  
  HW_DEBUG_PRINTLN("[Fans] Stopped for maintenance");
}

bool FanController::addFan(const String& token) {
  HW_DEBUG_PRINTF("[Fans] ===== ADD FAN START: token=%s =====\n", token.c_str());
  
  if (token.length() != 32) {
    HW_ERROR_PRINTF("[Fans] ✗ Invalid token length: %u (expected 32)\n", token.length());
    return false;
  }
  
  if (_fanCount >= MAX_FANS) {
    HW_ERROR_PRINTF("[Fans] ✗ Maximum fans reached (%u/%u)\n", _fanCount, MAX_FANS);
    return false;
  }
  
  // Check if fan already exists
  int existingIndex = findFanIndex(token);
  if (existingIndex >= 0) {
    HW_DEBUG_PRINTF("[Fans] ✓ Fan already exists at index %d\n", existingIndex);
    return true;  // Idempotent
  }
  
  HW_DEBUG_PRINTF("[Fans] → Adding fan to memory (index %u)\n", _fanCount);
  
  // Add fan with default values
  FanConfig& fan = _fans[_fanCount];
  fan.setToken(token);
  fan.ip = IPAddress(0, 0, 0, 0);
  fan.model[0] = '\0';
  fan.enabled = true;  // New fans are enabled by default
  _fanCount++;
  
  HW_DEBUG_PRINTF("[Fans] → Fan added to memory: token=%s, ip=0.0.0.0, did=0, model=\"\"\n", token.c_str());
  HW_DEBUG_PRINTF("[Fans] → Current fan count: %u\n", _fanCount);
  
  // Save to NVS
  HW_DEBUG_PRINTLN("[Fans] → Writing to NVS...");
  if (!saveFansToNVS()) {
    HW_ERROR_PRINTLN("[Fans] ✗ Failed to save fan to NVS - rolling back");
    _fanCount--;  // Rollback
    return false;
  }
  
  HW_DEBUG_PRINTF("[Fans] ✓ Fan saved to NVS successfully: %s\n", token.c_str());
  HW_DEBUG_PRINTF("[Fans] → Starting discovery for token: %s\n", token.c_str());
  
  // Start discovery for this fan
  _discoveryIsRecovery = false;
  _discoveryRecoveryNvsIndex = -1;
  startDiscoveryForToken(token);
  
  // Mark telemetry dirty
  markTelemetryDirty();
  
  HW_DEBUG_PRINTLN("[Fans] ===== ADD FAN COMPLETE =====");
  
  return true;
}

bool FanController::removeFan(const String& token) {
  // For backward compatibility, call fast removal then cleanup
  if (!removeFanFast(token)) {
    return false;
  }
  removeFanCleanup(token);
  return true;
}

bool FanController::removeFanFast(const String& token) {
  int index = findFanIndex(token);
  if (index < 0) {
    HW_DEBUG_PRINTLN("[Fans] Fan not found");
    return false;
  }
  
  // Remove from array FIRST (fast operation)
  for (int i = index; i < _fanCount - 1; i++) {
    _fans[i] = _fans[i + 1];
  }
  _fanCount--;
  
  // NOTE: We do NOT call SmartMiFanAsync_resetDiscoveredFans() here because:
  // 1. It would clear ALL discovered fans, not just the one being deleted
  // 2. Other configured fans would lose their discovered state
  // 3. The UI should filter based on _fans array (which we've already updated)
  // 4. The discovered list will naturally sync on next Smart Connect
  // The deleted fan will simply not appear in the UI because it's not in _fans anymore
  
  // Send telemetry update IMMEDIATELY (before slow operations)
  // This gives instant UI feedback - fan card disappears right away
  markTelemetryDirty();
#if HOMEWIND_ENABLE_WEBSOCKET
  extern WebSocketTelemetry* g_webSocketTelemetry;
  if (g_webSocketTelemetry) {
    g_webSocketTelemetry->flushFrame(TelemetryFrameType::FANS_SNAPSHOT);
  }
#endif
  
  HW_DEBUG_PRINTF("[Fans] Removed fan from memory: %s\n", token.c_str());
  
  return true;
}

void FanController::removeFanCleanup(const String& token) {
  // Find fan in discovered fans (needed for fan control)
  int discoveredIndex = findDiscoveredFanIndex(token);
  bool fanWasDiscovered = (discoveredIndex >= 0);
  
  // Control the specific fan (if it was discovered)
  // This happens after UI update, so it doesn't block the response
  if (fanWasDiscovered) {
    HW_DEBUG_PRINTF("[Fans] → Controlling fan at discovered index %d...\n", discoveredIndex);
    
    size_t count = 0;
    const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
    
    if (discoveredIndex >= 0 && discoveredIndex < (int)count) {
      const auto& fan = discovered[discoveredIndex];
      
      // Prepare context for this specific fan using the global SmartMiFanAsync instance
      SmartMiFanAsync.attachUdp(_fanUdp);
      if (SmartMiFanAsync.setTokenFromHex(fan.token)) {
        SmartMiFanAsync.setFanAddress(fan.ip);
        SmartMiFanAsync.setModel(fan.model);
        
        // Set speed to 0 (blocking, but happens after UI update)
        HW_DEBUG_PRINTLN("[Fans] → Setting speed to 0...");
        SmartMiFanAsync.setSpeed(0);
        
        // Turn off (blocking, but happens after UI update)
        HW_DEBUG_PRINTLN("[Fans] → Turning off...");
        SmartMiFanAsync.setPower(false);
        
        HW_DEBUG_PRINTLN("[Fans] ✓ Fan controlled");
      } else {
        HW_DEBUG_PRINTLN("[Fans] ✗ Failed to set fan token for control");
      }
    }
  }
  
  // Save to NVS (slow flash write, but happens after UI update)
  if (!saveFansToNVS()) {
    HW_ERROR_PRINTLN("[Fans] Failed to save fans to NVS after removal");
    // Don't fail the operation - fan is already removed from memory and UI updated
  }
  
  HW_DEBUG_PRINTF("[Fans] Cleanup complete for: %s\n", token.c_str());
}

bool FanController::updateFanToken(const String& oldToken, const String& newToken) {
  if (newToken.length() != 32) {
    HW_ERROR_PRINTF("[Fans] Invalid new token length: %u (expected 32)\n", newToken.length());
    return false;
  }
  
  int index = findFanIndex(oldToken);
  if (index < 0) {
    HW_ERROR_PRINTLN("[Fans] Old fan not found");
    return false;
  }
  
  // Check if new token already exists
  if (findFanIndex(newToken) >= 0 && oldToken != newToken) {
    HW_ERROR_PRINTLN("[Fans] New token already exists");
    return false;
  }
  
  // Update token
  _fans[index].setToken(newToken);
  _fans[index].ip = IPAddress(0, 0, 0, 0);
  _fans[index].model[0] = '\0';
  
  // Save to NVS
  if (!saveFansToNVS()) {
    HW_ERROR_PRINTLN("[Fans] Failed to save fan to NVS after update");
    return false;
  }
  
  HW_DEBUG_PRINTF("[Fans] Updated fan token: %s -> %s\n", oldToken.c_str(), newToken.c_str());
  
  // Start discovery for new token
  _discoveryIsRecovery = false;
  _discoveryRecoveryNvsIndex = -1;
  startDiscoveryForToken(newToken);
  
  // Mark telemetry dirty
  markTelemetryDirty();
  
  return true;
}

bool FanController::setFanControlState(const String& token, bool active) {
  // 1. Find fan in NVS config
  int nvsIndex = findFanIndex(token);
  if (nvsIndex < 0) {
    HW_DEBUG_PRINTF("[Fans] setFanControlState: Fan %s not found in NVS config\n", token.c_str());
    return false;
  }

  // If auto-recovery is currently running for this fan, UI enable clicks should have no effect.
  // We reject the enable request so the UI toggle reverts immediately.
  if (active && nvsIndex < MAX_FANS && _autoRecoveryActive[nvsIndex] && !_autoRecoveryExhausted[nvsIndex]) {
    HW_DEBUG_PRINTF("[Fans] Fan %s enable requested but auto-recovery is active - rejecting UI request\n", token.c_str());
    return false;
  }

  // If user disables a fan, stop any auto-recovery for it (user intent wins)
  if (!active && nvsIndex < MAX_FANS) {
    _autoRecoveryActive[nvsIndex] = false;
    _autoRecoveryExhausted[nvsIndex] = false;
    _autoRecoveryAttempts[nvsIndex] = 0;
    _lastAutoRecoveryAttemptMs[nvsIndex] = 0;
    _lastRecoveryDiscoveryAttemptMs[nvsIndex] = 0;
    _unhealthyStreak[nvsIndex] = 0;
  }
  
  // 2. Check throttling - prevent rapid toggle (500ms minimum between changes)
  if (nvsIndex < MAX_FANS) {
    unsigned long now = millis();
    unsigned long lastChange = _lastControlStateChangeMs[nvsIndex];
    unsigned long elapsed = now - lastChange;
    
    // Only throttle if there was a previous change (lastChange != 0)
    if (lastChange != 0 && elapsed < FAN_CONTROL_THROTTLE_MS) {
      HW_DEBUG_PRINTF("[Fans] Throttled: fan %s state change too fast (%lu ms < %lu ms)\n", 
                     token.c_str(), elapsed, FAN_CONTROL_THROTTLE_MS);
      return false;  // Reject - too fast
    }
    _lastControlStateChangeMs[nvsIndex] = now;
  }
  
  // 3. Find fan in discovered list (for runtime control)
  int discoveredIndex = findDiscoveredFanIndex(token);
  if (discoveredIndex < 0) {
    // Fan not yet discovered/connected - update NVS only
    HW_DEBUG_PRINTF("[Fans] Fan %s not yet discovered - updating NVS state only\n", token.c_str());
    _fans[nvsIndex].enabled = active;
    saveFansToNVS();
    markTelemetryDirty();

    // Recovery: if user enables a fan that is not discovered, start a token-specific discovery.
    // This matches the intended flow: enable → (if ERROR/not connected) → discovery → handshake → UI recovers.
    if (active) {
      // If auto-recovery was exhausted, UI click can trigger a stronger recovery: Smart Connect
      if (nvsIndex < MAX_FANS && _autoRecoveryExhausted[nvsIndex]) {
        if (!_smartConnectInProgress && !SmartMiFanAsync_isSmartConnectInProgress()) {
          HW_DEBUG_PRINTF("[Fans] Fan %s enable requested after auto-recovery exhausted - starting Smart Connect\n",
                          token.c_str());
          startSmartConnect();
        } else {
          HW_DEBUG_PRINTF("[Fans] Fan %s enable requested after auto-recovery exhausted - Smart Connect already running\n",
                          token.c_str());
        }
        return true;
      }

      unsigned long now = millis();
      unsigned long last = _lastRecoveryDiscoveryAttemptMs[nvsIndex];
      unsigned long elapsed = now - last;
      if (last == 0 || elapsed >= FAN_RECOVERY_DISCOVERY_THROTTLE_MS) {
        _lastRecoveryDiscoveryAttemptMs[nvsIndex] = now;
        if (SmartMiFanAsync_isSmartConnectInProgress()) {
          HW_DEBUG_PRINTF("[Fans] Fan %s enabled but not discovered - smart connect in progress, skipping recovery discovery\n",
                          token.c_str());
        } else {
          HW_DEBUG_PRINTF("[Fans] Fan %s enabled but not discovered - starting recovery discovery\n", token.c_str());
          _discoveryIsRecovery = true;
          _discoveryRecoveryNvsIndex = nvsIndex;
          startDiscoveryForToken(token);
        }
      } else {
        HW_DEBUG_PRINTF("[Fans] Recovery discovery throttled for fan %s (%lu ms < %lu ms)\n",
                        token.c_str(), elapsed, FAN_RECOVERY_DISCOVERY_THROTTLE_MS);
      }
    }

    return true;  // Success - NVS updated, runtime state will sync when fan connects
  }
  
  // 3. If disabling: Turn off the fan FIRST (before marking inactive)
  if (!active) {
    size_t count = 0;
    const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
    
    if (discoveredIndex < (int)count) {
      const auto& fan = discovered[discoveredIndex];
      
      // Prepare context for this specific fan
      SmartMiFanAsync.attachUdp(_fanUdp);
      if (SmartMiFanAsync.setTokenFromHex(fan.token)) {
        SmartMiFanAsync.setFanAddress(fan.ip);
        SmartMiFanAsync.setModel(fan.model);
        
        // Turn off the fan
        if (SmartMiFanAsync.setPower(false)) {
          HW_DEBUG_PRINTF("[Fans] Fan %s powered OFF before deactivation\n", token.c_str());
        } else {
          HW_DEBUG_PRINTF("[Fans] Warning: Failed to power OFF fan %s (continuing anyway)\n", token.c_str());
          // Continue anyway - fan will be excluded from future commands
        }
      }
    }
  }
  
  // 4. Update SmartMiFanAsync state (Single Source of Truth for runtime)
  SmartMiFanAsync_setFanEnabled(discoveredIndex, active);
  HW_DEBUG_PRINTF("[Fans] Set fan control state: %s -> %s\n", token.c_str(), active ? "ACTIVE" : "INACTIVE");
  
  // 4a. If enabling a fan in ERROR state, attempt handshake to reconnect
  if (active) {
    size_t count = 0;
    const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
    
    if (discoveredIndex >= 0 && discoveredIndex < (int)count) {
      const auto& fan = discovered[discoveredIndex];
      FanParticipationState state = SmartMiFanAsync_getFanParticipationState(static_cast<uint8_t>(discoveredIndex));
      
      // Check if fan is in ERROR state (not ready or has error)
      if (state == FanParticipationState::ERROR || !fan.ready || fan.lastError != MiioErr::OK) {
        HW_DEBUG_PRINTF("[Fans] Fan %s is in ERROR state (ready=%s, error=%d) - attempting handshake to reconnect\n",
                       token.c_str(), fan.ready ? "true" : "false", static_cast<int>(fan.lastError));
        
        // Prepare context for this specific fan
        SmartMiFanAsync.attachUdp(_fanUdp);
        if (SmartMiFanAsync.setTokenFromHex(fan.token)) {
          SmartMiFanAsync.setFanAddress(fan.ip);
          SmartMiFanAsync.setModel(fan.model);
          
          // Attempt handshake with 3 second timeout
          bool handshakeSuccess = SmartMiFanAsync.handshake(3000);
          if (handshakeSuccess) {
            HW_DEBUG_PRINTF("[Fans] ✓ Handshake successful - fan %s reconnected\n", token.c_str());
            if (nvsIndex >= 0 && nvsIndex < MAX_FANS) {
              // Treat as healthy: clear any pending auto-recovery state
              _autoRecoveryActive[nvsIndex] = false;
              _autoRecoveryExhausted[nvsIndex] = false;
              _autoRecoveryAttempts[nvsIndex] = 0;
              _lastAutoRecoveryAttemptMs[nvsIndex] = 0;
              _lastRecoveryDiscoveryAttemptMs[nvsIndex] = 0;
              _unhealthyStreak[nvsIndex] = 0;
            }
          } else {
            HW_DEBUG_PRINTF("[Fans] ✗ Handshake failed - fan %s still in ERROR state\n", token.c_str());

            // Recovery: if handshake fails, retry by running token-specific discovery (IP/model may have changed).
            // Throttled to avoid repeatedly canceling/starting discovery on flaky networks.
            if (nvsIndex >= 0 && nvsIndex < MAX_FANS) {
              // If auto-recovery was exhausted, prefer Smart Connect on user click
              if (_autoRecoveryExhausted[nvsIndex]) {
                if (!_smartConnectInProgress && !SmartMiFanAsync_isSmartConnectInProgress()) {
                  HW_DEBUG_PRINTF("[Fans] Handshake failed and auto-recovery exhausted - starting Smart Connect for fan %s\n",
                                  token.c_str());
                  startSmartConnect();
                }
                // Don't start per-token discovery here
              } else {
              unsigned long now = millis();
              unsigned long last = _lastRecoveryDiscoveryAttemptMs[nvsIndex];
              unsigned long elapsed = now - last;
              if (last == 0 || elapsed >= FAN_RECOVERY_DISCOVERY_THROTTLE_MS) {
                _lastRecoveryDiscoveryAttemptMs[nvsIndex] = now;
                if (SmartMiFanAsync_isSmartConnectInProgress()) {
                  HW_DEBUG_PRINTF("[Fans] Handshake failed - smart connect in progress, skipping recovery discovery for fan %s\n",
                                  token.c_str());
                } else {
                  HW_DEBUG_PRINTF("[Fans] Handshake failed - starting recovery discovery for fan %s\n", token.c_str());
                  _discoveryIsRecovery = true;
                  _discoveryRecoveryNvsIndex = nvsIndex;
                  startDiscoveryForToken(token);
                }
              } else {
                HW_DEBUG_PRINTF("[Fans] Recovery discovery throttled for fan %s (%lu ms < %lu ms)\n",
                                token.c_str(), elapsed, FAN_RECOVERY_DISCOVERY_THROTTLE_MS);
              }
              }
            }
          }
        } else {
          HW_DEBUG_PRINTF("[Fans] ✗ Failed to set token for handshake - fan %s\n", token.c_str());
        }
      }
    }
  }
  
  // 5. Update NVS for persistence across reboots (reuse nvsIndex from throttle check)
  if (nvsIndex >= 0) {
    _fans[nvsIndex].enabled = active;
    if (saveFansToNVS()) {
      HW_DEBUG_PRINTF("[Fans] NVS updated: fan %s enabled=%s\n", token.c_str(), active ? "true" : "false");
    } else {
      HW_ERROR_PRINTLN("[Fans] Failed to save enabled state to NVS");
      // Don't fail the operation - runtime state is already updated
    }
  } else {
    HW_DEBUG_PRINTF("[Fans] Fan %s not found in NVS config, skipping NVS update\n", token.c_str());
  }
  
  // 6. Mark telemetry dirty
  markTelemetryDirty();
  
  return true;
}

bool FanController::setFanSpeed(const String& token, uint8_t speed) {
  int discoveredIndex = findDiscoveredFanIndex(token);
  if (discoveredIndex < 0) {
    HW_DEBUG_PRINTF("[Fans] Fan not found in discovered fans: %s\n", token.c_str());
    return false;
  }
  
  size_t count = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
  
  if (discoveredIndex >= 0 && discoveredIndex < (int)count) {
    const auto& fan = discovered[discoveredIndex];
    
    // Prepare context for this specific fan using the global SmartMiFanAsync instance
    SmartMiFanAsync.attachUdp(_fanUdp);
    if (SmartMiFanAsync.setTokenFromHex(fan.token)) {
      SmartMiFanAsync.setFanAddress(fan.ip);
      SmartMiFanAsync.setModel(fan.model);
      
      HW_DEBUG_PRINTF("[Fans] Setting speed for fan %s to %u%%\n", token.c_str(), speed);
      SmartMiFanAsync.setSpeed(speed);
      
      // Mark telemetry dirty
      markTelemetryDirty();
      
      return true;
    } else {
      HW_DEBUG_PRINTLN("[Fans] ✗ Failed to set fan token for speed control");
      return false;
    }
  }
  
  HW_DEBUG_PRINTF("[Fans] ✗ Invalid discovered index: %d (count: %zu)\n", discoveredIndex, count);
  return false;
}

bool FanController::setFanPower(const String& token, bool on) {
  int discoveredIndex = findDiscoveredFanIndex(token);
  if (discoveredIndex < 0) {
    HW_DEBUG_PRINTF("[Fans] Fan not found in discovered fans: %s\n", token.c_str());
    return false;
  }
  
  size_t count = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
  
  if (discoveredIndex >= 0 && discoveredIndex < (int)count) {
    const auto& fan = discovered[discoveredIndex];
    
    // Prepare context for this specific fan using the global SmartMiFanAsync instance
    SmartMiFanAsync.attachUdp(_fanUdp);
    if (SmartMiFanAsync.setTokenFromHex(fan.token)) {
      SmartMiFanAsync.setFanAddress(fan.ip);
      SmartMiFanAsync.setModel(fan.model);
      
      HW_DEBUG_PRINTF("[Fans] Setting power for fan %s to %s\n", token.c_str(), on ? "ON" : "OFF");
      SmartMiFanAsync.setPower(on);
      
      // Mark telemetry dirty
      markTelemetryDirty();
      
      return true;
    } else {
      HW_DEBUG_PRINTLN("[Fans] ✗ Failed to set fan token for power control");
      return false;
    }
  }
  
  HW_DEBUG_PRINTF("[Fans] ✗ Invalid discovered index: %d (count: %zu)\n", discoveredIndex, count);
  return false;
}

uint8_t FanController::getConnectedFanCount() const {
  size_t discoveredCount = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(discoveredCount);
  
  uint8_t connectedCount = 0;
  for (size_t i = 0; i < discoveredCount; i++) {
    if (discovered[i].ready) {
      connectedCount++;
    }
  }
  return connectedCount;
}

bool FanController::getFanConfig(uint8_t index, FanConfig& config) const {
  if (index >= _fanCount) {
    return false;
  }
  
  config = _fans[index];
  return true;
}

bool FanController::getFanConfigByToken(const String& token, FanConfig& config) const {
  int index = findFanIndex(token);
  if (index < 0) {
    return false;
  }
  
  config = _fans[index];
  return true;
}

bool FanController::getDiscoveredFans(void* fans, size_t& count) const {
  if (!fans) {
    return false;
  }
  
  SmartMiFanDiscoveredDevice* outFans = static_cast<SmartMiFanDiscoveredDevice*>(fans);
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
  if (!discovered || count == 0) {
    count = 0;
    return true;
  }
  
  // Copy up to MAX_FANS
  size_t copyCount = (count > MAX_FANS) ? MAX_FANS : count;
  memcpy(outFans, discovered, copyCount * sizeof(SmartMiFanDiscoveredDevice));
  count = copyCount;
  
  return true;
}

void FanController::markTelemetryDirty() {
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::FANS_SNAPSHOT);
  }
#endif
#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->markFansDirty();
  }
#endif
}

uint8_t FanController::getFanRecoveryUiState(uint8_t index) const {
  if (index >= MAX_FANS) return 0;
  if (_autoRecoveryExhausted[index]) return 2;
  if (_autoRecoveryActive[index]) return 1;
  return 0;
}

// Private methods

/**
 * @brief Load fan configurations from NVS (deterministic struct-based storage)
 * 
 * Uses binary blob format with CRC16 validation. Falls back to legacy
 * NVSConfigBus format migration if new format not found.
 * Zero heap allocation during load.
 */
bool FanController::loadFansFromNVS() {
  HW_DEBUG_PRINTLN("[Fans] [NVS] loadFansFromNVS() called (deterministic storage)");
  
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {  // Read-only mode
    HW_DEBUG_PRINTLN("[Fans] [NVS] Failed to open NVS namespace");
    return false;
  }
  
  // Try to load new blob format first
  NVSFanData data;
  size_t len = prefs.getBytesLength(NVS_KEY_DATA);
  
  if (len == sizeof(NVSFanData)) {
    // New format exists
    prefs.getBytes(NVS_KEY_DATA, &data, sizeof(data));
    prefs.end();
    
    // Verify CRC
    uint16_t storedCrc = data.crc16;
    data.crc16 = 0;
    uint16_t calcedCrc = Crc16::calcCRC16((uint8_t*)&data, sizeof(data) - sizeof(data.crc16));
    
    if (storedCrc != calcedCrc) {
      HW_ERROR_PRINTF("[Fans] [NVS] CRC mismatch: stored=%04X, calculated=%04X\n", 
                      storedCrc, calcedCrc);
      return false;
    }
    
    // Check version
    if (data.version != NVS_DATA_VERSION) {
      HW_DEBUG_PRINTF("[Fans] [NVS] Version mismatch: stored=%u, expected=%u\n",
                      data.version, NVS_DATA_VERSION);
      // Future: handle migrations between versions here
      return false;
    }
    
    // Validate fan count
    if (data.fanCount > MAX_FANS) {
      HW_ERROR_PRINTF("[Fans] [NVS] Invalid fanCount: %u (max=%u)\n", data.fanCount, MAX_FANS);
      return false;
    }
    
    // Copy data to runtime structures
    _fanCount = data.fanCount;
    _postConnectSpeed = data.postConnectSpeed;
    
    // Clamp post-connect speed
    if (_postConnectSpeed < 1) _postConnectSpeed = 1;
    if (_postConnectSpeed > HW_FAN_MAX_SPEED_PERCENT) _postConnectSpeed = HW_FAN_MAX_SPEED_PERCENT;
    
    for (uint8_t i = 0; i < _fanCount; i++) {
      const NVSFanEntry& entry = data.fans[i];
      FanConfig& cfg = _fans[i];
      
      // Token
      cfg.setToken(entry.token);
      
      // IP address
      cfg.ip = IPAddress(entry.ip[0], entry.ip[1], entry.ip[2], entry.ip[3]);
      
      // Model
      strncpy(cfg.model, entry.model, sizeof(cfg.model) - 1);
      cfg.model[sizeof(cfg.model) - 1] = '\0';
      
      // Enabled state
      cfg.enabled = (entry.enabled != 0);
      
      { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [NVS] → Loaded fan %u: token=%s, ip=%s, model=\"%s\", enabled=%s\n",
                     i, cfg.token, ipToBuffer(cfg.ip, ipBuf, sizeof(ipBuf)), cfg.model,
                     cfg.enabled ? "true" : "false"); }
    }
    
    HW_DEBUG_PRINTF("[Fans] [NVS] ✓ Loaded %u fan(s) from blob, postConnectSpeed=%u%%\n", 
                    _fanCount, _postConnectSpeed);
    return _fanCount > 0 || true;  // Success even with 0 fans (just no fans configured)
  }
  
  prefs.end();
  
  HW_DEBUG_PRINTLN("[Fans] [NVS] No config found in NVS, using defaults");
  _postConnectSpeed = HW_FAN_POST_CONNECT_SPEED;
  return false;
}

/**
 * @brief Save fan configurations to NVS (deterministic struct-based storage)
 * 
 * Uses binary blob format with CRC16 for data integrity.
 * Zero heap allocation during save.
 */
bool FanController::saveFansToNVS() {
  HW_DEBUG_PRINTF("[Fans] [NVS] saveFansToNVS() called with %u fans (deterministic storage)\n", _fanCount);
  
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {  // Read-write mode
    HW_ERROR_PRINTLN("[Fans] [NVS] Failed to open NVS namespace for writing");
    return false;
  }
  
  // Prepare data structure
  NVSFanData data;
  memset(&data, 0, sizeof(data));
  data.version = NVS_DATA_VERSION;
  data.fanCount = _fanCount;
  data.postConnectSpeed = _postConnectSpeed;
  
  for (uint8_t i = 0; i < _fanCount; i++) {
    const FanConfig& cfg = _fans[i];
    NVSFanEntry& entry = data.fans[i];
    
    // Token
    strncpy(entry.token, cfg.token, sizeof(entry.token) - 1);
    entry.token[sizeof(entry.token) - 1] = '\0';
    
    // IP address
    entry.ip[0] = cfg.ip[0];
    entry.ip[1] = cfg.ip[1];
    entry.ip[2] = cfg.ip[2];
    entry.ip[3] = cfg.ip[3];
    
    // Model
    strncpy(entry.model, cfg.model, sizeof(entry.model) - 1);
    entry.model[sizeof(entry.model) - 1] = '\0';
    
    // Enabled state
    entry.enabled = cfg.enabled ? 1 : 0;
    
    { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [NVS] → Fan %u: token=%s, ip=%s, model=\"%s\", enabled=%s\n", 
                   i, cfg.token, ipToBuffer(cfg.ip, ipBuf, sizeof(ipBuf)), 
                   cfg.model, cfg.enabled ? "true" : "false"); }
  }
  
  // Calculate CRC
  data.crc16 = 0;
  data.crc16 = Crc16::calcCRC16((uint8_t*)&data, sizeof(data) - sizeof(data.crc16));
  
  // Write blob
  size_t written = prefs.putBytes(NVS_KEY_DATA, &data, sizeof(data));
  prefs.end();
  
  if (written != sizeof(data)) {
    HW_ERROR_PRINTF("[Fans] [NVS] ✗ Failed to write NVS: wrote %u, expected %u\n",
                    written, sizeof(data));
    return false;
  }
  
  HW_DEBUG_PRINTF("[Fans] [NVS] ✓ Saved %u fan(s) to NVS blob, crc=%04X\n", _fanCount, data.crc16);
  return true;
}


int FanController::findFanIndex(const String& token) const {
  for (int i = 0; i < _fanCount; i++) {
    if (_fans[i].tokenEquals(token)) {
      return i;
    }
  }
  return -1;
}

void FanController::startDiscoveryForToken(const String& token) {
  HW_DEBUG_PRINTF("[Fans] [DISCOVERY] ===== START DISCOVERY: token=%s =====\n", token.c_str());
  
  // Check and cancel any existing discovery (non-blocking)
  if (_discoveryInProgress) {
    HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] → Discovery already in progress, cancelling previous");
    SmartMiFanAsync_cancelDiscovery();
    _discoveryInProgress = false;
    // Note: No delay needed - state will be checked immediately after cancel
  }
  
  // Check library discovery state - must be IDLE to start new discovery
  DiscoveryState libState = SmartMiFanAsync_getDiscoveryState();
  if (libState != DiscoveryState::IDLE) {
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Library discovery state is not IDLE (state: %d), canceling...\n", (int)libState);
    SmartMiFanAsync_cancelDiscovery();
    // Note: No delay - SmartMiFanAsync_cancelDiscovery() is synchronous and should reset state immediately
    
    // Verify state is now IDLE
    libState = SmartMiFanAsync_getDiscoveryState();
    if (libState != DiscoveryState::IDLE) {
      HW_ERROR_PRINTF("[Fans] [DISCOVERY] ✗ Library discovery state still not IDLE after cancel (state: %d)\n", (int)libState);
      _discoveryInProgress = false;
      return;
    }
    HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] → Previous discovery canceled, state reset to IDLE");
  }
  
  // Check if Fast Connect is enabled - it blocks discovery
  if (SmartMiFanAsync_isFastConnectEnabled()) {
    HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] → Fast Connect is enabled, temporarily disabling for discovery...");
    SmartMiFanAsync_setFastConnectEnabled(false);
  }
  
  // Validate token
  if (token.length() != 32) {
    HW_ERROR_PRINTF("[Fans] [DISCOVERY] ✗ Invalid token length: %u (expected 32)\n", token.length());
    _discoveryInProgress = false;
    return;
  }
  
  _discoveryToken = token;
  _discoveryStartTime = millis();
  
  HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Starting async discovery (timeout: %u ms)\n", HW_FAN_DISCOVERY_TIMEOUT_MS);
  
  // Use class member storage to ensure token pointer remains valid for entire discovery
  // String.c_str() pointers can become invalid if the String is modified or destroyed
  // Class member storage persists for the entire discovery duration, even if function is called again
  // IMPORTANT: The tokens array must also be a class member, not a local variable,
  // because the library stores a pointer to this array and accesses it later during QUERYING_DEVICES
  token.toCharArray(_discoveryTokenStorage, 33);
  _discoveryTokenPtr = _discoveryTokenStorage;
  _discoveryTokensArray[0] = _discoveryTokenPtr;  // Update persistent array
  
  if (SmartMiFanAsync_startDiscovery(_fanUdp, _discoveryTokensArray, 1, HW_FAN_DISCOVERY_TIMEOUT_MS)) {
    _discoveryInProgress = true;
    DiscoveryState initialState = SmartMiFanAsync_getDiscoveryState();
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] ✓ Discovery started for token: %s\n", token.c_str());
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Initial discovery state: %d\n", (int)initialState);
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Discovery will run in background, checking in loop()\n");
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Make sure FanController::loop() is being called regularly!\n");
  } else {
    // Provide detailed error diagnostics
    libState = SmartMiFanAsync_getDiscoveryState();
    bool fastConnectEnabled = SmartMiFanAsync_isFastConnectEnabled();
    HW_ERROR_PRINTF("[Fans] [DISCOVERY] ✗ Failed to start discovery\n");
    HW_ERROR_PRINTF("[Fans] [DISCOVERY]   - Discovery state: %d\n", (int)libState);
    HW_ERROR_PRINTF("[Fans] [DISCOVERY]   - Fast Connect enabled: %s\n", fastConnectEnabled ? "yes" : "no");
    HW_ERROR_PRINTF("[Fans] [DISCOVERY]   - Token length: %u\n", token.length());
    _discoveryInProgress = false;
  }
}

void FanController::updateDiscovery() {
  if (!_discoveryInProgress) {
    return;
  }
  
  // Update discovery state machine
  // Note: This must be called regularly from loop() for discovery to progress
  bool updated = SmartMiFanAsync_updateDiscovery();
  
  // Get current discovery state for diagnostics
  DiscoveryState libState = SmartMiFanAsync_getDiscoveryState();
  unsigned long elapsed = millis() - _discoveryStartTime;
  
  // Log state changes (only once per state to avoid spam, plus periodic updates)
  static DiscoveryState lastLoggedState = DiscoveryState::IDLE;
  static unsigned long lastStateLogTime = 0;
  unsigned long now = millis();
  bool shouldLog = (libState != lastLoggedState) || 
                   (lastStateLogTime > 0 && now - lastStateLogTime > 1000); // Log every 1 second
  
  if (shouldLog) {
    const char* stateName = "UNKNOWN";
    switch (libState) {
      case DiscoveryState::IDLE: stateName = "IDLE"; break;
      case DiscoveryState::SENDING_HELLO: stateName = "SENDING_HELLO"; break;
      case DiscoveryState::COLLECTING_CANDIDATES: stateName = "COLLECTING_CANDIDATES"; break;
      case DiscoveryState::QUERYING_DEVICES: stateName = "QUERYING_DEVICES"; break;
      case DiscoveryState::COMPLETE: stateName = "COMPLETE"; break;
      case DiscoveryState::ERROR: stateName = "ERROR"; break;
      case DiscoveryState::TIMEOUT: stateName = "TIMEOUT"; break;
    }
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] State: %s (elapsed: %lu ms, updated: %s)\n", 
                   stateName, elapsed, updated ? "yes" : "no");
    lastLoggedState = libState;
    lastStateLogTime = now;
  }
  
  // Check if complete
  if (SmartMiFanAsync_isDiscoveryComplete()) {
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Discovery completed after %lu ms\n", elapsed);
    onDiscoveryComplete();
  } else if (!SmartMiFanAsync_isDiscoveryInProgress()) {
    // Discovery failed or timed out
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] ✗ Discovery failed or timed out after %lu ms (state: %d)\n", 
                   elapsed, (int)libState);
    _discoveryInProgress = false;
    markTelemetryDirty();
  }
}

void FanController::onDiscoveryComplete() {
  _discoveryInProgress = false;
  
  HW_DEBUG_PRINTF("[Fans] [DISCOVERY] ===== DISCOVERY COMPLETE: token=%s =====\n", _discoveryToken.c_str());
  
  // Get discovered fans
  size_t count = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
  
  HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Found %zu total discovered device(s)\n", count);
  
  // Find matching token and update fan config
  bool found = false;
  for (size_t i = 0; i < count; i++) {
    { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [DISCOVERY] → Checking device %zu: token=%s, ip=%s, model=%s\n",
                   i, discovered[i].token, ipToBuffer(discovered[i].ip, ipBuf, sizeof(ipBuf)), 
                   discovered[i].model); }
    
    if (strcasecmp(discovered[i].token, _discoveryToken.c_str()) == 0) {
      HW_DEBUG_PRINTF("[Fans] [DISCOVERY] ✓ Token match found! Updating fan config...\n");
      updateFanFromDiscovered(discovered[i]);
      found = true;
      break;
    }
  }
  
  if (!found) {
    HW_DEBUG_PRINTF("[Fans] [DISCOVERY] ✗ Token %s not found in discovery results\n", _discoveryToken.c_str());
    HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] → Keeping fan in NVS with ip: 0.0.0.0 (will retry later)");
    // Keep fan in NVS with ip: 0.0.0.0 (will retry later)
    // Mark as ERROR state (handled by participation state)
  }
  
  // Sync enabled states from NVS to SmartMiFanAsync (for newly discovered fan)
  if (found) {
    HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] → Syncing enabled states from NVS to SmartMiFanAsync...");
    syncEnabledStatesToSmartMiFanAsync();
  }
  
  // Perform handshake with discovered fan
  bool handshakeResult = false;
  if (found) {
    HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] → Performing handshake with discovered fan...");
    handshakeResult = SmartMiFanAsync_handshakeAll();
    if (handshakeResult) {
      HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] ✓ Handshake successful - fan is now connected");
      // Start non-blocking control sequence after successful connection,
      // but skip it for recovery (resume HR control directly).
      if (!_discoveryIsRecovery) {
        startPostConnectionControl();
      } else {
#if HW_ENABLE_BLE
        if (g_fanHrAdapter) {
          g_fanHrAdapter->onPostConnectControlComplete();
        }
#endif
      }
    } else {
      HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] ✗ Handshake failed");
    }
  }

  // If this discovery was triggered as part of recovery, update recovery bookkeeping.
  if (_discoveryIsRecovery && _discoveryRecoveryNvsIndex >= 0 && _discoveryRecoveryNvsIndex < MAX_FANS) {
    int idx = _discoveryRecoveryNvsIndex;
    if (found && handshakeResult) {
      HW_DEBUG_PRINTF("[Fans] [RECOVERY] ✓ Recovery succeeded for fan %s (resetting attempts)\n", _fans[idx].token);
      _autoRecoveryActive[idx] = false;
      _autoRecoveryExhausted[idx] = false;
      _autoRecoveryAttempts[idx] = 0;
      _lastAutoRecoveryAttemptMs[idx] = 0;
      _lastRecoveryDiscoveryAttemptMs[idx] = 0;
      _unhealthyStreak[idx] = 0;
    } else {
      // Keep active; attempts were already counted when started from auto mode.
      HW_DEBUG_PRINTF("[Fans] [RECOVERY] ✗ Recovery discovery completed but fan still not healthy (found=%s, handshake=%s)\n",
                      found ? "true" : "false", handshakeResult ? "true" : "false");
      _autoRecoveryActive[idx] = true;
      if (_autoRecoveryAttempts[idx] >= HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS) {
        _autoRecoveryExhausted[idx] = true;
        _autoRecoveryActive[idx] = false;
        HW_DEBUG_PRINTF("[Fans] [RECOVERY] → Auto-recovery exhausted for fan %s (needs user action)\n", _fans[idx].token);
      }
    }
  }

  // Clear discovery context
  _discoveryIsRecovery = false;
  _discoveryRecoveryNvsIndex = -1;
  
  markTelemetryDirty();
  HW_DEBUG_PRINTLN("[Fans] [DISCOVERY] ===== DISCOVERY COMPLETE END =====");
}

void FanController::startSmartConnect() {
  HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ===== START SMART CONNECT =====");
  
  if (_fanCount == 0) {
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] → No fans to connect, skipping");
    return;
  }
  
  HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Configuring Smart Connect for %u fan(s)\n", _fanCount);
  
  // Configure Fast Connect with fans from NVS
  // Use static storage to ensure pointers remain valid
  // String.c_str() and toString().c_str() pointers can become invalid
  static char ipStorage[MAX_FANS][16];    // IP address string (max 15 chars + null)
  static char tokenStorage[MAX_FANS][33]; // Token string (32 chars + null)
  static char modelStorage[MAX_FANS][24]; // Model string (max 23 chars + null)
  SmartMiFanFastConnectEntry fastConnectFans[MAX_FANS];
  uint8_t fastConnectCount = 0;
  
  for (uint8_t i = 0; i < _fanCount; i++) {
    // Add ALL fans to Fast Connect config, including those with IP 0.0.0.0
    // Fans with invalid/unknown IP will fail validation and automatically use Discovery
    // This is the correct approach per SmartMiFanAsync library design
    // OPTIMIZATION: Use ipToBuffer instead of ip.toString().toCharArray() - no heap allocation
    ipToBuffer(_fans[i].ip, ipStorage[fastConnectCount], 16);
    strncpy(tokenStorage[fastConnectCount], _fans[i].token, 33);
    fastConnectFans[fastConnectCount].ipStr = ipStorage[fastConnectCount];
    fastConnectFans[fastConnectCount].tokenHex = tokenStorage[fastConnectCount];
    
    // Model: if known, pass it to skip queryInfo (faster); otherwise nullptr triggers queryInfo
    if (_fans[i].model[0] != '\0') {
      strncpy(modelStorage[fastConnectCount], _fans[i].model, sizeof(modelStorage[0]) - 1);
      modelStorage[fastConnectCount][sizeof(modelStorage[0]) - 1] = '\0';
      fastConnectFans[fastConnectCount].model = modelStorage[fastConnectCount];
    } else {
      fastConnectFans[fastConnectCount].model = nullptr;  // Library will queryInfo
    }
    
    bool hasValidIp = _fans[i].ip != IPAddress(0, 0, 0, 0);
    HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Fan %u: token=%s, ip=%s%s, model=%s\n",
                   fastConnectCount, _fans[i].token, ipStorage[fastConnectCount], 
                   hasValidIp ? "" : " (will use Discovery)",
                   _fans[i].model[0] ? _fans[i].model : "(will query)");
    fastConnectCount++;
  }
  
  // Set Fast Connect config - all fans are included (even with IP 0.0.0.0)
  // Fans with invalid IP will fail validation → Discovery will find correct IP
  HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Setting Fast Connect config for %u fan(s)\n", fastConnectCount);
  SmartMiFanAsync_setFastConnectConfig(fastConnectFans, fastConnectCount);
  
  // Start Smart Connect
  _smartConnectStartTime = millis();
  HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Starting Smart Connect (timeout: %u ms)\n", HW_FAN_SMART_CONNECT_TIMEOUT_MS);
  
  if (SmartMiFanAsync_startSmartConnect(_fanUdp, HW_FAN_SMART_CONNECT_TIMEOUT_MS)) {
    _smartConnectInProgress = true;
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ✓ Smart Connect started");
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] → Smart Connect will run in background, checking in loop()");
  } else {
    HW_ERROR_PRINTLN("[Fans] [SMART_CONNECT] ✗ Failed to start Smart Connect");
    _smartConnectInProgress = false;
  }
}

void FanController::updateSmartConnect() {
  if (!_smartConnectInProgress) {
    return;
  }
  
  // Update Smart Connect state machine
  SmartMiFanAsync_updateSmartConnect();
  
  // Check if complete
  if (SmartMiFanAsync_isSmartConnectComplete()) {
    unsigned long elapsed = millis() - _smartConnectStartTime;
    HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Smart Connect completed after %lu ms\n", elapsed);
    onSmartConnectComplete();
  } else if (!SmartMiFanAsync_isSmartConnectInProgress()) {
    // Smart Connect failed
    unsigned long elapsed = millis() - _smartConnectStartTime;
    HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] ✗ Smart Connect failed after %lu ms\n", elapsed);
    _smartConnectInProgress = false;
    markTelemetryDirty();
  }
}

void FanController::onSmartConnectComplete() {
  _smartConnectInProgress = false;
  
  unsigned long elapsed = millis() - _smartConnectStartTime;
  HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ===== SMART CONNECT COMPLETE =====");
  HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Completed in %lu ms\n", elapsed);
  
  // Get discovered fans
  size_t count = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
  
  HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Found %zu total discovered device(s)\n", count);
  
  // Update NVS with discovered info
  bool nvsUpdated = false;
  for (size_t i = 0; i < count; i++) {
    { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Checking device %zu: token=%s, ip=%s, model=%s\n",
                   i, discovered[i].token, ipToBuffer(discovered[i].ip, ipBuf, sizeof(ipBuf)), 
                   discovered[i].model); }
    
    // Find matching fan by token
    for (uint8_t j = 0; j < _fanCount; j++) {
      if (strcasecmp(discovered[i].token, _fans[j].token) == 0) {
        HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Token match found for fan %u\n", j);
        
        // Update if info changed (IP or model)
        if (_fans[j].ip != discovered[i].ip || 
            strcmp(_fans[j].model, discovered[i].model) != 0) {
          { char ipBuf1[16], ipBuf2[16]; HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Updating fan %u: ip %s->%s, model \"%s\"->\"%s\"\n",
                         j, ipToBuffer(_fans[j].ip, ipBuf1, sizeof(ipBuf1)), ipToBuffer(discovered[i].ip, ipBuf2, sizeof(ipBuf2)),
                         _fans[j].model, discovered[i].model); }
          
          _fans[j].ip = discovered[i].ip;
          strncpy(_fans[j].model, discovered[i].model, sizeof(_fans[j].model) - 1);
          _fans[j].model[sizeof(_fans[j].model) - 1] = '\0';
          nvsUpdated = true;
        } else {
          HW_DEBUG_PRINTF("[Fans] [SMART_CONNECT] → Fan %u info unchanged, skipping NVS update\n", j);
        }
        break;
      }
    }
  }
  
  // Save updated info to NVS
  if (nvsUpdated) {
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] → Updating NVS with discovered fan info...");
    if (saveFansToNVS()) {
      HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ✓ NVS updated successfully");
    } else {
      HW_ERROR_PRINTLN("[Fans] [SMART_CONNECT] ✗ Failed to update NVS");
    }
  } else {
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] → No fan info changed, skipping NVS update");
  }
  
  // Sync enabled states from NVS to SmartMiFanAsync (Single Source of Truth)
  HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] → Syncing enabled states from NVS to SmartMiFanAsync...");
  syncEnabledStatesToSmartMiFanAsync();
  
  // Perform handshake with all fans
  HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] → Performing handshake with all fans...");
  bool handshakeResult = SmartMiFanAsync_handshakeAll();
  if (handshakeResult) {
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ✓ Handshake successful - all fans connected");
    HeapDiagnostics::hwHeapDiagCaptureFansConnected();
    // Start non-blocking control sequence after successful connection
    startPostConnectionControl();
  } else {
    HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ✗ Some fans failed handshake");
  }
  
  markTelemetryDirty();
  HW_DEBUG_PRINTLN("[Fans] [SMART_CONNECT] ===== SMART CONNECT COMPLETE END =====");
}

void FanController::updateFanFromDiscovered(const SmartMiFanDiscoveredDevice& discovered) {
  HW_DEBUG_PRINTF("[Fans] [UPDATE] ===== UPDATE FAN FROM DISCOVERY: token=%s =====\n", discovered.token);
  { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [UPDATE] → Discovered info: ip=%s, model=%s\n",
                 ipToBuffer(discovered.ip, ipBuf, sizeof(ipBuf)), discovered.model); }
  
  // Find fan by token
  bool found = false;
  for (uint8_t i = 0; i < _fanCount; i++) {
    if (strcasecmp(discovered.token, _fans[i].token) == 0) {
      found = true;
      HW_DEBUG_PRINTF("[Fans] [UPDATE] → Found fan at index %u\n", i);
      { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [UPDATE] → Current: ip=%s, model=\"%s\"\n",
                     ipToBuffer(_fans[i].ip, ipBuf, sizeof(ipBuf)), _fans[i].model); }
      
      // Update fan config
      _fans[i].ip = discovered.ip;
      strncpy(_fans[i].model, discovered.model, sizeof(_fans[i].model) - 1);
      _fans[i].model[sizeof(_fans[i].model) - 1] = '\0';
      
      { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [UPDATE] → Updated: ip=%s, model=\"%s\"\n",
                     ipToBuffer(_fans[i].ip, ipBuf, sizeof(ipBuf)), _fans[i].model); }
      
      // Save to NVS
      HW_DEBUG_PRINTLN("[Fans] [UPDATE] → Saving updated fan info to NVS...");
      if (saveFansToNVS()) {
        { char ipBuf[16]; HW_DEBUG_PRINTF("[Fans] [UPDATE] ✓ Fan updated and saved to NVS: %s -> %s (model: %s)\n", 
                       discovered.token, ipToBuffer(discovered.ip, ipBuf, sizeof(ipBuf)), discovered.model); }
      } else {
        HW_ERROR_PRINTLN("[Fans] [UPDATE] ✗ Failed to save updated fan to NVS");
      }
      break;
    }
  }
  
  if (!found) {
    HW_ERROR_PRINTF("[Fans] [UPDATE] ✗ Fan not found for token: %s\n", discovered.token);
  }
  
  HW_DEBUG_PRINTLN("[Fans] [UPDATE] ===== UPDATE FAN FROM DISCOVERY END =====");
}

int FanController::findDiscoveredFanIndex(const String& token) const {
  size_t count = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(count);
  
  for (size_t i = 0; i < count; i++) {
    if (strcasecmp(discovered[i].token, token.c_str()) == 0) {
      return i;
    }
  }
  
  return -1;
}

void FanController::updateAutoRecovery() {
  // Don't interfere with ongoing connection processes
  if (_maintenanceStopped || !initialized) {
    return;
  }
  if (_discoveryInProgress || SmartMiFanAsync_isDiscoveryInProgress()) {
    return;
  }
  if (_smartConnectInProgress || SmartMiFanAsync_isSmartConnectInProgress()) {
    return;
  }

  size_t discoveredCount = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(discoveredCount);
  if (!discovered || discoveredCount == 0) {
    return;
  }

  unsigned long now = millis();

  // For each discovered device, check if it's configured and unhealthy.
  for (size_t i = 0; i < discoveredCount; i++) {
    // Map discovered token -> configured fan index
    int cfgIndex = -1;
    for (uint8_t j = 0; j < _fanCount && j < MAX_FANS; j++) {
      if (strcasecmp(discovered[i].token, _fans[j].token) == 0) {
        cfgIndex = (int)j;
        break;
      }
    }
    if (cfgIndex < 0 || cfgIndex >= MAX_FANS) {
      continue;
    }

    // Only auto-recover fans that the user actually enabled (participating)
    if (!_fans[cfgIndex].enabled) {
      continue;
    }

    const auto& disc = discovered[i];
    FanParticipationState state = SmartMiFanAsync_getFanParticipationState(static_cast<uint8_t>(i));
    const bool isHealthy = disc.ready && (disc.lastError == MiioErr::OK);

    if (isHealthy) {
      // Clear recovery state when fan becomes healthy again
      if (_autoRecoveryAttempts[cfgIndex] || _autoRecoveryExhausted[cfgIndex] || _autoRecoveryActive[cfgIndex]) {
        HW_DEBUG_PRINTF("[Fans] [RECOVERY] Fan %s is healthy again - clearing recovery state\n", _fans[cfgIndex].token);
      }
      if (i < MAX_FANS) {
        _softActive[i] = false;
      }
      _autoRecoveryActive[cfgIndex] = false;
      _autoRecoveryExhausted[cfgIndex] = false;
      _autoRecoveryAttempts[cfgIndex] = 0;
      _lastAutoRecoveryAttemptMs[cfgIndex] = 0;
      _lastRecoveryDiscoveryAttemptMs[cfgIndex] = 0;
      _unhealthyStreak[cfgIndex] = 0;
      continue;
    }

    // Fan is in ERROR-ish condition
    const uint8_t prevStreak = _unhealthyStreak[cfgIndex];
    if (_unhealthyStreak[cfgIndex] < 255) _unhealthyStreak[cfgIndex]++;
    if (prevStreak == 0) {
      HW_DEBUG_PRINTF("[Fans] [RECOVERY] Unhealthy detected: fan=%s state=%d ready=%s err=%d (t=%lums)\n",
                      _fans[cfgIndex].token,
                      (int)state,
                      discovered[i].ready ? "true" : "false",
                      (int)discovered[i].lastError,
                      (unsigned long)now);
    }
    if (_unhealthyStreak[cfgIndex] == HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS) {
      HW_DEBUG_PRINTF("[Fans] [RECOVERY] Unhealthy threshold reached: fan=%s streak=%u/%u state=%d ready=%s err=%d (t=%lums)\n",
                      _fans[cfgIndex].token,
                      _unhealthyStreak[cfgIndex],
                      (uint8_t)HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS,
                      (int)state,
                      discovered[i].ready ? "true" : "false",
                      (int)discovered[i].lastError,
                      (unsigned long)now);
    }
    if (_unhealthyStreak[cfgIndex] < HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS) {
      // Soft-error phase: keep fan ACTIVE despite lastError
      if (i < MAX_FANS) {
        _softActive[i] = true;
      }
      continue;  // Debounce: require consecutive unhealthy observations
    }
    // Hard-error phase: allow ERROR state (skip commands, start recovery)
    if (i < MAX_FANS) {
      _softActive[i] = false;
    }

    if (_autoRecoveryExhausted[cfgIndex]) {
      continue;  // wait for explicit user action
    }

    // Respect retry interval
    unsigned long last = _lastAutoRecoveryAttemptMs[cfgIndex];
    unsigned long elapsed = now - last;
    if (last != 0 && elapsed < FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS) {
      continue;
    }

    if (_autoRecoveryAttempts[cfgIndex] >= HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS) {
      _autoRecoveryExhausted[cfgIndex] = true;
      _autoRecoveryActive[cfgIndex] = false;
      HW_DEBUG_PRINTF("[Fans] [RECOVERY] → Auto-recovery exhausted for fan %s (needs user action)\n", _fans[cfgIndex].token);
      continue;
    }

    // Start a recovery attempt: token-specific discovery (async) which will lead to handshakeAll() on completion.
    _autoRecoveryActive[cfgIndex] = true;
    _autoRecoveryAttempts[cfgIndex]++;
    _lastAutoRecoveryAttemptMs[cfgIndex] = now;
    _lastRecoveryDiscoveryAttemptMs[cfgIndex] = now;

    HW_DEBUG_PRINTF("[Fans] [RECOVERY] Attempt %u/%u: starting discovery for fan %s (streak=%u/%u, state=%d, ready=%s, err=%d)\n",
                    _autoRecoveryAttempts[cfgIndex], HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS,
                    _fans[cfgIndex].token,
                    _unhealthyStreak[cfgIndex], (uint8_t)HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS,
                    (int)state, discovered[i].ready ? "true" : "false", (int)discovered[i].lastError);

    _discoveryIsRecovery = true;
    _discoveryRecoveryNvsIndex = cfgIndex;
    startDiscoveryForToken(String(_fans[cfgIndex].token));
    return;  // Only run one recovery discovery at a time
  }
}

/**
 * @brief Start non-blocking post-connection control sequence
 * 
 * This replaces the old blocking performPostConnectionControl() with a
 * millis-based state machine. The sequence:
 * 1. Power on all fans
 * 2. Wait 500ms for stabilization
 * 3. Set configured speed
 * 4. Wait 3000ms
 * 5. Power off all fans
 * 
 * Call updatePostConnectionControl() from loop() to advance the state machine.
 */
void FanController::startPostConnectionControl() {
  if (_postConnectStep != PostConnectStep::IDLE) {
    HW_DEBUG_PRINTLN("[Fans] [CONTROL] Post-connect sequence already in progress");
    return;
  }
  
  HW_DEBUG_PRINTLN("[Fans] [CONTROL] ===== POST-CONNECTION CONTROL SEQUENCE START (non-blocking) =====");
  _postConnectStep = PostConnectStep::POWER_ON;
  _postConnectStepStartTime = millis();
}

/**
 * @brief Update post-connection control state machine (call from loop)
 * 
 * This is the non-blocking replacement for the old delay()-based sequence.
 * Each step transitions to the next based on elapsed time.
 */
void FanController::updatePostConnectionControl() {
  if (_postConnectStep == PostConnectStep::IDLE) {
    return;
  }
  
  unsigned long elapsed = millis() - _postConnectStepStartTime;
  
  switch (_postConnectStep) {
    case PostConnectStep::POWER_ON: {
      HW_DEBUG_PRINTLN("[Fans] [CONTROL] → Turning on all fans...");
      bool powerResult = SmartMiFanAsync_setPowerAll(true);
      if (powerResult) {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✓ Fans turned on");
        _postConnectStep = PostConnectStep::WAIT_STABILIZE;
        _postConnectStepStartTime = millis();
      } else {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✗ Failed to turn on fans - aborting sequence");
        _postConnectStep = PostConnectStep::DONE;
        markTelemetryDirty();
      }
      break;
    }
    
    case PostConnectStep::WAIT_STABILIZE:
      // Wait 500ms for fans to stabilize
      if (elapsed >= 500) {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✓ Stabilization wait complete (500ms)");
        _postConnectStep = PostConnectStep::SET_SPEED;
      }
      break;
    
    case PostConnectStep::SET_SPEED: {
      HW_DEBUG_PRINTF("[Fans] [CONTROL] → Setting speed to %u%%...\n", _postConnectSpeed);
      bool speedResult = SmartMiFanAsync_setSpeedAll(_postConnectSpeed);
      if (speedResult) {
        HW_DEBUG_PRINTF("[Fans] [CONTROL] ✓ Speed set to %u%%\n", _postConnectSpeed);
      } else {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✗ Failed to set speed (continuing anyway)");
      }
      _postConnectStep = PostConnectStep::WAIT_RUN;
      _postConnectStepStartTime = millis();
      break;
    }
    
    case PostConnectStep::WAIT_RUN:
      // Wait 3000ms for fans to run
      if (elapsed >= 3000) {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✓ Run wait complete (3000ms)");
        _postConnectStep = PostConnectStep::POWER_OFF;
      }
      break;
    
    case PostConnectStep::POWER_OFF: {
      HW_DEBUG_PRINTLN("[Fans] [CONTROL] → Turning off all fans...");
      bool powerResult = SmartMiFanAsync_setPowerAll(false);
      if (powerResult) {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✓ Fans turned off");
      } else {
        HW_DEBUG_PRINTLN("[Fans] [CONTROL] ✗ Failed to turn off fans");
      }
      _postConnectStep = PostConnectStep::DONE;
      break;
    }
    
    case PostConnectStep::DONE:
      HW_DEBUG_PRINTLN("[Fans] [CONTROL] ===== POST-CONNECTION CONTROL SEQUENCE COMPLETE =====");
      HeapDiagnostics::hwHeapDiagCapturePostCtrlDone();
      markTelemetryDirty();
#if HW_ENABLE_BLE
      if (g_fanHrAdapter) {
        g_fanHrAdapter->onPostConnectControlComplete();
      }
#endif
      _postConnectStep = PostConnectStep::IDLE;  // IDLE indicates POST_CTRL_DONE reached
      break;
    
    case PostConnectStep::IDLE:
      // Should not reach here, but handle gracefully
      break;
  }
}

bool FanController::isPostConnectDone() const {
  // POST_CTRL_DONE is reached when:
  // 1. Post-connect step is IDLE (sequence completed or never started)
  // 2. Sequence was started (_postConnectStepStartTime > 0 indicates it ran)
  // 3. Sequence completed (back to IDLE after DONE)
  // Note: _postConnectStep starts as IDLE, so we need to check if sequence ran
  if (_postConnectStep != PostConnectStep::IDLE) {
    return false;  // Sequence still running
  }
  // If IDLE and sequence was started, it means DONE was reached
  return (_postConnectStepStartTime > 0);
}

bool FanController::isFanActiveForHr(size_t discoveredIndex, const SmartMiFanDiscoveredDevice& fan) const {
  if (!fan.userEnabled) {
    return false;
  }
  if (fan.lastError == MiioErr::OK) {
    return true;
  }
  if (discoveredIndex >= MAX_FANS) {
    return false;
  }
  return _softActive[discoveredIndex];
}

void FanController::syncEnabledStatesToSmartMiFanAsync() {
  size_t discoveredCount = 0;
  const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(discoveredCount);
  
  if (discoveredCount == 0) {
    HW_DEBUG_PRINTLN("[Fans] [SYNC] No discovered fans to sync");
    return;
  }
  
  HW_DEBUG_PRINTF("[Fans] [SYNC] Syncing enabled states for %zu discovered fan(s)...\n", discoveredCount);
  
  for (size_t i = 0; i < discoveredCount; i++) {
    // Find matching fan in _fans[] by token
    bool found = false;
    for (uint8_t j = 0; j < _fanCount; j++) {
      if (strcasecmp(discovered[i].token, _fans[j].token) == 0) {
        // Set enabled state in SmartMiFanAsync from NVS-loaded value
        SmartMiFanAsync_setFanEnabled(static_cast<uint8_t>(i), _fans[j].enabled);
        HW_DEBUG_PRINTF("[Fans] [SYNC] Fan[%zu] (token=%s): enabled=%s\n", 
                       i, discovered[i].token, _fans[j].enabled ? "true" : "false");
        found = true;
        break;
      }
    }
    
    if (!found) {
      // Fan discovered but not in NVS config - default to enabled
      SmartMiFanAsync_setFanEnabled(static_cast<uint8_t>(i), true);
      HW_DEBUG_PRINTF("[Fans] [SYNC] Fan[%zu] (token=%s): not in NVS, defaulting to enabled=true\n", 
                     i, discovered[i].token);
    }
  }
  
  HW_DEBUG_PRINTLN("[Fans] [SYNC] State sync complete");
}

bool FanController::setPostConnectSpeed(uint8_t speed, bool persist) {
  // Clamp to valid range (1-100%)
  if (speed < 1) speed = 1;
  if (speed > HW_FAN_MAX_SPEED_PERCENT) speed = HW_FAN_MAX_SPEED_PERCENT;
  
  _postConnectSpeed = speed;
  HW_DEBUG_PRINTF("[Fans] Post-connect speed set to %u%%\n", _postConnectSpeed);
  
  if (persist) {
    if (saveFansToNVS()) {
      HW_DEBUG_PRINTF("[Fans] Post-connect speed %u%% saved to NVS\n", _postConnectSpeed);
    } else {
      HW_ERROR_PRINTLN("[Fans] Failed to save post-connect speed to NVS");
      return false;
    }
  }
  
  return true;
}

#endif // HW_ENABLE_FANS

