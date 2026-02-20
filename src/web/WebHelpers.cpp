/**
 * @file WebHelpers.cpp
 * @brief Common web server helper functions implementation
 */

#include "WebHelpers.h"
#include "../core/MaintenanceMode.h"
#include <string.h>
#include <stdlib.h>  // For strtol

namespace WebHelpers {

void sendMaintenance503(AsyncWebServerRequest* request) {
  if (!request) return;
  
  // Static string literals only - no allocations
  const char* reason = MaintenanceMode::reasonString();
  const char* json = nullptr;
  
  if (strcmp(reason, "ota") == 0) {
    json = "{\"error\":\"MAINTENANCE_MODE\",\"reason\":\"ota\"}";
  } else if (strcmp(reason, "factory_reset") == 0) {
    json = "{\"error\":\"MAINTENANCE_MODE\",\"reason\":\"factory_reset\"}";
  } else {
    json = "{\"error\":\"MAINTENANCE_MODE\",\"reason\":\"unknown\"}";
  }
  
  request->send(503, "application/json", json);
}

bool getStringParam(AsyncWebServerRequest* request, const char* name, String& out, bool required) {
  if (!request || !name) {
    return false;
  }
  
  // Check query params first, then form params
  if (request->hasParam(name, true)) {
    out = request->getParam(name, true)->value();
    return true;
  } else if (request->hasParam(name)) {
    out = request->getParam(name)->value();
    return true;
  }
  
  if (required) {
    // Note: This requires the caller to have sendError available
    // For ApiActions/ApiSettings, they will handle the error themselves
    // We just return false here
  }
  return false;
}

bool getParamToBuffer(AsyncWebServerRequest* request, const char* name, 
                      char* buffer, size_t bufferSize, bool required) {
  if (!request || !name || !buffer || bufferSize == 0) {
    return false;
  }
  
  // Initialize buffer to empty string
  buffer[0] = '\0';
  
  const char* value = nullptr;
  
  // Check query params first, then form params
  // Use c_str() to get pointer without creating new String
  if (request->hasParam(name, true)) {
    value = request->getParam(name, true)->value().c_str();
  } else if (request->hasParam(name)) {
    value = request->getParam(name)->value().c_str();
  }
  
  if (value) {
    // Copy value to buffer (truncate if too long)
    size_t len = strlen(value);
    if (len >= bufferSize) {
      len = bufferSize - 1;
    }
    memcpy(buffer, value, len);
    buffer[len] = '\0';
    return true;
  }
  
  return false;
}

bool getIntParam(AsyncWebServerRequest* request, const char* name, 
                 int& out, bool required) {
  if (!request || !name) {
    return false;
  }
  
  const char* value = nullptr;
  
  // Check query params first, then form params
  if (request->hasParam(name, true)) {
    value = request->getParam(name, true)->value().c_str();
  } else if (request->hasParam(name)) {
    value = request->getParam(name)->value().c_str();
  }
  
  if (value && value[0] != '\0') {
    // Parse integer using strtol for proper error handling
    char* endptr;
    long val = strtol(value, &endptr, 10);
    
    // Check if entire string was consumed (valid integer)
    if (*endptr == '\0') {
      out = static_cast<int>(val);
      return true;
    }
  }
  
  return false;
}

bool getBoolParam(AsyncWebServerRequest* request, const char* name, 
                  bool& out, bool required) {
  if (!request || !name) {
    return false;
  }
  
  const char* value = nullptr;
  
  // Check query params first, then form params
  if (request->hasParam(name, true)) {
    value = request->getParam(name, true)->value().c_str();
  } else if (request->hasParam(name)) {
    value = request->getParam(name)->value().c_str();
  }
  
  if (value && value[0] != '\0') {
    // Check for true values
    if (strcmp(value, "true") == 0 || strcmp(value, "1") == 0 || strcmp(value, "yes") == 0) {
      out = true;
      return true;
    }
    // Check for false values
    if (strcmp(value, "false") == 0 || strcmp(value, "0") == 0 || strcmp(value, "no") == 0) {
      out = false;
      return true;
    }
  }
  
  return false;
}

} // namespace WebHelpers
