/**
 * @file SystemState.h
 * @brief Global system state management with per-module readiness
 */

#ifndef HOMEWIND_SYSTEM_STATE_H
#define HOMEWIND_SYSTEM_STATE_H

#include <Arduino.h>
#include "../app/Config.h"

/**
 * @namespace SystemState
 * @brief Global system state tracking with per-module readiness gates
 */
namespace SystemState {
  /**
   * Update system state (call from main loop)
   */
  void update();
  
  /**
   * Check if all critical services are ready
   * @return true if all enabled critical services are ready
   */
  bool isReady();
  
  /**
   * Set overall services ready state
   * @param ready Ready state
   */
  void setServicesReady(bool ready);
  
  /**
   * Get uptime in milliseconds
   * @return uptime in ms
   */
  uint32_t getUptimeMs();
  
  /**
   * Get free heap in bytes
   * @return free heap
   */
  uint32_t getFreeHeap();
  
  /**
   * Get largest free block in bytes
   * @return largest free block
   */
  uint32_t getLargestFreeBlock();
  
  // Per-module readiness flags (set by boot orchestrator)
  
#if HW_ENABLE_NVS
  /**
   * Set NVS readiness
   */
  void setNVSReady(bool ready);
  
  /**
   * Check if NVS is ready
   */
  bool isNVSReady();
#endif

#if HW_ENABLE_WIFI
  /**
   * Set WiFi readiness (Level A: AP or STA)
   * True when: AP active OR STA connected
   * Enables: WebServer, Setup/Config UI
   */
  void setWiFiReady(bool ready);
  
  /**
   * Check if WiFi is ready (Level A)
   */
  bool isWiFiReady();
  
  /**
   * Set Station Online (Level B: STA connected)
   * True when: STA connected to router WiFi
   * Enables: BLE, Fans, OTA (operational features)
   */
  void setStationOnline(bool online);
  
  /**
   * Check if Station is online (Level B)
   * @return true if connected to router WiFi (not just AP mode)
   */
  bool isStationOnline();
#endif

#if HW_ENABLE_WEB
  /**
   * Set Web server readiness
   */
  void setWebReady(bool ready);
  
  /**
   * Check if Web server is ready
   */
  bool isWebReady();
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
  /**
   * Set WebSocket readiness
   */
  void setWebSocketReady(bool ready);
  
  /**
   * Check if WebSocket is ready
   */
  bool isWebSocketReady();
#endif

#if HW_ENABLE_BLE
  /**
   * Set BLE readiness
   */
  void setBLEReady(bool ready);
  
  /**
   * Check if BLE is ready
   */
  bool isBLEReady();
#endif

#if HW_ENABLE_FANS
  /**
   * Set Fan controller readiness
   */
  void setFansReady(bool ready);
  
  /**
   * Check if Fan controller is ready
   */
  bool isFansReady();
#endif
}

#endif // HOMEWIND_SYSTEM_STATE_H

