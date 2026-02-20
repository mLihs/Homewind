/**
 * @file ApiActions.h
 * @brief HTTP action API handlers (POST /api/v1/action/*)
 */

#ifndef HOMEWIND_API_ACTIONS_H
#define HOMEWIND_API_ACTIONS_H

#include <Arduino.h>
#include "../app/Config.h"

#if HW_ENABLE_WEB
#include <ESPAsyncWebServer.h>
#endif

class ApiActions {
public:
  ApiActions();
  void registerRoutes(void* server = nullptr);
  
#if HOMEWIND_ENABLE_SETTINGS
  static void setSettingsStore(void* store);
#endif
  static void setWiFiServiceWML(void* wml);

private:
  bool routesRegistered;
#if HOMEWIND_ENABLE_API_ACTIONS && HW_ENABLE_WEB
  static void handleSensorConnect(AsyncWebServerRequest* request);
  static void handleSensorDisconnect(AsyncWebServerRequest* request);
  static void handleSensorDelete(AsyncWebServerRequest* request);
  static void handleSensorSet(AsyncWebServerRequest* request);
  static void handleSensorReload(AsyncWebServerRequest* request);
  static void handleSensorServerPause(AsyncWebServerRequest* request);
  static void handleSensorServerResume(AsyncWebServerRequest* request);
  static void handleDiscoveryStart(AsyncWebServerRequest* request);
  static void handleDiscoveryStop(AsyncWebServerRequest* request);
  static void handleFanControl(AsyncWebServerRequest* request);
  static void handleFanAdd(AsyncWebServerRequest* request);
  static void handleFanUpdate(AsyncWebServerRequest* request);
  static void handleFanDiscoveryStart(AsyncWebServerRequest* request);
  static void handleFanDiscoveryStop(AsyncWebServerRequest* request);
  static void handleFanRemove(AsyncWebServerRequest* request);
  static void handleFirmwareSearch(AsyncWebServerRequest* request);
  static void handleFirmwareDownload(AsyncWebServerRequest* request);
  static void handleFirmwareInstall(AsyncWebServerRequest* request);
  static void handleFirmwareAbort(AsyncWebServerRequest* request);
  static void handleFirmwareStatus(AsyncWebServerRequest* request);
  static void handleSystemInfo(AsyncWebServerRequest* request);
  static void handleSessionGet(AsyncWebServerRequest* request);
  static void handleUILockAcquire(AsyncWebServerRequest* request);
  static void handleUILockRelease(AsyncWebServerRequest* request);
  static void handleUILockStatus(AsyncWebServerRequest* request);
  static void handleMaintenanceBegin(AsyncWebServerRequest* request);
  static void handleMaintenanceEnd(AsyncWebServerRequest* request);
  static void handleFactoryReset(AsyncWebServerRequest* request);
  static void handleSystemRestart(AsyncWebServerRequest* request);
  static void handleWifiReset(AsyncWebServerRequest* request);
  // NOTE: handleHeartRateGet removed - settings now sent via WebSocket (HEART_RATE_SETTINGS frame)
  static void handleHeartRateSetMin(AsyncWebServerRequest* request);
  static void handleHeartRateSetMax(AsyncWebServerRequest* request);
  static void handleTelemetryRefresh(AsyncWebServerRequest* request);
  static void sendResponse(AsyncWebServerRequest* request, int code, const char* message = nullptr);
  static void sendJSON(AsyncWebServerRequest* request, int code, const char* json);
  static void sendError(AsyncWebServerRequest* request, int code, const char* message);
  static void sendSuccess(AsyncWebServerRequest* request, const char* message = "OK");
  static bool getStringParam(AsyncWebServerRequest* request, const char* name, String& out, bool required = false);
  static bool getBoolParam(AsyncWebServerRequest* request, const char* name, bool& out, bool required = false);
  static bool getIntParam(AsyncWebServerRequest* request, const char* name, int& out, bool required = false);
  static bool getUint8Param(AsyncWebServerRequest* request, const char* name, uint8_t& out, bool required = false);
  static bool validateSensorId(const String& id);
  static bool validateFanToken(const String& token);
  static bool validateSensorType(const String& type);
  
  // Helper to resolve fan token from ID (first 8 chars) or full token
  static bool resolveFanToken(String& token);
  static bool checkSystemReady();
#endif
};

#endif
