/**
 * @file WiFiService.h
 * @brief Minimal WiFi service wrapper (thin layer over ESP32 WiFi)
 * 
 * Provides a simple, non-blocking interface for WiFi connectivity:
 * - STA mode with compile-time credentials
 * - AP fallback mode on timeout (if enabled)
 * - No SettingsStore dependency
 * - No blocking loops or delays
 */

#ifndef HOMEWIND_WIFI_SERVICE_H
#define HOMEWIND_WIFI_SERVICE_H

#include <Arduino.h>
#include "../app/Config.h"

#if HW_ENABLE_WIFI && HW_WIFI_BACKEND_FIXED

#include <WiFi.h>

/**
 * @class WiFiService
 * @brief Thin WiFi wrapper for STA connection and AP fallback
 */
class WiFiService {
public:
  WiFiService();
  ~WiFiService();
  
  /**
   * Initialize WiFi (non-blocking)
   * Starts STA connection attempt using compile-time credentials
   */
  void begin();
  
  /**
   * Update WiFi state (call from main loop)
   * Monitors connection status and handles AP fallback on timeout
   */
  void loop();
  
  /**
   * Check if WiFi is ready
   * @return true if STA connected OR AP active OR degraded-allowed
   */
  bool ready() const;
  
  /**
   * Check if currently in STA mode and connected
   * @return true if STA connected
   */
  bool isSTA() const;
  
  /**
   * Check if currently in AP mode and active
   * @return true if AP active
   */
  bool isAP() const;
  
  /**
   * Get IP address as string
   * @return Stable c-string buffer with IP address (no heap allocation)
   */
  const char* ipString() const;
  
  /**
   * Get WiFi mode as string
   * @return Mode string ("STA", "AP", "AP+STA", or "OFF")
   */
  const char* modeString() const;

private:
  bool initialized;
  bool staConnected;
  bool apActive;
  unsigned long connectStartTime;
  bool connecting;
  
  // Static buffers for string returns (no heap churn)
  mutable char ipBuffer[16];  // "255.255.255.255\0"
  mutable char modeBuffer[8]; // "AP+STA\0"
  
  /**
   * Start STA connection attempt
   */
  void startSTA();
  
  /**
   * Start AP fallback mode
   */
  void startAP();
  
  /**
   * Check if connection timeout expired
   */
  bool isTimeout() const;
};

#endif // HW_ENABLE_WIFI

#endif // HOMEWIND_WIFI_SERVICE_H

