/**
 * @file WiFiService.cpp
 * @brief Minimal WiFi service implementation
 */

#include "../app/Config.h"

#if HW_ENABLE_WIFI && HW_WIFI_BACKEND_FIXED

#include "WiFiService.h"
#include "../core/DebugLog.h"

WiFiService::WiFiService()
  : initialized(false)
  , staConnected(false)
  , apActive(false)
  , connectStartTime(0)
  , connecting(false)
{
  ipBuffer[0] = '\0';
  modeBuffer[0] = '\0';
}

WiFiService::~WiFiService() {
  // WiFi library handles cleanup
}

void WiFiService::begin() {
  if (initialized) {
    return; // Already initialized
  }
  
  HW_DEBUG_PRINTLN("[WiFiService] Initializing...");
  
  // Set hostname
  WiFi.setHostname(HW_WIFI_HOSTNAME);
  
  // Start in STA mode
  WiFi.mode(WIFI_STA);
  
  // Check if credentials are configured
  const char* ssid = HW_WIFI_SSID;
  const char* pass = HW_WIFI_PASS;
  
  if (ssid && strlen(ssid) > 0 && strcmp(ssid, "CHANGE_ME") != 0) {
    // Start STA connection
    startSTA();
  } else {
    HW_DEBUG_PRINTLN("[WiFiService] No STA credentials configured");
    // If no credentials and AP fallback enabled, start AP immediately
#if HW_WIFI_AP_FALLBACK
    startAP();
#else
    initialized = true; // Mark initialized but not ready
#endif
  }
  
  initialized = true;
}

void WiFiService::loop() {
  if (!initialized) {
    return;
  }
  
  // If connecting via STA, check status
  if (connecting && !staConnected) {
    wl_status_t status = WiFi.status();
    
    if (status == WL_CONNECTED) {
      staConnected = true;
      connecting = false;
      IPAddress ip = WiFi.localIP();
      HW_DEBUG_PRINTF("[WiFiService] STA connected! IP: %d.%d.%d.%d\n", 
                      ip[0], ip[1], ip[2], ip[3]);
    } else if (isTimeout()) {
      // Connection timeout - fallback to AP if enabled
      HW_DEBUG_PRINTLN("[WiFiService] STA connection timeout");
      connecting = false;
#if HW_WIFI_AP_FALLBACK
      if (!apActive) {
        startAP();
      }
#else
      // No AP fallback - WiFi not ready (degraded mode)
      HW_DEBUG_PRINTLN("[WiFiService] STA failed, no AP fallback");
#endif
    }
  }
  
  // Update AP status
  if (apActive) {
    wifi_mode_t mode = WiFi.getMode();
    if (!(mode & WIFI_AP)) {
      // AP was disabled externally
      apActive = false;
    }
  }
}

bool WiFiService::ready() const {
  // Ready if STA connected OR AP active
  // (If AP fallback disabled and STA failed, returns false - degraded mode)
  return staConnected || apActive;
}

bool WiFiService::isSTA() const {
  return staConnected && (WiFi.status() == WL_CONNECTED);
}

bool WiFiService::isAP() const {
  return apActive && (WiFi.getMode() & WIFI_AP);
}

const char* WiFiService::ipString() const {
  IPAddress ip(0, 0, 0, 0);
  
  if (staConnected) {
    ip = WiFi.localIP();
  } else if (apActive) {
    ip = WiFi.softAPIP();
  }
  
  snprintf(ipBuffer, sizeof(ipBuffer), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  return ipBuffer;
}

const char* WiFiService::modeString() const {
  wifi_mode_t mode = WiFi.getMode();
  
  if (mode & WIFI_AP && mode & WIFI_STA) {
    strncpy(modeBuffer, "AP+STA", sizeof(modeBuffer) - 1);
  } else if (mode & WIFI_AP) {
    strncpy(modeBuffer, "AP", sizeof(modeBuffer) - 1);
  } else if (mode & WIFI_STA) {
    strncpy(modeBuffer, "STA", sizeof(modeBuffer) - 1);
  } else {
    strncpy(modeBuffer, "OFF", sizeof(modeBuffer) - 1);
  }
  
  modeBuffer[sizeof(modeBuffer) - 1] = '\0';
  return modeBuffer;
}

void WiFiService::startSTA() {
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(); // Ensure clean state
  
  const char* ssid = HW_WIFI_SSID;
  const char* pass = HW_WIFI_PASS;
  
  // Only use password if provided (empty string means open network)
  if (pass && strlen(pass) > 0 && strcmp(pass, "CHANGE_ME") != 0) {
    WiFi.begin(ssid, pass);
  } else {
    WiFi.begin(ssid);
  }
  
  connecting = true;
  connectStartTime = millis();
  staConnected = false;
  
  HW_DEBUG_PRINTF("[WiFiService] Connecting to STA: %s\n", ssid);
}

void WiFiService::startAP() {
  if (apActive) {
    return; // Already active
  }
  
  HW_DEBUG_PRINTLN("[WiFiService] Starting AP fallback mode");
  
  // Set mode to AP or AP_STA
  wifi_mode_t currentMode = WiFi.getMode();
  if (currentMode == WIFI_STA) {
    WiFi.mode(WIFI_AP_STA); // Keep STA, add AP
  } else {
    WiFi.mode(WIFI_AP);
  }
  
  // Configure and start AP
  const char* apSsid = HW_WIFI_AP_SSID;
  const char* apPass = HW_WIFI_AP_PASS;
  
  bool apStarted;
  if (apPass && strlen(apPass) > 0) {
    apStarted = WiFi.softAP(apSsid, apPass);
  } else {
    apStarted = WiFi.softAP(apSsid); // Open AP
  }
  
  if (apStarted) {
    apActive = true;
    IPAddress apIP = WiFi.softAPIP();
    HW_DEBUG_PRINTF("[WiFiService] AP started: %s (IP: %d.%d.%d.%d)\n", 
                    apSsid, apIP[0], apIP[1], apIP[2], apIP[3]);
  } else {
    HW_DEBUG_PRINTLN("[WiFiService] AP start failed");
    apActive = false;
  }
}

bool WiFiService::isTimeout() const {
  if (!connecting || connectStartTime == 0) {
    return false;
  }
  
  unsigned long elapsed = millis() - connectStartTime;
  return elapsed >= HW_WIFI_TIMEOUT_MS;
}

#endif // HW_ENABLE_WIFI

