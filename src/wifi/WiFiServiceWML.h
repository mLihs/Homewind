/**
 * @file WiFiServiceWML.h
 * @brief WiFi service using wifiMangerLite with Captive Portal
 * 
 * This backend is used in Production mode (HW_DEV_MODE == 0).
 * Provides WiFi setup via Captive Portal with NVS credential storage.
 * 
 * Connectivity Levels:
 *   Level A (ready): AP active OR STA connected → Setup/UI accessible
 *   Level B (isSTA): STA connected → Operational features enabled
 */

#ifndef HOMEWIND_WIFI_SERVICE_WML_H
#define HOMEWIND_WIFI_SERVICE_WML_H

#include "../app/Config.h"

#if HW_ENABLE_WIFI && HW_WIFI_BACKEND_WML

#include <Arduino.h>
#include <wifiMangerLite.h>
#include <ESPAsyncWebServer.h>

// Forward declaration
class AsyncWebServer;

/**
 * @class WiFiServiceWML
 * @brief WiFi service using wifiMangerLite + Captive Portal
 * 
 * Same interface as WiFiService for easy swap via compile-time flags.
 */
class WiFiServiceWML {
public:
  WiFiServiceWML();
  ~WiFiServiceWML();
  
  /**
   * Initialize WiFi with wifiMangerLite
   * Must be called AFTER WebServerManager creates the AsyncWebServer
   * @param server Pointer to AsyncWebServer (from WebServerManager)
   */
  void begin(AsyncWebServer* server);
  
  /**
   * Update WiFi state (call from main loop)
   */
  void loop();
  
  /**
   * Check if WiFi is ready (Level A)
   * @return true if STA connected OR AP active
   */
  bool ready() const;
  
  /**
   * Check if currently in STA mode and connected (Level B)
   * @return true if STA connected to router
   */
  bool isSTA() const;
  
  /**
   * Check if currently in AP mode
   * @return true if AP active
   */
  bool isAP() const;
  
  /**
   * Get IP address as string
   * @return Stable c-string buffer with IP address
   */
  const char* ipString() const;
  
  /**
   * Get WiFi mode as string
   * @return Mode string ("STA", "AP", "AP+STA", or "OFF")
   */
  const char* modeString() const;
  
  /**
   * Check if Portal/Setup is currently active
   * (AP mode without stored credentials)
   */
  bool isPortalActive() const;

  /**
   * WiFi reset via WML factoryReset(callback, restart).
   * Clears credentials (NVS) and schedules restart; WML handles reboot.
   * Used by WiFi reset API. Caller must not restart the device.
   * @return true if reset was initiated
   */
  bool wifiReset();

private:
  bool initialized;
  
  // wifiMangerLite components (static allocation)
  WML::WiFiManagerLite* wifiManager;
  WML::CaptivePortal* portal;
  WML::Storage* storage;
  WML::StorageProvider* configProvider;
  
  // Static buffers for string returns
  mutable char ipBuffer[16];
  mutable char modeBuffer[8];
  
  // Track state changes for SystemState updates
  bool lastStaConnected;
  bool lastApActive;
  
  /**
   * Setup portal callbacks
   */
  void setupCallbacks();
};

#endif // HW_ENABLE_WIFI && HW_WIFI_BACKEND_WML

#endif // HOMEWIND_WIFI_SERVICE_WML_H
