/**
 * @file SessionManager.cpp
 * @brief Implementation of session token management
 */

#include "SessionManager.h"
#include "DebugLog.h"

#if HOMEWIND_ENABLE_API_ACTIONS

#if HW_ENABLE_WEB
#include <ESPAsyncWebServer.h>
#endif

void SessionManager::generateToken(char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize < 17) {
    if (buffer && bufferSize > 0) buffer[0] = '\0';
    return;
  }
  
  // Generate a simple session token: "session_" + random hex string
  // Format: session_XXXXXXXX where X is hex digit
  // Length: 8 hex digits = 32 bits of randomness
  
  uint32_t random = esp_random(); // ESP32 hardware RNG
  snprintf(buffer, bufferSize, "session_%08x", random);
}

bool SessionManager::validateToken(const char* token) {
  if (!token) return false;
  
  size_t len = strlen(token);
  if (len < 9 || len > 20) {
    return false;
  }
  
  // Check prefix
  if (strncmp(token, "session_", 8) != 0) {
    return false;
  }
  
  // Check hex suffix (after "session_")
  for (size_t i = 8; i < len; i++) {
    char c = token[i];
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  
  return true;
}

void SessionManager::generateOwnerHint(void* request, char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize == 0) return;
  
#if HW_ENABLE_WEB
  AsyncWebServerRequest* req = static_cast<AsyncWebServerRequest*>(request);
  if (!req) {
    strncpy(buffer, "Unknown", bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
    return;
  }
  
  IPAddress ip = req->client()->remoteIP();
  
  // Get User-Agent and extract browser name (no String allocation)
  const char* userAgent = req->header("User-Agent").c_str();
  const char* browser = "Browser";
  
  // Check for browser names using strstr (no String allocation)
  if (strstr(userAgent, "Chrome") != nullptr) {
    browser = "Chrome";
  } else if (strstr(userAgent, "Firefox") != nullptr) {
    browser = "Firefox";
  } else if (strstr(userAgent, "Safari") != nullptr) {
    browser = "Safari";
  } else if (strstr(userAgent, "Edge") != nullptr) {
    browser = "Edge";
  }
  
  snprintf(buffer, bufferSize, "%s (%d.%d.%d.%d)", 
           browser, ip[0], ip[1], ip[2], ip[3]);
#else
  strncpy(buffer, "Unknown", bufferSize - 1);
  buffer[bufferSize - 1] = '\0';
#endif
}

#endif // HOMEWIND_ENABLE_API_ACTIONS

