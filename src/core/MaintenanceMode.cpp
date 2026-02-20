/**
 * @file MaintenanceMode.cpp
 * @brief Maintenance mode implementation
 */

#include "MaintenanceMode.h"
#include "DebugLog.h"

#if HW_ENABLE_FANS
#include "FanController.h"
extern FanController* g_fanController;
#endif

#if HW_ENABLE_BLE
#include "BLERelayManager.h"
extern BLERelayManager* g_bleRelayManager;
#endif

namespace MaintenanceMode {
  static bool active = false;
  static MaintenanceReason currentReason = UNKNOWN;
  
  void begin(MaintenanceReason reason) {
    // Idempotency: if already active, return immediately
    if (active) {
      return;
    }
    
    // Set internal maintenance active flag (v0 behavior)
    active = true;
    currentReason = reason;
    
    // Log single GEN message (v0 behavior)
    HW_DEBUG_PRINTF("[MaintenanceMode] Begin: %s\n", reasonString());
    
    // Call stop primitives (v1 behavior)
#if HW_ENABLE_FANS
    if (g_fanController) {
      g_fanController->stopForMaintenance();
    }
#endif

#if HW_ENABLE_BLE
    if (g_bleRelayManager) {
      g_bleRelayManager->stopForMaintenance();
    }
#endif
  }
  
  void end() {
    if (active) {
      HW_DEBUG_PRINTF("[MaintenanceMode] End: %s\n", reasonString());
    }
    active = false;
    currentReason = UNKNOWN;
  }
  
  bool isActive() {
    return active;
  }
  
  MaintenanceReason reason() {
    return currentReason;
  }
  
  const char* reasonString() {
    switch (currentReason) {
      case OTA:
        return "ota";
      case FACTORY_RESET:
        return "factory_reset";
      case UNKNOWN:
      default:
        return "unknown";
    }
  }
}

