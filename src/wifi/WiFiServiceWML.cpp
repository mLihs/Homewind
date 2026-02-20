/**
 * @file WiFiServiceWML.cpp
 * @brief WiFi service implementation using wifiMangerLite
 */

#include "../app/Config.h"

#if HW_ENABLE_WIFI && HW_WIFI_BACKEND_WML

#include "WiFiServiceWML.h"
#include "../core/DebugLog.h"
#include "../core/SystemState.h"
#include "../app/BuildInfo.h"
#include <new>  // For placement-new
#include <WiFi.h>
#include <ESPmDNS.h>

// Static instances for wifiMangerLite components
// These live for the entire program lifetime
static WML::WiFiManagerLite s_wifiManager;
static WML::Storage s_storage("hwwifi", "cfg");
static WML::StorageProvider s_configProvider(s_storage);

// Static buffer for CaptivePortal (placement-new, no heap allocation)
// Portal is created when begin() is called with server reference
alignas(WML::CaptivePortal) static uint8_t s_portalBuffer[sizeof(WML::CaptivePortal)];
static WML::CaptivePortal* s_portal = nullptr;

WiFiServiceWML::WiFiServiceWML()
  : initialized(false)
  , wifiManager(&s_wifiManager)
  , portal(nullptr)
  , storage(&s_storage)
  , configProvider(&s_configProvider)
  , lastStaConnected(false)
  , lastApActive(false)
{
  ipBuffer[0] = '\0';
  modeBuffer[0] = '\0';
}

WiFiServiceWML::~WiFiServiceWML() {
  // Portal cleanup if needed
  if (s_portal) {
    // Explicit destructor call for placement-new object
    s_portal->~CaptivePortal();
    s_portal = nullptr;
  }
}

void WiFiServiceWML::begin(AsyncWebServer* server) {
  if (initialized || !server) {
    return;
  }
  
  HW_DEBUG_PRINTLN("[WiFiWML] Initializing wifiMangerLite...");
  
  // Set identity base name (for AP SSID + mDNS)
  wifiManager->setIdentityBaseName(HW_WIFI_IDENTITY_BASE);
  
  // Load saved config from NVS
  WML::Config config;
  bool hasCredentials = storage->load(config);
  
  if (hasCredentials) {
    HW_DEBUG_PRINTLN("[WiFiWML] Loaded credentials from NVS");
    HW_DEBUG_PRINTF("[WiFiWML]   SSID: %s\n", config.primary.ssid);
    HW_DEBUG_PRINTF("[WiFiWML]   deviceName (from NVS): %s\n", config.deviceName);
    
    // Migration check: If deviceName doesn't start with our identity base (lowercase),
    // it's a stale value from an older install or ESP default. Fix it once and re-save.
    const String identityHostname = wifiManager->getIdentityNameWithMac();
    char expectedPrefix[32];
    strncpy(expectedPrefix, HW_WIFI_IDENTITY_BASE, sizeof(expectedPrefix) - 1);
    expectedPrefix[sizeof(expectedPrefix) - 1] = '\0';
    // Convert to lowercase for comparison
    for (size_t i = 0; expectedPrefix[i]; i++) {
      if (expectedPrefix[i] >= 'A' && expectedPrefix[i] <= 'Z') {
        expectedPrefix[i] = expectedPrefix[i] + ('a' - 'A');
      }
    }
    
    // Check if current deviceName starts with expected prefix (e.g., "homewind")
    bool needsMigration = (strncmp(config.deviceName, expectedPrefix, strlen(expectedPrefix)) != 0);
    
    if (needsMigration && identityHostname.length() > 0) {
      HW_DEBUG_PRINTF("[WiFiWML]   deviceName needs migration (doesn't start with '%s')\n", expectedPrefix);
      
      // Build correct hostname (lowercase)
      char hostnameBuf[64];
      strncpy(hostnameBuf, identityHostname.c_str(), sizeof(hostnameBuf) - 1);
      hostnameBuf[sizeof(hostnameBuf) - 1] = '\0';
      for (size_t i = 0; hostnameBuf[i]; i++) {
        if (hostnameBuf[i] >= 'A' && hostnameBuf[i] <= 'Z') {
          hostnameBuf[i] = hostnameBuf[i] + ('a' - 'A');
        }
      }
      config.setDeviceName(hostnameBuf);
      HW_DEBUG_PRINTF("[WiFiWML]   deviceName migrated to: %s\n", config.deviceName);
      
      // Re-save to NVS so migration only happens once
      if (configProvider->updateConfig(config)) {
        HW_DEBUG_PRINTLN("[WiFiWML]   Migration saved to NVS");
      }
    }
    
    wifiManager->setConfig(config);
  } else {
    HW_DEBUG_PRINTLN("[WiFiWML] No saved credentials - will start AP mode");
  }
  
  // Placement-new: construct in static buffer (no heap allocation)
  s_portal = new (s_portalBuffer) WML::CaptivePortal(*server, *wifiManager);
  portal = s_portal;
  
  // Setup portal configuration
  portal->setDeviceName(HW_WIFI_IDENTITY_BASE);
  portal->setFirmwareVersion(BuildInfo::getVersion());
  portal->setRestartAfterSave(true);  // Restart after config save
  
  // Setup callbacks
  setupCallbacks();
  
  // Register event handler for state tracking
  wifiManager->onEvent([this](WML::Event event, const String& info) {
    switch (event) {
      case WML::Event::Connected:
        HW_DEBUG_PRINTF("[WiFiWML] Connected to: %s\n", info.c_str());
        HW_DEBUG_PRINTF("[WiFiWML]   IP: %s\n", ipString());
        {
          // Build the correct hostname: "homewind-XXXXXXXX" (lowercase)
          // Use ESP MAC directly to ensure consistency
          const uint32_t mac32 = static_cast<uint32_t>(ESP.getEfuseMac() & 0xFFFFFFFFu);
          char hostnameBuf[64];
          snprintf(hostnameBuf, sizeof(hostnameBuf), "%s-%08lx", HW_WIFI_HOSTNAME, static_cast<unsigned long>(mac32));
          
          HW_DEBUG_PRINTF("[WiFiWML]   Setting mDNS hostname: %s\n", hostnameBuf);
          
          // Stop any existing mDNS
          MDNS.end();
          
          // Start mDNS with our hostname
          if (MDNS.begin(hostnameBuf)) {
            MDNS.addService("http", "tcp", 80);
            HW_DEBUG_PRINTF("[WiFiWML]   mDNS started: %s.local\n", hostnameBuf);
          } else {
            HW_DEBUG_PRINTLN("[WiFiWML]   ERROR: Failed to start mDNS!");
          }
        }
        SystemState::setWiFiReady(true);
        SystemState::setStationOnline(true);
        break;
        
      case WML::Event::Disconnected:
        HW_DEBUG_PRINTLN("[WiFiWML] Disconnected");
        SystemState::setStationOnline(false);
        // WiFiReady stays true if AP is active
        if (!wifiManager->isAPMode()) {
          SystemState::setWiFiReady(false);
        }
        break;
        
      case WML::Event::APStarted:
        HW_DEBUG_PRINTF("[WiFiWML] AP started: %s\n", info.c_str());
        SystemState::setWiFiReady(true);
        break;
        
      case WML::Event::APStopped:
        HW_DEBUG_PRINTLN("[WiFiWML] AP stopped");
        if (!wifiManager->isConnected()) {
          SystemState::setWiFiReady(false);
        }
        break;
        
      case WML::Event::ConnectionFailed:
        HW_DEBUG_PRINTF("[WiFiWML] Connection failed: %s\n", info.c_str());
        break;
        
      default:
        break;
    }
  });
  
  // Start wifiManager (will try STA if credentials exist, else AP)
  wifiManager->begin();
  
  // Start portal routes (under /wml/*)
  portal->begin();
  
  initialized = true;
  HW_DEBUG_PRINTLN("[WiFiWML] Initialized");
  
  // Log effective config for debugging
  HW_DEBUG_PRINTF("[WiFiWML] Effective config:\n");
  HW_DEBUG_PRINTF("[WiFiWML]   mDNS hostname: %s.local\n", wifiManager->getIdentityNameWithMac().c_str());
  HW_DEBUG_PRINTF("[WiFiWML]   Identity base: %s\n", wifiManager->getIdentityBaseName().c_str());
  
  // Set initial state
  if (wifiManager->isConnected()) {
    SystemState::setWiFiReady(true);
    SystemState::setStationOnline(true);
    HW_DEBUG_PRINTF("[WiFiWML] Already connected, IP: %s\n", ipString());
  } else if (wifiManager->isAPMode()) {
    SystemState::setWiFiReady(true);
    SystemState::setStationOnline(false);
    HW_DEBUG_PRINTF("[WiFiWML] AP mode active, IP: %s\n", ipString());
  }
}

void WiFiServiceWML::loop() {
  if (!initialized) {
    return;
  }
  
  // Process wifiManager tasks
  wifiManager->loop();
  
  // Process portal tasks
  if (portal) {
    portal->loop();
  }
  
  // Track state changes for UI updates
  bool currentSta = wifiManager->isConnected();
  bool currentAp = wifiManager->isAPMode();
  
  if (currentSta != lastStaConnected) {
    lastStaConnected = currentSta;
    SystemState::setStationOnline(currentSta);
  }
  
  if (currentAp != lastApActive || currentSta != lastStaConnected) {
    lastApActive = currentAp;
    SystemState::setWiFiReady(currentSta || currentAp);
  }
}

void WiFiServiceWML::setupCallbacks() {
  if (!portal) return;
  
  // Called when portal needs current config
  portal->onConfigGet([this]() {
    return configProvider->getConfig();
  });
  
  // Called when user saves new config
  portal->onConfigChange([this](const WML::Config& newConfig) -> bool {
    HW_DEBUG_PRINTLN("[WiFiWML] Config changed, saving to NVS...");
    
    // Make a mutable copy to fix the deviceName before saving
    WML::Config configToSave = newConfig;
    
    // Ensure deviceName is set to the correct identity hostname (lowercase)
    // This prevents stale/default names like "esp32s3-DABB8C" from being saved
    const String identityHostname = wifiManager->getIdentityNameWithMac();
    if (identityHostname.length() > 0) {
      char hostnameBuf[64];
      strncpy(hostnameBuf, identityHostname.c_str(), sizeof(hostnameBuf) - 1);
      hostnameBuf[sizeof(hostnameBuf) - 1] = '\0';
      // Convert to lowercase (mDNS hostnames should be lowercase)
      for (size_t i = 0; hostnameBuf[i]; i++) {
        if (hostnameBuf[i] >= 'A' && hostnameBuf[i] <= 'Z') {
          hostnameBuf[i] = hostnameBuf[i] + ('a' - 'A');
        }
      }
      configToSave.setDeviceName(hostnameBuf);
      HW_DEBUG_PRINTF("[WiFiWML]   deviceName set to: %s\n", configToSave.deviceName);
    }
    
    bool saved = configProvider->updateConfig(configToSave);
    if (saved) {
      HW_DEBUG_PRINTLN("[WiFiWML] Config saved successfully");
      HW_DEBUG_PRINTF("[WiFiWML]   SSID: %s\n", configToSave.primary.ssid);
      HW_DEBUG_PRINTF("[WiFiWML]   mDNS hostname: %s.local\n", configToSave.deviceName);
    } else {
      HW_DEBUG_PRINTLN("[WiFiWML] Failed to save config");
    }
    return saved;  // Return true to accept and restart
  });
  
  // Called on factory reset
  portal->onFactoryReset([this]() {
    HW_DEBUG_PRINTLN("[WiFiWML] Factory reset - clearing WiFi credentials");
    storage->clear();
  });
  
  // Called on WiFi reset (just clear WiFi, not full factory reset)
  portal->onWiFiReset([this]() {
    HW_DEBUG_PRINTLN("[WiFiWML] WiFi reset - clearing credentials");
    storage->clear();
  });
}

bool WiFiServiceWML::wifiReset() {
  if (!initialized || !wifiManager || !storage) {
    return false;
  }
  HW_DEBUG_PRINTLN("[WiFiWML] WiFi reset (API) via factoryReset(callback, true)");
  wifiManager->factoryReset([this]() {
    storage->clear();
  }, true);  // restart = true; WML schedules ESP restart
  return true;
}

bool WiFiServiceWML::ready() const {
  if (!initialized || !wifiManager) {
    return false;
  }
  // Level A: AP or STA
  return wifiManager->isConnected() || wifiManager->isAPMode();
}

bool WiFiServiceWML::isSTA() const {
  if (!initialized || !wifiManager) {
    return false;
  }
  // Level B: STA only
  return wifiManager->isConnected();
}

bool WiFiServiceWML::isAP() const {
  if (!initialized || !wifiManager) {
    return false;
  }
  return wifiManager->isAPMode();
}

const char* WiFiServiceWML::ipString() const {
  if (!initialized || !wifiManager) {
    strncpy(ipBuffer, "0.0.0.0", sizeof(ipBuffer));
    return ipBuffer;
  }
  
  IPAddress ip;
  if (wifiManager->isConnected()) {
    ip = wifiManager->getStationIP();
  } else if (wifiManager->isAPMode()) {
    ip = wifiManager->getAPIP();
  } else {
    ip = IPAddress(0, 0, 0, 0);
  }
  
  snprintf(ipBuffer, sizeof(ipBuffer), "%d.%d.%d.%d", ip[0], ip[1], ip[2], ip[3]);
  return ipBuffer;
}

const char* WiFiServiceWML::modeString() const {
  if (!initialized || !wifiManager) {
    strncpy(modeBuffer, "OFF", sizeof(modeBuffer));
    return modeBuffer;
  }
  
  bool sta = wifiManager->isConnected();
  bool ap = wifiManager->isAPMode();
  
  if (sta && ap) {
    strncpy(modeBuffer, "AP+STA", sizeof(modeBuffer) - 1);
  } else if (ap) {
    strncpy(modeBuffer, "AP", sizeof(modeBuffer) - 1);
  } else if (sta) {
    strncpy(modeBuffer, "STA", sizeof(modeBuffer) - 1);
  } else {
    strncpy(modeBuffer, "OFF", sizeof(modeBuffer) - 1);
  }
  
  modeBuffer[sizeof(modeBuffer) - 1] = '\0';
  return modeBuffer;
}

bool WiFiServiceWML::isPortalActive() const {
  // Portal is "active" when in AP mode (setup needed)
  return isAP() && !isSTA();
}

#endif // HW_ENABLE_WIFI && HW_WIFI_BACKEND_WML
