/**
 * @file ApiSettings.h
 * @brief HTTP settings API handlers (GET/POST /api/v1/settings/*)
 * 
 * Settings are persistent configuration values stored in NVS.
 * - GET /api/v1/settings/get?key=... - Get setting value
 * - POST /api/v1/settings/set?key=...&value=... - Set setting value
 * - GET /api/v1/settings/list?prefix=... - List settings with prefix
 * - Input: Query params (no JSON)
 * - Output: text/plain responses only
 */

#ifndef HOMEWIND_API_SETTINGS_H
#define HOMEWIND_API_SETTINGS_H

#include <Arduino.h>
#include "../app/Config.h"

#if HOMEWIND_ENABLE_API_SETTINGS
#if HW_ENABLE_WEB
#include <ESPAsyncWebServer.h>
#endif

class SettingsStore; // Forward declaration

/**
 * @class ApiSettings
 * @brief HTTP settings API handlers
 * 
 * All settings operations use query params (no JSON).
 * Responses are text/plain.
 */
class ApiSettings {
public:
  ApiSettings();
  
  /**
   * Register all settings routes
   * @param server Web server instance (if available)
   * @param settingsStore Settings store instance (if available)
   */
  void registerRoutes(void* server = nullptr, void* settingsStore = nullptr);

private:
  bool routesRegistered;
#if HW_ENABLE_WEB
  
  // Settings handlers
  static void handleGet(AsyncWebServerRequest* request);
  static void handleSet(AsyncWebServerRequest* request);
  static void handleList(AsyncWebServerRequest* request);
  
  // Helper methods
  static void sendResponse(AsyncWebServerRequest* request, int code, const char* message = nullptr);
  static void sendError(AsyncWebServerRequest* request, int code, const char* message);
  
  // Parameter parsing helpers
  static bool getStringParam(AsyncWebServerRequest* request, const char* name, String& out, bool required = false);
  
  // Validation helpers
  static bool validateKey(const String& key);
  static bool validateValue(const String& value);
  
  // Readiness checks
  static bool checkSystemReady();
  
  // Settings store access (static instance)
  static SettingsStore* getSettingsStore();
  static void setSettingsStore(SettingsStore* store);
  
  static SettingsStore* s_settingsStore;
#endif
};
#endif // HOMEWIND_ENABLE_API_SETTINGS

#endif // HOMEWIND_API_SETTINGS_H

