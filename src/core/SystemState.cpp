/**
 * @file SystemState.cpp
 * @brief Global system state management implementation with per-module readiness
 */

#include "SystemState.h"

namespace SystemState {
  static uint32_t bootTime = 0;
  static bool servicesReady = false;
  
#if HW_ENABLE_NVS
  static bool nvsReady = false;
#endif

#if HW_ENABLE_WIFI
  static bool wifiReady = false;      // Level A: AP or STA
  static bool stationOnline = false;  // Level B: STA connected
#endif

#if HW_ENABLE_WEB
  static bool webReady = false;
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
  static bool wsReady = false;
#endif

#if HW_ENABLE_BLE
  static bool bleReady = false;
#endif

#if HW_ENABLE_FANS
  static bool fansReady = false;
#endif
  
  void update() {
    if (bootTime == 0) {
      bootTime = millis();
    }
    
    // Services ready state is set by boot orchestrator
    // This function just updates uptime tracking
  }
  
  bool isReady() {
    // Critical services ready check:
    // - servicesReady flag (set by boot orchestrator when PH_RUNNING reached)
    // - NVS (if enabled) must be ready
    // - WiFi (if enabled) must be ready (STA connected OR AP active)
    // - Web (if enabled) must be ready
    
    if (!servicesReady) {
      return false; // Boot orchestrator hasn't reached PH_RUNNING yet
    }
    
#if HW_ENABLE_NVS
    if (!nvsReady) {
      return false; // NVS required but not ready
    }
#endif

#if HW_ENABLE_WIFI
    if (!wifiReady) {
      return false; // WiFi required but not ready (STA not connected and AP not active)
    }
#endif

#if HW_ENABLE_WEB
    if (!webReady) {
      return false; // Web server required but not ready
    }
#endif
    
    return true; // All critical services ready
  }
  
  void setServicesReady(bool ready) {
    servicesReady = ready;
  }
  
  uint32_t getUptimeMs() {
    if (bootTime == 0) return 0;
    return millis() - bootTime;
  }
  
  uint32_t getFreeHeap() {
    return ESP.getFreeHeap();
  }
  
  uint32_t getLargestFreeBlock() {
    return ESP.getMaxAllocHeap();
  }

#if HW_ENABLE_NVS
  void setNVSReady(bool ready) {
    nvsReady = ready;
  }
  
  bool isNVSReady() {
    return nvsReady;
  }
#endif

#if HW_ENABLE_WIFI
  void setWiFiReady(bool ready) {
    wifiReady = ready;
  }
  
  bool isWiFiReady() {
    return wifiReady;
  }
  
  void setStationOnline(bool online) {
    stationOnline = online;
  }
  
  bool isStationOnline() {
    return stationOnline;
  }
#endif

#if HW_ENABLE_WEB
  void setWebReady(bool ready) {
    webReady = ready;
  }
  
  bool isWebReady() {
    return webReady;
  }
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
  void setWebSocketReady(bool ready) {
    wsReady = ready;
  }
  
  bool isWebSocketReady() {
    return wsReady;
  }
#endif

#if HW_ENABLE_BLE
  void setBLEReady(bool ready) {
    bleReady = ready;
  }
  
  bool isBLEReady() {
    return bleReady;
  }
#endif

#if HW_ENABLE_FANS
  void setFansReady(bool ready) {
    fansReady = ready;
  }
  
  bool isFansReady() {
    return fansReady;
  }
#endif
}

