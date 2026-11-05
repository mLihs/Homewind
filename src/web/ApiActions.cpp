/**
 * @file ApiActions.cpp
 * @brief HTTP action API handlers implementation
 * 
 * All actions use POST with query params or form-urlencoded.
 * No JSON parsing - deterministic, fast execution.
 */

#include "ApiActions.h"

#if HOMEWIND_ENABLE_API_ACTIONS

#include <string.h>
#include "../core/SystemState.h"
#include "../core/TextUtil.h"
#include "../app/BuildInfo.h"
#include "../core/DebugLog.h"
#include "../core/MaintenanceMode.h"
#include "WebHelpers.h"

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
// Forward declaration - will be set by App
extern WebSocketTelemetry* g_webSocketTelemetry;
#endif

#if HW_ENABLE_BLE
#include "../core/BLERelayManager.h"
// Forward declaration - will be set by App
extern BLERelayManager* g_bleRelayManager;
#endif

#if HW_ENABLE_FANS
#include "../core/FanController.h"
// Forward declaration - will be set by App
extern FanController* g_fanController;
#endif

#if HW_ENABLE_DISPLAY
#include "../core/DisplayManager.h"
#endif

#if HW_ENABLE_OTA
#include "../core/FirmwareUpdateManager.h"
// Forward declaration - will be set by App
extern FirmwareUpdateManager* g_firmwareUpdateManager;
#endif

#if HW_ENABLE_WEB

ApiActions::ApiActions()
  : routesRegistered(false)
{
}

// Centralized maintenance mode gate (must be defined before registerRoutes)

// Heap-free whitelist check using strcmp() - no temporary String objects
static bool isWhitelistedDuringMaintenance(const char* path) {
  // Whitelist: firmware endpoints + maintenance endpoints only
  return
    strcmp(path, "/api/v1/action/firmware/status") == 0 ||
    strcmp(path, "/api/v1/action/firmware/abort") == 0 ||
    strcmp(path, "/api/v1/action/firmware/install") == 0 ||
    strcmp(path, "/api/v1/action/firmware/download") == 0 ||
    strcmp(path, "/api/v1/action/firmware/search") == 0 ||
    strcmp(path, "/api/v1/action/system/maintenance/begin") == 0 ||
    strcmp(path, "/api/v1/action/system/maintenance/end") == 0 ||
    strcmp(path, "/api/v1/action/system/maintenance/status") == 0 ||
    strcmp(path, "/api/v1/action/system/factory_reset") == 0 ||
    strcmp(path, "/api/v1/action/system/restart") == 0;
}


// Centralized maintenance gate wrapper - single point of enforcement
static void maintenanceGateWrapper(AsyncWebServerRequest* request, void (*handler)(AsyncWebServerRequest*)) {
  if (!request || !handler) {
    return;
  }
  
  // If maintenance not active, allow request
  if (!MaintenanceMode::isActive()) {
    handler(request);
    return;
  }
  
  // Get request path as C-string (heap-free)
  const char* path = request->url().c_str();
  
  // If whitelisted, allow request
  if (isWhitelistedDuringMaintenance(path)) {
    handler(request);
    return;
  }
  
  // Blocked: send 503
  WebHelpers::sendMaintenance503(request);
}

void ApiActions::registerRoutes(void* serverPtr) {
  if (routesRegistered || !serverPtr) {
    return;
  }
  
  AsyncWebServer* server = static_cast<AsyncWebServer*>(serverPtr);
  if (!server) {
    return;
  }
  
  // Sensor actions (gated via wrapper)
  server->on("/api/v1/action/sensor/connect", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorConnect);
  });
  server->on("/api/v1/action/sensor/disconnect", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorDisconnect);
  });
  server->on("/api/v1/action/sensor/delete", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorDelete);
  });
  server->on("/api/v1/action/sensor/set", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorSet);
  });
  server->on("/api/v1/action/sensor/reload", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorReload);
  });
  server->on("/api/v1/action/sensor/server/pause", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorServerPause);
  });
  server->on("/api/v1/action/sensor/server/resume", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSensorServerResume);
  });
  
  // Discovery actions (gated via wrapper)
  server->on("/api/v1/action/discovery/start", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleDiscoveryStart);
  });
  server->on("/api/v1/action/discovery/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleDiscoveryStop);
  });
  
  // Fan actions (gated via wrapper)
  server->on("/api/v1/action/fan/control", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleFanControl);
  });
  server->on("/api/v1/action/fan/add", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleFanAdd);
  });
  server->on("/api/v1/action/fan/update", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleFanUpdate);
  });
  server->on("/api/v1/action/fan/discovery/start", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleFanDiscoveryStart);
  });
  server->on("/api/v1/action/fan/discovery/stop", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleFanDiscoveryStop);
  });
  server->on("/api/v1/action/fan/remove", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleFanRemove);
  });
  
  // Firmware actions (whitelisted - no gate wrapper)
  server->on("/api/v1/action/firmware/search", HTTP_POST, handleFirmwareSearch);
  server->on("/api/v1/action/firmware/download", HTTP_POST, handleFirmwareDownload);
  server->on("/api/v1/action/firmware/install", HTTP_POST, handleFirmwareInstall);
  server->on("/api/v1/action/firmware/abort", HTTP_POST, handleFirmwareAbort);
  server->on("/api/v1/action/firmware/status", HTTP_GET, handleFirmwareStatus);
  
  // System info (gated via wrapper)
  server->on("/api/v1/system/info", HTTP_GET, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSystemInfo);
  });
  
  // Maintenance mode debug endpoints (whitelisted by definition - no gate wrapper)
  server->on("/api/v1/action/system/maintenance/begin", HTTP_POST, handleMaintenanceBegin);
  server->on("/api/v1/action/system/maintenance/end", HTTP_POST, handleMaintenanceEnd);
  server->on("/api/v1/action/system/maintenance/status", HTTP_GET, handleMaintenanceStatus);
  
  // System actions
  server->on("/api/v1/action/system/factory_reset", HTTP_POST, handleFactoryReset);  // whitelisted
  server->on("/api/v1/action/system/wifi_reset", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleWifiReset);
  });
  server->on("/api/v1/action/system/restart", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleSystemRestart);
  });
  
  // Heart rate settings (gated via wrapper)
  // NOTE: GET endpoint removed - settings are now sent via WebSocket (HEART_RATE_SETTINGS frame)
  server->on("/api/v1/action/heartrate/setmin", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleHeartRateSetMin);
  });
  server->on("/api/v1/action/heartrate/setmax", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleHeartRateSetMax);
  });
  
  // Telemetry refresh (gated via wrapper)
  server->on("/api/v1/action/telemetry/refresh", HTTP_POST, [](AsyncWebServerRequest* req) {
    maintenanceGateWrapper(req, handleTelemetryRefresh);
  });
  
  routesRegistered = true;
  HW_DEBUG_PRINTLN("[ApiActions] Routes registered");
}

// Helper methods

void ApiActions::sendResponse(AsyncWebServerRequest* request, int code, const char* message) {
  if (!request) return;
  if (message) {
    request->send(code, "text/plain", message);
  } else {
    request->send(code, "text/plain", "");
  }
}

void ApiActions::sendError(AsyncWebServerRequest* request, int code, const char* message) {
  sendResponse(request, code, message);
}

void ApiActions::sendSuccess(AsyncWebServerRequest* request, const char* message) {
  sendResponse(request, 200, message ? message : "OK");
}

void ApiActions::sendJSON(AsyncWebServerRequest* request, int code, const char* json) {
  if (!request) return;
  if (json) {
    request->send(code, "application/json", json);
  } else {
    request->send(code, "application/json", "{}");
  }
}

bool ApiActions::getStringParam(AsyncWebServerRequest* request, const char* name, String& out, bool required) {
  bool found = WebHelpers::getStringParam(request, name, out, required);
  if (!found && required) {
    sendError(request, 400, "Missing required parameter");
  }
  return found;
}

bool ApiActions::getBoolParam(AsyncWebServerRequest* request, const char* name, bool& out, bool required) {
  // OPTIMIZATION: Use fixed buffer instead of String (no heap allocation)
  char buffer[16];
  if (!WebHelpers::getParamToBuffer(request, name, buffer, sizeof(buffer), required)) {
    if (required) {
      sendError(request, 400, "Missing required parameter");
    }
    return false;
  }
  
  // In-place lowercase conversion (avoids heap reallocation)
  const char* v = buffer;
  size_t len = strlen(buffer);
  
  // Skip leading whitespace
  while (len > 0 && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) {
    v++;
    len--;
  }
  
  // Skip trailing whitespace (calculate effective length)
  while (len > 0 && (v[len-1] == ' ' || v[len-1] == '\t' || v[len-1] == '\r' || v[len-1] == '\n')) {
    len--;
  }
  
  // Case-insensitive comparison using fixed char buffers
  // Check for common boolean values: "true", "1", "on", "yes"
  out = false;
  if (len == 4 && 
      (v[0] == 't' || v[0] == 'T') && 
      (v[1] == 'r' || v[1] == 'R') && 
      (v[2] == 'u' || v[2] == 'U') && 
      (v[3] == 'e' || v[3] == 'E')) {
    out = true;
  } else if (len == 1 && v[0] == '1') {
    out = true;
  } else if (len == 2 && 
             (v[0] == 'o' || v[0] == 'O') && 
             (v[1] == 'n' || v[1] == 'N')) {
    out = true;
  } else if (len == 3 && 
             (v[0] == 'y' || v[0] == 'Y') && 
             (v[1] == 'e' || v[1] == 'E') && 
             (v[2] == 's' || v[2] == 'S')) {
    out = true;
  }
  
  return true;
}

bool ApiActions::getIntParam(AsyncWebServerRequest* request, const char* name, int& out, bool required) {
  // OPTIMIZATION: Use fixed buffer instead of String (no heap allocation)
  char buffer[32];
  if (!WebHelpers::getParamToBuffer(request, name, buffer, sizeof(buffer), required)) {
    if (required) {
      sendError(request, 400, "Missing required parameter");
    }
    return false;
  }
  
  // In-place trimming using pointer arithmetic (no heap allocation)
  const char* v = buffer;
  size_t len = strlen(buffer);
  
  // Skip leading whitespace
  while (len > 0 && (*v == ' ' || *v == '\t' || *v == '\r' || *v == '\n')) {
    v++;
    len--;
  }
  
  // Skip trailing whitespace
  while (len > 0 && (v[len-1] == ' ' || v[len-1] == '\t' || v[len-1] == '\r' || v[len-1] == '\n')) {
    len--;
  }
  
  // Empty string check
  if (len == 0) {
    if (required) {
      sendError(request, 400, "Invalid integer parameter");
    }
    return false;
  }
  
  // Parse integer manually to avoid String allocations
  // Handle optional leading minus sign
  bool negative = false;
  size_t pos = 0;
  if (v[0] == '-') {
    negative = true;
    pos = 1;
    if (len == 1) {
      // Just a minus sign - invalid
      if (required) {
        sendError(request, 400, "Invalid integer parameter");
      }
      return false;
    }
  }
  
  // Parse digits
  long result = 0;
  for (; pos < len; pos++) {
    char c = v[pos];
    if (c < '0' || c > '9') {
      // Non-digit character - invalid
      if (required) {
        sendError(request, 400, "Invalid integer parameter");
      }
      return false;
    }
    result = result * 10 + (c - '0');
    
    // Overflow check (simplified - won't catch all edge cases)
    if (result > 2147483647L) {
      if (required) {
        sendError(request, 400, "Integer overflow");
      }
      return false;
    }
  }
  
  out = negative ? -(int)result : (int)result;
  return true;
}

bool ApiActions::getUint8Param(AsyncWebServerRequest* request, const char* name, uint8_t& out, bool required) {
  int value;
  if (!getIntParam(request, name, value, required)) {
    return false;
  }
  
  if (value < 0 || value > 255) {
    if (required) {
      sendError(request, 400, "Parameter out of range");
    }
    return false;
  }
  
  out = static_cast<uint8_t>(value);
  return true;
}

bool ApiActions::validateSensorId(const String& id) {
  if (id.length() == 0 || id.length() > 128) {
    return false;
  }
  return true;
}

bool ApiActions::validateFanToken(const String& token) {
  // Fan tokens must be exactly 32 hex characters
  if (token.length() != 32) {
    return false;
  }
  
  for (size_t i = 0; i < token.length(); i++) {
    char c = token.charAt(i);
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  
  return true;
}

bool ApiActions::validateSensorType(const String& type) {
  // Case-insensitive comparison without String allocation
  // Valid types: "HR", "CSC" (case-insensitive)
  size_t len = type.length();
  const char* t = type.c_str();
  
  if (len == 2) {
    // Check for "HR"
    if ((t[0] == 'h' || t[0] == 'H') && (t[1] == 'r' || t[1] == 'R')) {
      return true;
    }
  } else if (len == 3) {
    // Check for "CSC"
    if ((t[0] == 'c' || t[0] == 'C') && (t[1] == 's' || t[1] == 'S') && (t[2] == 'c' || t[2] == 'C')) {
      return true;
    }
  }
  
  return false;
}

bool ApiActions::resolveFanToken(String& token) {
  // If id provided (8 chars), try to resolve to full token
  if (token.length() == 8) {
    // Try to find fan by first 8 chars (case-insensitive, no heap allocation)
#if HW_ENABLE_FANS
    if (!g_fanController || !g_fanController->ready()) {
      return false;
    }
    
    FanConfig config;
    bool found = false;
    const char* searchId = token.c_str();
    for (uint8_t i = 0; i < FanController::MAX_FANS; i++) {
      if (g_fanController->getFanConfig(i, config)) {
        // Compare first 8 chars without creating substring (avoids heap allocation)
        if (strlen(config.token) >= 8 && strncasecmp(config.token, searchId, 8) == 0) {
          token = config.token;
          found = true;
          break;
        }
      }
    }
    return found;
#else
    return false;
#endif
  }
  // Already a full token (32 chars) or invalid length
  return token.length() == 32;
}

bool ApiActions::checkSystemReady() {
  // Check if system is ready (WiFi connected, services initialized)
  return SystemState::isReady();
}

// Sensor action handlers

#if HW_ENABLE_BLE
// Commands are applied by BLERelayManager::loop(); 200 = accepted, WS confirms.
static void sendBleEnqueueResult(AsyncWebServerRequest* request, BleEnqueueResult result) {
  switch (result) {
    case BleEnqueueResult::ACCEPTED:
      request->send(200, "text/plain", "OK");
      break;
    case BleEnqueueResult::FULL:
      request->send(503, "text/plain", "BLE command queue full");
      break;
    case BleEnqueueResult::NOT_FOUND:
      request->send(404, "text/plain", "Sensor not found");
      break;
    case BleEnqueueResult::STALE:
      request->send(409, "text/plain", "Discovery result no longer valid");
      break;
    case BleEnqueueResult::CLOSED:
      request->send(409, "text/plain", "Maintenance active");
      break;
  }
}
#endif

void ApiActions::handleSensorConnect(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
#endif
  
  String name;
  if (!getStringParam(request, "name", name, false)) {
    // Try "id" as fallback
    if (!getStringParam(request, "id", name, true)) {
      return;
    }
  }
  
  if (name.length() == 0) {
    sendError(request, 400, "Missing sensor name/id");
    return;
  }
  
#if HW_ENABLE_BLE
  sendBleEnqueueResult(request, g_bleRelayManager->enqueueConnect(name.c_str()));
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleSensorDisconnect(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
#endif
  
  String name;
  if (!getStringParam(request, "name", name, false)) {
    // Try "id" as fallback
    if (!getStringParam(request, "id", name, true)) {
      return;
    }
  }
  
  if (name.length() == 0) {
    sendError(request, 400, "Missing sensor name/id");
    return;
  }
  
#if HW_ENABLE_BLE
  sendBleEnqueueResult(request, g_bleRelayManager->enqueueDisconnect(name.c_str()));
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleSensorDelete(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
#endif
  
  String name;
  if (!getStringParam(request, "name", name, false)) {
    // Try "id" as fallback
    if (!getStringParam(request, "id", name, true)) {
      return;
    }
  }
  
  if (name.length() == 0) {
    sendError(request, 400, "Missing sensor name/id");
    return;
  }
  
#if HW_ENABLE_BLE
  sendBleEnqueueResult(request, g_bleRelayManager->enqueueDelete(name.c_str()));
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleSensorSet(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
#endif
  
  // Get index parameter (discovery cache index, or -1 for cancel)
  // OPTIMIZATION: Use getIntParam directly (no String allocation)
  int index;
  if (!getIntParam(request, "index", index, false)) {
    sendError(request, 400, "Missing sensor index parameter");
    return;
  }
  
  // Handle cancel (-1)
  if (index < 0) {
    sendSuccess(request, "Cancelled");
    return;
  }
  
#if HW_ENABLE_BLE
  if (index > 255) {
    sendError(request, 404, "Invalid sensor index (not found in discovery cache)");
    return;
  }

  // Discovery generation the index belongs to (DISCOVERY_RESULTS frame)
  char genStr[12];
  if (!WebHelpers::getParamToBuffer(request, "generation", genStr, sizeof(genStr), false)) {
    sendError(request, 400, "Missing generation parameter");
    return;
  }
  char* end = nullptr;
  const unsigned long generation = strtoul(genStr, &end, 10);
  if (end == genStr || *end != '\0') {
    sendError(request, 400, "Invalid generation parameter");
    return;
  }
  
  // Get forceApply parameter (optional)
  // OPTIMIZATION: Use fixed buffer instead of String
  char forceStr[8];
  bool forceApply = false;
  if (WebHelpers::getParamToBuffer(request, "forceApply", forceStr, sizeof(forceStr), false) || 
      WebHelpers::getParamToBuffer(request, "force", forceStr, sizeof(forceStr), false)) {
    forceApply = (strcmp(forceStr, "true") == 0 || strcmp(forceStr, "1") == 0);
  }
  
  sendBleEnqueueResult(request, g_bleRelayManager->enqueueSetFromDiscovery(
      static_cast<uint32_t>(generation), static_cast<uint8_t>(index), forceApply));
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleSensorReload(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
  
  sendBleEnqueueResult(request, g_bleRelayManager->enqueueReload());
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleSensorServerPause(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
  
  sendBleEnqueueResult(request, g_bleRelayManager->requestServerPaused(true));
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleSensorServerResume(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
  
  sendBleEnqueueResult(request, g_bleRelayManager->requestServerPaused(false));
#else
  sendSuccess(request, "OK");
#endif
}

// Discovery action handlers

void ApiActions::handleDiscoveryStart(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
#endif
  
  String type;
  if (!getStringParam(request, "type", type, false)) {
    // Type is optional - use default if not provided
    type = "";
  }
  
  if (type.length() > 0 && !validateSensorType(type)) {
    sendError(request, 400, "Invalid sensor type");
    return;
  }
  
#if HW_ENABLE_BLE
  // Empty type = both; "HR" has length 2, "CSC" length 3 (validated above)
  uint8_t mask = hw::kBleTypeHR | hw::kBleTypeCSC;
  if (type.length() > 0) {
    mask = (type.length() == 2) ? hw::kBleTypeHR : hw::kBleTypeCSC;
  }
  // "busy" (discovery already open) is reported in the DISCOVERY_STATUS frame
  sendBleEnqueueResult(request, g_bleRelayManager->requestDiscoveryStart(mask));
#else
  sendSuccess(request, "OK");
#endif
}

void ApiActions::handleDiscoveryStop(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    sendError(request, 503, "BLE service not available");
    return;
  }
  
  sendBleEnqueueResult(request, g_bleRelayManager->requestDiscoveryStop());
#else
  sendSuccess(request, "OK");
#endif
}

// Fan action handlers

void ApiActions::handleFanControl(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_FANS
  if (!g_fanController || !g_fanController->ready()) {
    sendError(request, 503, "Fan controller not available");
    return;
  }
  
  String token;
  // Try token first, then id (first 8 chars of token)
  if (!getStringParam(request, "token", token, false)) {
    if (!getStringParam(request, "id", token, false)) {
      sendError(request, 400, "Missing token or id parameter");
      return;
    }
  }
  
  // Resolve token from ID (8 chars) to full token (32 chars)
  if (!resolveFanToken(token)) {
    sendError(request, 404, "Fan not found");
    return;
  }
  
  if (!validateFanToken(token)) {
    sendError(request, 400, "Invalid fan token");
    return;
  }
  
  bool active;
  // Support both "active" and "controlState" parameters
  // OPTIMIZATION: Use fixed buffer instead of String
  char controlState[16];
  if (WebHelpers::getParamToBuffer(request, "controlState", controlState, sizeof(controlState), false)) {
    // Case-insensitive check for "ACTIVE" (avoid strcasecmp for portability)
    active = (strcmp(controlState, "ACTIVE") == 0 || 
              strcmp(controlState, "active") == 0 || 
              strcmp(controlState, "1") == 0);
  } else if (!getBoolParam(request, "active", active, true)) {
    return;
  }
  
  if (!g_fanController->setFanControlState(token, active)) {
    if (MaintenanceMode::admissionClosed()) {
      sendError(request, 409, "Maintenance active");
      return;
    }
    sendError(request, 500, "Failed to set fan control state");
    return;
  }
  
  sendSuccess(request, "OK");
#else
  sendError(request, 503, "Fans not enabled");
#endif
}

void ApiActions::handleFanDiscoveryStart(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  // TODO: Integrate with fan manager
  // if (!fanManager.startDiscovery()) {
  //   sendError(request, 409, "Discovery already in progress");
  //   return;
  // }
  
  sendSuccess(request, "OK");
}

void ApiActions::handleFanDiscoveryStop(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  // TODO: Integrate with fan manager
  // if (!fanManager.stopDiscovery()) {
  //   sendError(request, 500, "Failed to stop discovery");
  //   return;
  // }
  
  sendSuccess(request, "OK");
}

void ApiActions::handleFanAdd(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_FANS
  if (!g_fanController || !g_fanController->ready()) {
    sendError(request, 503, "Fan controller not available");
    return;
  }
  
  String token;
  if (!getStringParam(request, "token", token, true)) {
    return;
  }
  
  if (!validateFanToken(token)) {
    sendError(request, 400, "Invalid fan token");
    return;
  }
  
  if (!g_fanController->addFan(token)) {
    if (MaintenanceMode::admissionClosed()) {
      sendError(request, 409, "Maintenance active");
      return;
    }
    sendError(request, 500, "Failed to add fan");
    return;
  }
  
  sendSuccess(request, "OK");
#else
  sendError(request, 503, "Fans not enabled");
#endif
}

void ApiActions::handleFanUpdate(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_FANS
  if (!g_fanController || !g_fanController->ready()) {
    sendError(request, 503, "Fan controller not available");
    return;
  }
  
  String oldToken;
  if (!getStringParam(request, "oldToken", oldToken, false)) {
    if (!getStringParam(request, "token", oldToken, false)) {
      sendError(request, 400, "Missing oldToken or token parameter");
      return;
    }
  }
  
  String newToken;
  if (!getStringParam(request, "newToken", newToken, false)) {
    if (!getStringParam(request, "token", newToken, false)) {
      sendError(request, 400, "Missing newToken or token parameter");
      return;
    }
  }
  
  if (!validateFanToken(oldToken) || !validateFanToken(newToken)) {
    sendError(request, 400, "Invalid fan token");
    return;
  }
  
  if (!g_fanController->updateFanToken(oldToken, newToken)) {
    if (MaintenanceMode::admissionClosed()) {
      sendError(request, 409, "Maintenance active");
      return;
    }
    sendError(request, 500, "Failed to update fan token");
    return;
  }
  
  sendSuccess(request, "OK");
#else
  sendError(request, 503, "Fans not enabled");
#endif
}

void ApiActions::handleFanRemove(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_FANS
  if (!g_fanController || !g_fanController->ready()) {
    sendError(request, 503, "Fan controller not available");
    return;
  }
  
  String token;
  // Try token first, then id (first 8 chars of token)
  if (!getStringParam(request, "token", token, false)) {
    if (!getStringParam(request, "id", token, false)) {
      sendError(request, 400, "Missing token or id parameter");
      return;
    }
  }
  
  // Resolve token from ID (8 chars) to full token (32 chars)
  if (!resolveFanToken(token)) {
    sendError(request, 404, "Fan not found");
    return;
  }
  
  if (!validateFanToken(token)) {
    sendError(request, 400, "Invalid fan token");
    return;
  }
  
  // Removal, library cleanup and the NVS save are queued for loop()
  if (!g_fanController->removeFanFast(token)) {
    if (MaintenanceMode::admissionClosed()) {
      sendError(request, 409, "Maintenance active");
      return;
    }
    sendError(request, 500, "Failed to remove fan");
    return;
  }
  
  sendSuccess(request, "OK");
#else
  sendError(request, 503, "Fans not enabled");
#endif
}

// Firmware action handlers

void ApiActions::handleFirmwareSearch(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_OTA
  if (!g_firmwareUpdateManager || !g_firmwareUpdateManager->isReady()) {
    sendError(request, 503, "Firmware update not available");
    return;
  }
  
  if (g_firmwareUpdateManager->isUpdating() || MaintenanceMode::admissionClosed()) {
    sendError(request, 409, "Update already in progress");
    return;
  }
  
  // Request async search (returns immediately)
  if (!g_firmwareUpdateManager->requestSearch()) {
    sendError(request, 500, "Failed to start search");
    return;
  }
  
  // Return 202 Accepted - search is in progress, UI should poll status
  request->send(202, "application/json", "{\"ok\":true,\"state\":\"searching\"}");
#else
  sendError(request, 503, "Firmware update not enabled");
  return;
#endif
}

void ApiActions::handleFirmwareDownload(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_OTA
  if (!g_firmwareUpdateManager || !g_firmwareUpdateManager->isReady()) {
    sendError(request, 503, "Firmware update not available");
    return;
  }
  
  // Admission + enqueue only; the loop quiesces BLE/fans, the worker downloads
  // after the ACK. Progress and result via firmware/status (design §7.4).
  switch (g_firmwareUpdateManager->requestDownload()) {
    case hw::MaintAdmit::ACCEPTED:
    case hw::MaintAdmit::ALREADY:
      request->send(202, "application/json", "{\"ok\":true,\"job\":\"ota\",\"state\":\"quiescing\"}");
      return;
    case hw::MaintAdmit::CONFLICT:
      request->send(409, "application/json", "{\"ok\":false,\"error\":\"maintenance_active\"}");
      return;
    case hw::MaintAdmit::NOT_READY:
      request->send(409, "application/json", "{\"ok\":false,\"error\":\"not_ready_to_update\"}");
      return;
  }
#else
  sendError(request, 503, "Firmware update not enabled");
  return;
#endif
}

void ApiActions::handleFirmwareInstall(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_OTA
  if (!g_firmwareUpdateManager || !g_firmwareUpdateManager->isReady()) {
    sendError(request, 503, "Firmware update not available");
    return;
  }
  
  // Installation is handled automatically by GitFirmwareUpdate after download
  // This endpoint just confirms the transition
  if (!g_firmwareUpdateManager->startInstall()) {
    sendError(request, 400, "Firmware not ready for installation");
    return;
  }
#else
  sendError(request, 503, "Firmware update not enabled");
  return;
#endif
  
  sendSuccess(request, "OK");
}

void ApiActions::handleFirmwareAbort(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HW_ENABLE_OTA
  if (!g_firmwareUpdateManager || !g_firmwareUpdateManager->isReady()) {
    sendError(request, 503, "Firmware update not available");
    return;
  }
  
  if (!g_firmwareUpdateManager->abort()) {
    sendError(request, 500, "Failed to abort update");
    return;
  }
#else
  sendError(request, 503, "Firmware update not enabled");
  return;
#endif
  
  sendSuccess(request, "OK");
}

void ApiActions::handleFirmwareStatus(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check
  // Status endpoint - always available (read-only)
#if HW_ENABLE_OTA
  if (!g_firmwareUpdateManager || !g_firmwareUpdateManager->isReady()) {
    sendResponse(request, 200, "state=unavailable\n");
    return;
  }
  
  // Fixed-size buffer for deterministic response (no heap allocation)
  static const size_t OUT_BUF_SIZE = 512;
  static_assert(OUT_BUF_SIZE >= 512, "Buffer must be at least 512 bytes");
  char out[OUT_BUF_SIZE];
  size_t used = 0;
  bool truncated = false;
  
  // OPTIMIZATION: All getters now return const char* (no String allocation)
  const char* state = g_firmwareUpdateManager->getStateString();
  int progress = g_firmwareUpdateManager->getProgress();
  const char* error = g_firmwareUpdateManager->getLastError();
  const char* remoteVersion = g_firmwareUpdateManager->getRemoteVersion();
  // const char* releaseNotes = g_firmwareUpdateManager->getReleaseNotes();  // Unused, delivered via WS
  
  // Build response in text/plain format (key=value pairs) using fixed buffer
  if (!appendf(out, OUT_BUF_SIZE, used, "state=%s\nprogress=%d\n", 
               state, progress)) {
    truncated = true;
  }
  
  if (error[0] != '\0' && !truncated) {
    if (!appendf(out, OUT_BUF_SIZE, used, "error=%s\n", error)) {
      truncated = true;
    }
  }
  
  if (remoteVersion[0] != '\0' && !truncated) {
    // Truncate version to max 32 chars
    char versionBuf[33];
    truncate_ascii(versionBuf, sizeof(versionBuf), remoteVersion, 32, false);
    if (!appendf(out, OUT_BUF_SIZE, used, "remote_version=%s\n", versionBuf)) {
      truncated = true;
    }
  }
  
  // Notes delivered via WebSocket FIRMWARE_SEARCH_RESULT frame
  // Omit from HTTP to keep response small
  // (releaseNotes available via getReleaseNotes() if needed)
  
  if (truncated) {
    // Indicate truncation in response
    appendf(out, OUT_BUF_SIZE, used, "truncated=1\n");
  }
  
  // Ensure NUL termination
  if (used >= OUT_BUF_SIZE) {
    used = OUT_BUF_SIZE - 1;
  }
  out[used] = '\0';
  
  sendResponse(request, 200, out);
#else
  sendResponse(request, 200, "state=disabled\n");
#endif
}

void ApiActions::handleSystemInfo(AsyncWebServerRequest* request) {
  // System info endpoint (read-only)
  // Returns: name, version, build_id, uptime, free_heap (+ board, te_pulses with display)
  // Fixed-size buffer for deterministic response (no heap allocation)
  static const size_t OUT_BUF_SIZE = 512;
  static_assert(OUT_BUF_SIZE >= 512, "Buffer must be at least 512 bytes");
  char out[OUT_BUF_SIZE];
  size_t used = 0;
  bool truncated = false;
  
  // Truncate build_id to max 8 chars
  const char* buildId = BuildInfo::getBuildId();
  char buildIdBuf[9];
  truncate_ascii(buildIdBuf, sizeof(buildIdBuf), buildId, 8, false);
  
  // Truncate version to max 32 chars
  const char* version = BuildInfo::getVersion();
  char versionBuf[33];
  truncate_ascii(versionBuf, sizeof(versionBuf), version, 32, false);
  
  if (!appendf(out, OUT_BUF_SIZE, used, "name=%s\nversion=%s\nbuild_id=%s\n",
               BuildInfo::getName(), versionBuf, buildIdBuf)) {
    truncated = true;
  }
  
  if (!truncated) {
    if (!appendf(out, OUT_BUF_SIZE, used, "uptime_ms=%lu\nfree_heap=%lu\nlargest_free_block=%lu\nready=%d\n",
                 SystemState::getUptimeMs(), 
                 SystemState::getFreeHeap(),
                 SystemState::getLargestFreeBlock(),
                 SystemState::isReady() ? 1 : 0)) {
      truncated = true;
    }
  }

#if HW_ENABLE_DISPLAY
  if (!truncated && g_displayManager && g_displayManager->boardRevision()) {
    if (!appendf(out, OUT_BUF_SIZE, used, "board=%s\nte_pulses=%lu\n",
                 g_displayManager->boardRevision(),
                 static_cast<unsigned long>(g_displayManager->tePulseCount()))) {
      truncated = true;
    }
  }
#endif
  
  if (truncated) {
    // Indicate truncation in response
    appendf(out, OUT_BUF_SIZE, used, "truncated=1\n");
  }
  
  // Ensure NUL termination
  if (used >= OUT_BUF_SIZE) {
    used = OUT_BUF_SIZE - 1;
  }
  out[used] = '\0';
  
  sendResponse(request, 200, out);
}

// Maintenance endpoints (design §7.4: one-way until reboot)

static void sendAdmitResult(AsyncWebServerRequest* request, hw::MaintAdmit admit, const char* job) {
  char json[96];
  if (admit == hw::MaintAdmit::ACCEPTED || admit == hw::MaintAdmit::ALREADY) {
    snprintf(json, sizeof(json), "{\"ok\":true,\"job\":\"%s\"}", job);
    request->send(202, "application/json", json);
  } else {
    request->send(409, "application/json", "{\"ok\":false,\"error\":\"maintenance_active\"}");
  }
}

void ApiActions::handleMaintenanceBegin(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check (by definition)
  // Manual maintenance only: an erase or a download needs its own endpoint.
  sendAdmitResult(request, MaintenanceMode::request(hw::MaintReason::MANUAL), "maintenance");
}

void ApiActions::handleMaintenanceEnd(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check (by definition)
  // Documented no-op: maintenance ends with a reboot.
  MaintenanceMode::end();
  request->send(409, "application/json", "{\"ok\":false,\"error\":\"restart_required\"}");
}

void ApiActions::handleMaintenanceStatus(AsyncWebServerRequest* request) {
  // Whitelisted, heap-free
  char json[224];
  int n = snprintf(json, sizeof(json),
                   "{\"state\":\"%s\",\"reason\":\"%s\",\"result\":\"%s\",\"restart_required\":%s",
                   MaintenanceMode::stateString(), MaintenanceMode::reasonString(),
                   MaintenanceMode::resultString(),
                   MaintenanceMode::restartRequired() ? "true" : "false");
  uint32_t attempted = 0;
  uint32_t failedMask = 0;
  if (n > 0 && static_cast<size_t>(n) < sizeof(json) &&
      MaintenanceMode::factoryResetReport(attempted, failedMask)) {
    n += snprintf(json + n, sizeof(json) - n,
                  ",\"factory_reset\":{\"attempted\":%lu,\"failed_mask\":%lu}",
                  static_cast<unsigned long>(attempted), static_cast<unsigned long>(failedMask));
  }
  if (n > 0 && static_cast<size_t>(n) < sizeof(json) - 1) {
    json[n++] = '}';
    json[n] = '\0';
  }
  request->send(200, "application/json", json);
}

void ApiActions::handleFactoryReset(AsyncWebServerRequest* request) {
  // Whitelisted - no maintenance gate check (by definition)
  // Erase and reboot run in loop() after the maintenance ACK.
  HW_DEBUG_PRINTLN("[Reset] Factory reset requested");
  sendAdmitResult(request, MaintenanceMode::request(hw::MaintReason::FACTORY_RESET), "factory_reset");
}

void ApiActions::handleSystemRestart(AsyncWebServerRequest* request) {
  // Whitelisted: also the way out of maintenance
  HW_DEBUG_PRINTLN("[ApiActions] Device restart requested");
  MaintenanceMode::requestRestart();
  request->send(202, "application/json", "{\"ok\":true,\"job\":\"restart\"}");
}

void ApiActions::handleWifiReset(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
#if HW_ENABLE_WIFI && HW_WIFI_BACKEND_WML
  if (!MaintenanceMode::requestWifiReset()) {
    request->send(409, "application/json", "{\"ok\":false,\"error\":\"maintenance_active\"}");
    return;
  }
  HW_DEBUG_PRINTLN("[ApiActions] WiFi reset queued, WML will reboot");
  request->send(202, "application/json", "{\"ok\":true,\"job\":\"wifi_reset\",\"rebooting\":true}");
#else
  sendError(request, 503, "WiFi reset not available");
#endif
}

#if HW_ENABLE_NVS
#include "../core/HeartRateSettings.h"
// Forward declaration - will be set by App
extern HeartRateSettings* g_heartRateSettings;

// NOTE: handleHeartRateGet() removed - settings are now sent via WebSocket (HEART_RATE_SETTINGS frame)

void ApiActions::handleHeartRateSetMin(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  if (!g_heartRateSettings) {
    sendError(request, 503, "Heart rate settings not available");
    return;
  }
  
  int value;
  if (!getIntParam(request, "value", value, true)) {
    return;
  }
  
  if (value < 0 || value > 65535) {
    sendError(request, 400, "Invalid value");
    return;
  }
  
  uint16_t minValue = static_cast<uint16_t>(value);
  if (g_heartRateSettings->setMin(minValue)) {
    char json[128];
    snprintf(json, sizeof(json), "{\"min\":%u,\"max\":%u}", 
             g_heartRateSettings->getMin(), 
             g_heartRateSettings->getMax());
    sendJSON(request, 200, json);
  } else {
    sendError(request, 500, "Failed to set min heart rate");
  }
}

void ApiActions::handleHeartRateSetMax(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  if (!g_heartRateSettings) {
    sendError(request, 503, "Heart rate settings not available");
    return;
  }
  
  int value;
  if (!getIntParam(request, "value", value, true)) {
    return;
  }
  
  if (value < 0 || value > 65535) {
    sendError(request, 400, "Invalid value");
    return;
  }
  
  uint16_t maxValue = static_cast<uint16_t>(value);
  if (g_heartRateSettings->setMax(maxValue)) {
    char json[128];
    snprintf(json, sizeof(json), "{\"min\":%u,\"max\":%u}", 
             g_heartRateSettings->getMin(), 
             g_heartRateSettings->getMax());
    sendJSON(request, 200, json);
  } else {
    sendError(request, 500, "Failed to set max heart rate");
  }
}

void ApiActions::handleTelemetryRefresh(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
#if HOMEWIND_ENABLE_WEBSOCKET
  if (!g_webSocketTelemetry || !g_webSocketTelemetry->ready()) {
    sendError(request, 503, "WebSocket not ready");
    return;
  }
  
  // Trigger immediate refresh for key UI snapshots.
  //
  // IMPORTANT (heap/fragmentation):
  // The UI may call this right after a cold page load (no browser cache),
  // when the web server is already under memory pressure from concurrent
  // HTTP asset transfers. Broadcasting *both* heavy snapshots (SENSORS+FANS)
  // to multiple WS clients can cause a large temporary largest-block collapse.
  //
  // Strategy:
  // - Always allow "small" frames (cheap)
  // - Only allow "heavy" snapshots when <= 1 client is connected
  //   (multi-client heavy is handled by WebSocketTelemetry's budgeted scheduler)
  g_webSocketTelemetry->markDirty(TelemetryFrameType::SYSTEM_STATUS);
  g_webSocketTelemetry->markDirty(TelemetryFrameType::FIRMWARE_VERSION);
  g_webSocketTelemetry->markDirty(TelemetryFrameType::HEART_RATE_SETTINGS);

  // If the UI provides its WS client_id (sent via a small WS control frame),
  // unicast heavy snapshots to that specific client even in multi-client scenarios.
  int clientIdParam = 0;
  bool hasClientId = getIntParam(request, "client_id", clientIdParam, false);
  if (hasClientId && clientIdParam > 0) {
    const uint32_t clientId = static_cast<uint32_t>(clientIdParam);
    g_webSocketTelemetry->requestSnapshotsForClient(clientId);
  } else {
    const uint8_t clients = g_webSocketTelemetry->getClientCount();
    if (clients <= 1) {
      g_webSocketTelemetry->markDirty(TelemetryFrameType::SENSORS_SNAPSHOT);
      g_webSocketTelemetry->markDirty(TelemetryFrameType::FANS_SNAPSHOT);
    }
  }
  
  sendSuccess(request, "OK");
#else
  sendError(request, 503, "WebSocket disabled");
#endif
}
#endif // HW_ENABLE_NVS

#else // HW_ENABLE_WEB == 0

ApiActions::ApiActions()
  : routesRegistered(false)
{
}

void ApiActions::registerRoutes(void* server) {
  (void)server;
  if (routesRegistered) {
    return;
  }
  HW_DEBUG_PRINTLN("[ApiActions] Routes registered (stub - Web server not enabled)");
  routesRegistered = true;
}

#endif // HW_ENABLE_WEB

#else // HOMEWIND_ENABLE_API_ACTIONS == 0

// Stub implementation when API Actions are disabled
#include "../core/DebugLog.h"

ApiActions::ApiActions() : routesRegistered(false) {}

void ApiActions::registerRoutes(void* server) {
  (void)server;
}

#endif // HOMEWIND_ENABLE_API_ACTIONS
