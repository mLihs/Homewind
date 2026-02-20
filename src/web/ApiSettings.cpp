/**
 * @file ApiSettings.cpp
 * @brief HTTP settings API handlers implementation
 * 
 * All settings operations use query params (no JSON).
 * Responses are text/plain.
 */

#include "ApiSettings.h"

#if HOMEWIND_ENABLE_API_SETTINGS
#include "../settings/SettingsStore.h"
#include "../core/SystemState.h"
#include "../core/DebugLog.h"
#include "../core/MaintenanceMode.h"
#include "WebHelpers.h"
#include <string.h>

#if HW_ENABLE_WEB

SettingsStore* ApiSettings::s_settingsStore = nullptr;

ApiSettings::ApiSettings()
  : routesRegistered(false)
{
}

void ApiSettings::setSettingsStore(SettingsStore* store) {
  s_settingsStore = store;
}

SettingsStore* ApiSettings::getSettingsStore() {
  return s_settingsStore;
}

// Centralized maintenance mode gate for ApiSettings (must be defined before registerRoutes)

static void settingsMaintenanceGateWrapper(AsyncWebServerRequest* request, void (*handler)(AsyncWebServerRequest*)) {
  if (!request || !handler) {
    return;
  }
  
  // If maintenance not active, allow request
  if (!MaintenanceMode::isActive()) {
    handler(request);
    return;
  }
  
  // ApiSettings endpoints are NOT whitelisted - block during maintenance
  WebHelpers::sendMaintenance503(request);
}

void ApiSettings::registerRoutes(void* serverPtr, void* settingsStorePtr) {
  if (routesRegistered || !serverPtr) {
    return;
  }
  
  AsyncWebServer* server = static_cast<AsyncWebServer*>(serverPtr);
  if (!server) {
    return;
  }
  
  SettingsStore* settingsStore = static_cast<SettingsStore*>(settingsStorePtr);
  setSettingsStore(settingsStore);
  
  // Settings routes (gated via wrapper)
  server->on("/api/v1/settings/get", HTTP_GET, [](AsyncWebServerRequest* req) {
    settingsMaintenanceGateWrapper(req, handleGet);
  });
  server->on("/api/v1/settings/set", HTTP_POST, [](AsyncWebServerRequest* req) {
    settingsMaintenanceGateWrapper(req, handleSet);
  });
  server->on("/api/v1/settings/list", HTTP_GET, [](AsyncWebServerRequest* req) {
    settingsMaintenanceGateWrapper(req, handleList);
  });
  
  routesRegistered = true;
  HW_DEBUG_PRINTLN("[ApiSettings] Routes registered");
}

// Helper methods

void ApiSettings::sendResponse(AsyncWebServerRequest* request, int code, const char* message) {
  if (!request) return;
  if (message) {
    request->send(code, "text/plain", message);
  } else {
    request->send(code, "text/plain", "");
  }
}

void ApiSettings::sendError(AsyncWebServerRequest* request, int code, const char* message) {
  sendResponse(request, code, message);
}

bool ApiSettings::getStringParam(AsyncWebServerRequest* request, const char* name, String& out, bool required) {
  bool found = WebHelpers::getStringParam(request, name, out, required);
  if (!found && required) {
    sendError(request, 400, "Missing required parameter");
  }
  return found;
}

bool ApiSettings::validateKey(const String& key) {
  if (key.length() == 0 || key.length() > 128) {
    return false;
  }
  
  // Validate key format: alphanumeric, dots, underscores, hyphens
  for (size_t i = 0; i < key.length(); i++) {
    char c = key.charAt(i);
    if (!((c >= 'a' && c <= 'z') || 
          (c >= 'A' && c <= 'Z') || 
          (c >= '0' && c <= '9') || 
          c == '.' || c == '_' || c == '-')) {
      return false;
    }
  }
  
  return true;
}

bool ApiSettings::validateValue(const String& value) {
  // Values can be up to 512 bytes (reasonable limit for embedded)
  if (value.length() > 512) {
    return false;
  }
  return true;
}

bool ApiSettings::checkSystemReady() {
  // Check if system is ready (WiFi connected, services initialized)
  return SystemState::isReady();
}

// Settings handlers

void ApiSettings::handleGet(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  SettingsStore* store = getSettingsStore();
  if (!store) {
    sendError(request, 503, "Settings store not available");
    return;
  }
  
  String key;
  if (!getStringParam(request, "key", key, true)) {
    return;
  }
  
  if (!validateKey(key)) {
    sendError(request, 400, "Invalid setting key");
    return;
  }
  
  String value = store->get(key);
  
  if (value.length() == 0 && !store->exists(key)) {
    sendError(request, 404, "Setting not found");
    return;
  }
  
  sendResponse(request, 200, value.c_str());
}

void ApiSettings::handleSet(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  SettingsStore* store = getSettingsStore();
  if (!store) {
    sendError(request, 503, "Settings store not available");
    return;
  }
  
  String key;
  if (!getStringParam(request, "key", key, true)) {
    return;
  }
  
  if (!validateKey(key)) {
    sendError(request, 400, "Invalid setting key");
    return;
  }
  
  String value;
  if (!getStringParam(request, "value", value, true)) {
    return;
  }
  
  if (!validateValue(value)) {
    sendError(request, 400, "Setting value too long");
    return;
  }
  
  // Special handling for structured settings (comma-separated key=value pairs)
  // Examples: sensor.{name} = "type=HR,mac=AA:BB:CC:DD:EE:FF"
  //           sensor.hr_range = "min=50,max=200"
  // This allows complex settings without JSON
  
  if (!store->set(key, value)) {
    sendError(request, 500, "Failed to save setting");
    return;
  }
  
  sendResponse(request, 200, "OK");
}

void ApiSettings::handleList(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  SettingsStore* store = getSettingsStore();
  if (!store) {
    sendError(request, 503, "Settings store not available");
    return;
  }
  
  String prefix;
  if (!getStringParam(request, "prefix", prefix, false)) {
    prefix = ""; // List all if no prefix
  }
  
  // Validate prefix if provided
  if (prefix.length() > 0 && !validateKey(prefix)) {
    sendError(request, 400, "Invalid prefix");
    return;
  }
  
  String list = store->list(prefix);
  
  // Response format: newline-separated keys, or key=value pairs
  // For simplicity, return newline-separated keys
  // Client can call GET for each key to get values
  
  if (list.length() == 0) {
    sendResponse(request, 200, ""); // Empty list
    return;
  }
  
  sendResponse(request, 200, list.c_str());
}

#else // HW_ENABLE_WEB == 0

// Stub implementation when Web server is not enabled
// Note: s_settingsStore is not declared in the class when HW_ENABLE_WEB is 0,
// so we cannot define it here.

ApiSettings::ApiSettings()
  : routesRegistered(false)
{
}

void ApiSettings::registerRoutes(void* server, void* settingsStore) {
  (void)server;
  (void)settingsStore;
  if (routesRegistered) {
    return;
  }
  HW_DEBUG_PRINTLN("[ApiSettings] Routes registered (stub - Web server not enabled)");
  routesRegistered = true;
}

#endif // HW_ENABLE_WEB

#else // HOMEWIND_ENABLE_API_SETTINGS == 0

// Stub implementation when API Settings are disabled
// Note: This block should never be reached if App.cpp uses ApiSettings,
// because the class definition only exists when HOMEWIND_ENABLE_API_SETTINGS is 1.
// However, we provide minimal stubs for completeness.

#include "../core/DebugLog.h"

// If HOMEWIND_ENABLE_API_SETTINGS is 0, the class ApiSettings doesn't exist,
// so we cannot provide implementations here. This code path should not compile.

#endif // HOMEWIND_ENABLE_API_SETTINGS
