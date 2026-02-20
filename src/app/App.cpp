/**
 * @file App.cpp
 * @brief Main application coordinator with deterministic boot orchestrator
 * 
 * MEMORY MODEL (Stufe 3):
 * All subsystems are statically allocated (no heap) for deterministic lifetimes.
 * This eliminates fragmentation risks from new/delete cycles on ESP32.
 */

#include "App.h"
#include "../core/SystemState.h"
#include "../core/Timing.h"
#include "../core/DebugLog.h"
#include "../core/HeapDiagnostics.h"

#if HW_ENABLE_NVS
#include "../settings/SettingsStore.h"
#endif

#if HW_ENABLE_WEB
#include "../web/WebServer.h"
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
#endif

#if HW_ENABLE_WIFI
  #include <WiFi.h>  // Required for WiFi.mode() in all backends
  #if HW_WIFI_BACKEND_FIXED
    #include "../wifi/WiFiService.h"
  #elif HW_WIFI_BACKEND_WML
    #include "../wifi/WiFiServiceWML.h"
  #endif
#endif

#if HW_ENABLE_BLE
#include "../core/BLERelayManager.h"
#include <BluetoothBikeSensorServer.h>
#endif

#if HW_ENABLE_FANS
#include "../core/FanController.h"
#if HW_ENABLE_BLE
#include "../core/FanHRAdapter.h"
#endif
#endif

#if HW_ENABLE_OTA
#include "../core/FirmwareUpdateManager.h"
#endif

#if HW_ENABLE_NVS
#include "../core/HeartRateSettings.h"
#endif

#if HOMEWIND_ENABLE_API_ACTIONS
#include "../web/ApiActions.h"
#endif

#if HOMEWIND_ENABLE_API_SETTINGS
#include "../web/ApiSettings.h"
#endif

#if HW_ENABLE_DISPLAY
#include "../core/DisplayManager.h"
#endif

// ============================================================================
// STATIC SUBSYSTEM INSTANCES (no heap allocation)
// ============================================================================
// These objects live for the entire program lifetime. 
// Initialization happens in begin(), cleanup (if needed) in end().
// This model ensures deterministic memory usage and no fragmentation.

#if HW_ENABLE_NVS
static SettingsStore s_settings;
#endif

#if HW_ENABLE_WIFI
  #if HW_WIFI_BACKEND_FIXED
    static WiFiService s_wifi;
  #elif HW_WIFI_BACKEND_WML
    static WiFiServiceWML s_wifiWML;
  #endif
#endif

#if HW_ENABLE_WEB
static WebServerManager s_webServer;
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
static WebSocketTelemetry s_telemetry;
#endif

#if HW_ENABLE_BLE
static BLERelayManager s_bleManager;
#endif

#if HW_ENABLE_FANS
static FanController s_fanController;
#if HW_ENABLE_BLE
static FanHRAdapter s_fanHrAdapter;
#endif
#endif

#if HW_ENABLE_OTA
static FirmwareUpdateManager s_firmwareUpdateManager;
#endif

#if HW_ENABLE_NVS
static HeartRateSettings s_heartRateSettings;
#endif

#if HW_ENABLE_DISPLAY
static DisplayManager s_displayManager;
#endif

// ============================================================================
// GLOBAL POINTERS (for cross-module access)
// ============================================================================
// These point to the static instances above. Set during init, never changed.

#if HW_ENABLE_FANS
FanController* g_fanController = nullptr;
#if HW_ENABLE_BLE
FanHRAdapter* g_fanHrAdapter = nullptr;
#endif
#endif

#if HW_ENABLE_OTA
FirmwareUpdateManager* g_firmwareUpdateManager = nullptr;
#endif

#if HW_ENABLE_NVS
HeartRateSettings* g_heartRateSettings = nullptr;
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
WebSocketTelemetry* g_webSocketTelemetry = nullptr;
#endif

#if HW_ENABLE_BLE
BLERelayManager* g_bleRelayManager = nullptr;
#endif

#if HW_ENABLE_DISPLAY
// g_displayManager defined in DisplayManager.cpp
#endif

App::App()
  : initialized(false)
  , currentPhase(BootPhase::PH_CORE)
  , phaseStartTime(0)
  , bootStartTime(0)
#if HW_ENABLE_NVS
  , settings(nullptr)
  , nvsReady(false)
#endif
#if HW_ENABLE_WIFI
  #if HW_WIFI_BACKEND_FIXED
  , wifi(nullptr)
  #endif
  , wifiReady(false)
#endif
#if HW_ENABLE_WEB
  , webServer(nullptr)
  , webReady(false)
#endif
#if HOMEWIND_ENABLE_WEBSOCKET
  , telemetry(nullptr)
  , wsReady(false)
#endif
#if HW_ENABLE_BLE
  , bleManager(nullptr)
  , bleReady(false)
#endif
#if HW_ENABLE_FANS
  , fanController(nullptr)
  , fansReady(false)
#endif
#if HW_ENABLE_OTA
  , firmwareUpdateManager(nullptr)
  , otaReady(false)
#endif
#if HW_ENABLE_NVS
  , heartRateSettings(nullptr)
  , heartRateSettingsReady(false)
#endif
{
}

App::~App() {
  // Cleanup handled by RAII or explicit cleanup if needed
}

bool App::begin() {
  if (initialized) {
    return true;
  }
  
#if HW_ENABLE_SERIAL_COMMANDS
  HeapDiagnostics::hwHeapDiagCaptureBoot();
#endif
  
  bootStartTime = millis();
  phaseStartTime = bootStartTime;
  currentPhase = BootPhase::PH_CORE;
  initialized = true;
  
  HW_DEBUG_PRINTF("[App] Boot orchestrator started (Homewind v%s)\n", BuildInfo::getVersion());
  
  // Initialize core immediately (non-blocking)
  initCore();
  
  return true;
}

void App::loop() {
  if (!initialized) {
    return;
  }
  
  // Handle serial debug commands (if enabled)
#if HW_ENABLE_SERIAL_COMMANDS
  handleSerialCommands();
#endif
  
  // Update system state first
  SystemState::update();
  
  // Advance boot phase if not yet running
  if (currentPhase != BootPhase::PH_RUNNING && currentPhase != BootPhase::PH_DEGRADED) {
    advanceBootPhase();
  }
  
  // Loop subsystems (safe to call even if not fully initialized)
  loopSubsystems();
  
#if HW_ENABLE_SERIAL_COMMANDS
  // Heap diagnostics runtime tick (every 30s with operational status)
  if (currentPhase == BootPhase::PH_RUNNING) {
    HeapDiagnostics::OperationalStatus status;
#if HOMEWIND_ENABLE_WEBSOCKET
    if (telemetry) {
      status.wsClientCount = telemetry->getClientCount();
    }
#endif
#if HW_ENABLE_BLE
    if (bleManager) {
      status.bleSensorsConnected = bleManager->getConnectedSensorCount();
      status.bleSensorsConfigured = bleManager->getConfiguredSensorCount();
      status.bleDiscoveryCount = bleManager->getLastDiscoveryCount();
      // Use BikeSensorServerIsScanning() to check actual BLE scan status (includes server-mode scan)
      status.bleScanning = BikeSensorServerIsScanning();
    }
#endif
#if HW_ENABLE_FANS
    if (fanController) {
      status.fansConnected = fanController->getConnectedFanCount();
      status.fansConfigured = fanController->getFanCount();
    }
#endif
    HeapDiagnostics::hwHeapDiagRuntimeTick(&status);
  }
#endif
}

#if HW_ENABLE_SERIAL_COMMANDS
void App::handleSerialCommands() {
  if (!Serial.available()) {
    return;
  }
  
  char cmd = Serial.read();
  
  switch (cmd) {
    case 'h':
    case 'H': {
      HeapDiagnostics::hwHeapDiagPrintHeapInfo(Serial);
      break;
    }
      
    case 's':
    case 'S':
      // System status
      Serial.println(F("\n===== SYSTEM STATUS ====="));
      Serial.printf("Version:    %s\n", BuildInfo::getVersion());
      Serial.printf("Uptime:     %lu ms\n", millis());
      Serial.printf("Boot Phase: %d\n", static_cast<int>(currentPhase));
#if HW_ENABLE_WIFI
  #if HW_WIFI_BACKEND_FIXED
      Serial.printf("WiFi:       %s (%s)\n", wifi ? wifi->ipString() : "N/A", wifi ? wifi->modeString() : "N/A");
  #elif HW_WIFI_BACKEND_WML
      Serial.printf("WiFi:       %s (%s)\n", s_wifiWML.ipString(), s_wifiWML.modeString());
  #endif
#endif
      Serial.println(F("=========================\n"));
      break;
      
    case '?':
      // Help
      Serial.println(F("\n===== SERIAL COMMANDS ====="));
      Serial.println(F("h - Heap info (memory, fragmentation)"));
      Serial.println(F("s - System status (version, uptime, WiFi)"));
      Serial.println(F("? - Show this help"));
      Serial.println(F("===========================\n"));
      break;
      
    case '\n':
    case '\r':
      // Ignore newlines
      break;
      
    default:
      // Unknown command - show hint
      Serial.printf("[Serial] Unknown command '%c'. Type '?' for help.\n", cmd);
      break;
  }
}
#endif

bool App::isReady() const {
  return SystemState::isReady();
}

void App::advanceBootPhase() {
  unsigned long now = millis();
  
  // Check phase timeout
  if (isPhaseTimeout()) {
    HW_DEBUG_PRINTF("[App] Phase timeout in phase %d\n", static_cast<int>(currentPhase));
    // Move to next phase or degraded mode
  }
  
  switch (currentPhase) {
    case BootPhase::PH_CORE:
      // Core is initialized immediately in begin()
      phaseStartTime = now;
      currentPhase = BootPhase::PH_NVS;
      HW_DEBUG_PRINTLN("[App] → PH_NVS");
      initNVS();
      break;
      
    case BootPhase::PH_NVS:
#if HW_ENABLE_NVS
      if (nvsReady) {
        phaseStartTime = now;
        currentPhase = BootPhase::PH_WIFI;
        HW_DEBUG_PRINTLN("[App] → PH_WIFI");
        initWiFi();
      } else if (isPhaseTimeout()) {
        // NVS failed but continue (degraded mode)
        HW_DEBUG_PRINTLN("[App] NVS timeout, continuing without NVS");
        phaseStartTime = now;
        currentPhase = BootPhase::PH_WIFI;
        initWiFi();
      }
#else
      // Skip NVS if disabled
      phaseStartTime = now;
      currentPhase = BootPhase::PH_WIFI;
      HW_DEBUG_PRINTLN("[App] → PH_WIFI (NVS disabled)");
      initWiFi();
#endif
      break;
      
    case BootPhase::PH_WIFI:
#if HW_ENABLE_WIFI
      if (wifiReady) {
        phaseStartTime = now;
        currentPhase = BootPhase::PH_WEB;
        HW_DEBUG_PRINTLN("[App] → PH_WEB");
        initWeb();
      } else if (isPhaseTimeout() || (now - phaseStartTime) > HW_WIFI_TIMEOUT_MS) {
#if HW_WIFI_BACKEND_FIXED
        // Fixed backend: Check if AP fallback activated
        if (wifi && wifi->ready()) {
          // AP fallback is active, continue normally
          wifiReady = true;
          SystemState::setWiFiReady(true);
          phaseStartTime = now;
          currentPhase = BootPhase::PH_WEB;
          HW_DEBUG_PRINTLN("[App] → PH_WEB (WiFi AP fallback active)");
          initWeb();
        } else {
          // WiFi failed, enter degraded mode but still start web server
          HW_DEBUG_PRINTLN("[App] WiFi timeout, entering degraded mode");
          phaseStartTime = now;
          currentPhase = BootPhase::PH_WEB;
          initWeb();
        }
#elif HW_WIFI_BACKEND_WML
        // WML backend: wifiReady is already set in initWiFi(), just proceed to web
        // The actual WiFi connection happens after WebServer is up (portal needs server)
        HW_DEBUG_PRINTLN("[App] WiFi (WML) timeout - proceeding to web phase");
        phaseStartTime = now;
        currentPhase = BootPhase::PH_WEB;
        initWeb();
#endif
      }
#else
      // Skip WiFi if disabled
      phaseStartTime = now;
      currentPhase = BootPhase::PH_WEB;
      HW_DEBUG_PRINTLN("[App] → PH_WEB (WiFi disabled)");
      initWeb();
#endif
      break;
      
    case BootPhase::PH_WEB:
#if HW_ENABLE_WEB
      if (webReady) {
        phaseStartTime = now;
        currentPhase = BootPhase::PH_RUNNING;
        HW_DEBUG_PRINTLN("[App] → PH_RUNNING");

#if HW_ENABLE_DISPLAY
        if (g_displayManager) {
#if HW_WIFI_BACKEND_WML
          if (s_wifiWML.isAP() && !s_wifiWML.isSTA()) {
            g_displayManager->showAPScreen();
          } else {
            g_displayManager->showMainScreen();
          }
#else
          g_displayManager->showMainScreen();
#endif
        }
#endif

#if HW_ENABLE_SERIAL_COMMANDS
        HeapDiagnostics::hwHeapDiagCaptureReady();
#endif
        
#if HW_ENABLE_BLE && HW_BLE_PREINIT_NIMBLE_STACK
        // Two-phase BLE init experiment:
        // Pre-initialize ONLY the NimBLE stack early (minimal), start full server later in initBLE().
        // NOTE: This is intentionally done at PH_RUNNING entry even if STA is not yet online
        // (WML may still be connecting). Goal: place large NimBLE allocations early for better heap locality.
        static bool s_nimbleStackPreinited = false;
        if (!s_nimbleStackPreinited) {
          s_nimbleStackPreinited = true;
#if HW_WIFI_BACKEND_WML
          HW_DEBUG_PRINTF("[App] Pre-initializing NimBLE stack (two-phase BLE init, STA online=%d)...\n",
                          SystemState::isStationOnline() ? 1 : 0);
#else
          HW_DEBUG_PRINTLN("[App] Pre-initializing NimBLE stack (two-phase BLE init)...");
#endif
          BluetoothBikeSensorServerInitStack(
            "homewind",
            SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE,
            BBS_MODE_MAC
          );
        }
#if HW_ENABLE_SERIAL_COMMANDS
        HeapDiagnostics::hwHeapDiagPrintTick("BLE_STACK_ONLY");
#endif
#endif
        // Update system state to mark services ready
        SystemState::setServicesReady(true);
        
        // Start background services based on WiFi backend
        // Optimized sequence: FirmwareUpdate (small, needs WebServer) → Fans (small) → BLE (large, after SmartConnect)
#if HW_WIFI_BACKEND_FIXED
        // DEV mode: Start operational services immediately
        initOTA();  // First: Small (12KB stack, no heap), needs WebServer
        initFans(); // Second: Small (~8KB heap), SmartConnect starts async
        // BLE deferred until SmartConnect completes (initialized in loopSubsystems())
#elif HW_WIFI_BACKEND_WML
        // PROD mode: Operational services only when STA connected (Level B)
        // They will be started in loopSubsystems() when station comes online
        if (SystemState::isStationOnline()) {
          HW_DEBUG_PRINTLN("[App] Station online - starting operational services");
          initOTA();  // First: Small (12KB stack, no heap), needs WebServer
          initFans(); // Second: Small (~8KB heap), SmartConnect starts async
          // BLE deferred until SmartConnect completes (initialized in loopSubsystems())
        } else {
          HW_DEBUG_PRINTLN("[App] Station offline - operational services deferred until STA connected");
        }
#endif
      } else if (isPhaseTimeout()) {
        // Web server failed - enter degraded mode
        HW_DEBUG_PRINTLN("[App] Web server timeout, entering degraded mode");
        enterDegradedMode();
      }
#else
      // Skip web if disabled, go straight to running
      phaseStartTime = now;
      currentPhase = BootPhase::PH_RUNNING;
      HW_DEBUG_PRINTLN("[App] → PH_RUNNING (Web disabled)");

#if HW_ENABLE_DISPLAY
      if (g_displayManager) {
#if HW_WIFI_BACKEND_WML
        if (s_wifiWML.isAP() && !s_wifiWML.isSTA()) {
          g_displayManager->showAPScreen();
        } else {
          g_displayManager->showMainScreen();
        }
#else
        g_displayManager->showMainScreen();
#endif
      }
#endif

#if HW_ENABLE_SERIAL_COMMANDS
      HeapDiagnostics::hwHeapDiagCaptureReady();
#endif
      SystemState::setServicesReady(true);
      // Optimized sequence: FirmwareUpdate (small) → Fans (small) → BLE (large, after SmartConnect)
      initOTA();  // First: Small (12KB stack, no heap), needs WebServer
      initFans(); // Second: Small (~8KB heap), SmartConnect starts async
      // BLE deferred until SmartConnect completes (initialized in loopSubsystems())
#endif
      break;
      
    case BootPhase::PH_RUNNING:
      // Already running, nothing to do
      break;
      
    case BootPhase::PH_DEGRADED:
      // Degraded mode, subsystems may still work
      break;
      
    default:
      break;
  }
}

void App::initCore() {
  HW_DEBUG_PRINTLN("[App] Initializing core...");

#if HW_ENABLE_DISPLAY
  g_displayManager = &s_displayManager;
  if (g_displayManager->begin()) {
    HW_DEBUG_PRINTLN("[App] Display initialized");
  }
#endif

  // Core initialization is complete
  HW_DEBUG_PRINTLN("[App] Core initialized");
}

void App::initNVS() {
#if HW_ENABLE_NVS
  if (nvsReady) {
    return; // Already initialized
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing NVS...");
  settings = &s_settings;  // Point to static instance
  if (settings->begin()) {
    nvsReady = true;
    SystemState::setNVSReady(true);
    HW_DEBUG_PRINTLN("[App] NVS ready");
#if HW_ENABLE_SERIAL_COMMANDS
    HeapDiagnostics::hwHeapDiagPrintTick("NVS");
#endif
    
    // Set settings store for ApiActions factory reset
#if HOMEWIND_ENABLE_API_ACTIONS && HOMEWIND_ENABLE_SETTINGS
    ApiActions::setSettingsStore(settings);
#endif
  } else {
    HW_DEBUG_PRINTLN("[App] NVS initialization failed");
    SystemState::setNVSReady(false);
    // Note: settings pointer remains valid (static instance), just not initialized
  }
  
  // Initialize heart rate settings after NVS is ready
  initHeartRateSettings();
#endif
}

void App::initWiFi() {
#if HW_ENABLE_WIFI
  if (wifiReady) {
    return; // Already initialized
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing WiFi...");
  
#if HW_WIFI_BACKEND_FIXED
  // DEV mode: Fixed credentials via WiFiService
  wifi = &s_wifi;  // Point to static instance
  wifi->begin();
  
  // Initial ready state check
  wifiReady = wifi->ready();
  SystemState::setWiFiReady(wifiReady);
  SystemState::setStationOnline(wifi->isSTA());
  
  if (wifiReady) {
    HW_DEBUG_PRINTF("[App] WiFi ready (%s): %s\n", wifi->modeString(), wifi->ipString());
#if HW_ENABLE_DISPLAY
    if (g_displayManager && wifi->isSTA()) {
      char url[64];
      snprintf(url, sizeof(url), "http://%s", wifi->ipString());
      g_displayManager->setUrl(url);
    }
#endif
  }

#elif HW_WIFI_BACKEND_WML
  // PROD mode: wifiMangerLite with Captive Portal
  // Note: WML needs the WebServer pointer, so we defer full initialization
  // until after WebServer is created. This is handled in initWeb().
  
  // IMPORTANT: We MUST initialize WiFi hardware before WebServer starts!
  // ESPAsyncWebServer requires WiFi.mode() to be set, otherwise FreeRTOS 
  // semaphore assertions fail.
  HW_DEBUG_PRINTLN("[App] WiFi (WML) pre-init: setting WiFi mode...");
  // Use STA-only here to avoid early AP+STA allocations/fragmentation.
  // wifiMangerLite will switch to WIFI_AP_STA later if it actually enters AP mode.
  WiFi.mode(WIFI_STA);
  
  HW_DEBUG_PRINTLN("[App] WiFi (WML) full init will happen after WebServer");
  
  // Mark as ready for now - WML handles its own state
  // The actual portal/connection happens after WebServer is up
  wifiReady = true;  // Allow transition to PH_WEB
  SystemState::setWiFiReady(true);
#endif

#endif // HW_ENABLE_WIFI
}

void App::initWeb() {
#if HW_ENABLE_WEB
  if (webReady) {
    return; // Already initialized
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing Web server...");
  webServer = &s_webServer;  // Point to static instance
  
  if (webServer->begin()) {
    webReady = true;
    SystemState::setWebReady(true);
    HW_DEBUG_PRINTLN("[App] Web server ready");
#if HW_ENABLE_SERIAL_COMMANDS
    HeapDiagnostics::hwHeapDiagPrintTick("WebServer");
#endif
    
    // Register API routes if available
#if HOMEWIND_ENABLE_API_ACTIONS
    static ApiActions s_apiActions;
    s_apiActions.registerRoutes(webServer->getServer());
#endif

#if HOMEWIND_ENABLE_API_SETTINGS
    static ApiSettings s_apiSettings;
    s_apiSettings.registerRoutes(webServer->getServer(), this->settings);
#endif

    // Initialize WML WiFi backend AFTER WebServer is ready
    // (Portal needs server pointer for /wml/* routes)
#if HW_WIFI_BACKEND_WML
    HW_DEBUG_PRINTLN("[App] Initializing WiFi (WML) with WebServer...");
    s_wifiWML.begin(webServer->getServer());
#if HOMEWIND_ENABLE_API_ACTIONS
    ApiActions::setWiFiServiceWML(&s_wifiWML);
#endif
    // Update state based on WML status
    SystemState::setWiFiReady(s_wifiWML.ready());
    SystemState::setStationOnline(s_wifiWML.isSTA());
    
    if (s_wifiWML.isSTA()) {
      HW_DEBUG_PRINTF("[App] WiFi ready (STA): %s\n", s_wifiWML.ipString());
    } else if (s_wifiWML.isAP()) {
      HW_DEBUG_PRINTF("[App] WiFi Portal active (AP): %s\n", s_wifiWML.ipString());
    }
#endif

  } else {
    HW_DEBUG_PRINTLN("[App] Web server initialization failed");
    SystemState::setWebReady(false);
  }
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
  if (wsReady) {
    return; // Already initialized
  }
  
  // WebSocket requires WebServer to be ready
  if (!webReady || !webServer) {
    HW_DEBUG_PRINTLN("[App] WebSocket telemetry skipped - WebServer not ready");
    return;
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing WebSocket telemetry...");
  telemetry = &s_telemetry;  // Point to static instance
  g_webSocketTelemetry = telemetry;
  
  AsyncWebServer* server = webServer->getServer();
  if (telemetry->begin(server)) {
    wsReady = true;
    SystemState::setWebSocketReady(true);
    HW_DEBUG_PRINTLN("[App] WebSocket telemetry ready");
#if HW_ENABLE_SERIAL_COMMANDS
    HeapDiagnostics::hwHeapDiagPrintTick("WebSocket");
#endif
  } else {
    HW_DEBUG_PRINTLN("[App] WebSocket telemetry initialization failed");
    SystemState::setWebSocketReady(false);
    g_webSocketTelemetry = nullptr;
    // Note: telemetry pointer remains valid (static instance), just not initialized
  }
#endif
}

void App::initBLE() {
#if HW_ENABLE_BLE
  if (bleReady) {
    return; // Already initialized
  }
  
  // BLE starts in background after PH_RUNNING
  if (currentPhase != BootPhase::PH_RUNNING) {
    return; // Not yet ready
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing BLE (background)...");
  
  bleManager = &s_bleManager;  // Point to static instance
  
#if HOMEWIND_ENABLE_WEBSOCKET
  if (bleManager->begin(telemetry)) {
#else
  if (bleManager->begin()) {
#endif
    g_bleRelayManager = bleManager;
    bleReady = true;
    SystemState::setBLEReady(true);
    HW_DEBUG_PRINTLN("[App] BLERelayManager initialized");
#if HW_ENABLE_DISPLAY
    /* Refresh HR/CSC display state so "configured but not connected" shows "Not Connected" not "Not Configured" */
    if (g_displayManager) {
      g_displayManager->updateHRState();
      g_displayManager->updateCSCState();
    }
#endif
#if HW_ENABLE_SERIAL_COMMANDS
    HeapDiagnostics::hwHeapDiagCaptureBleReady();
    HeapDiagnostics::hwHeapDiagPrintTick("BLE");
#endif
  } else {
    HW_ERROR_PRINTLN("[App] BLERelayManager initialization failed");
    // Note: bleManager pointer remains valid (static instance), just not initialized
    bleReady = false;
  }
#endif
}

void App::initFans() {
#if HW_ENABLE_FANS
  if (fansReady) {
    return; // Already initialized
  }
  
  // Fan controller starts in background after PH_RUNNING
  if (currentPhase != BootPhase::PH_RUNNING) {
    return; // Not yet ready
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing Fan controller (background)...");
  
  fanController = &s_fanController;  // Point to static instance
  
#if HOMEWIND_ENABLE_WEBSOCKET
  if (fanController->begin(telemetry)) {
#else
  if (fanController->begin()) {
#endif
    g_fanController = fanController;
    fansReady = true;
    SystemState::setFansReady(true);
    HW_DEBUG_PRINTLN("[App] Fan controller initialized");
#if HW_ENABLE_SERIAL_COMMANDS
    HeapDiagnostics::hwHeapDiagPrintTick("Fans");
#endif
    
#if HW_ENABLE_BLE
    // Initialize FanHRAdapter after fanController and heartRateSettings are ready
    if (heartRateSettings && heartRateSettingsReady && !g_fanHrAdapter) {
      FanHRAdapter* adapter = &s_fanHrAdapter;  // Point to static instance
      if (adapter->begin(fanController, heartRateSettings)) {
        g_fanHrAdapter = adapter;
        HW_DEBUG_PRINTLN("[App] FanHRAdapter initialized");
      } else {
        HW_ERROR_PRINTLN("[App] FanHRAdapter initialization failed");
        // Note: adapter pointer remains valid (static instance), just not initialized
      }
    }
#endif
  } else {
    HW_ERROR_PRINTLN("[App] Fan controller initialization failed");
    // Note: fanController pointer remains valid (static instance), just not initialized
    fansReady = false;
  }
#endif
}

void App::initHeartRateSettings() {
#if HW_ENABLE_NVS
  if (heartRateSettingsReady) {
    return; // Already initialized
  }
  
  // Heart rate settings require NVS to be ready
  if (!nvsReady) {
    return; // NVS not ready yet
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing Heart Rate Settings...");
  
  heartRateSettings = &s_heartRateSettings;  // Point to static instance
  
  if (heartRateSettings->begin()) {
    g_heartRateSettings = heartRateSettings;
    heartRateSettingsReady = true;
    HW_DEBUG_PRINTLN("[App] HeartRateSettings initialized");
    
#if HW_ENABLE_FANS && HW_ENABLE_BLE
    // Deferred FanHRAdapter initialization if fans already initialized
    if (fanController && fansReady && !g_fanHrAdapter) {
      FanHRAdapter* adapter = &s_fanHrAdapter;  // Point to static instance
      if (adapter->begin(fanController, heartRateSettings)) {
        g_fanHrAdapter = adapter;
        HW_DEBUG_PRINTLN("[App] FanHRAdapter initialized (deferred)");
      } else {
        HW_ERROR_PRINTLN("[App] FanHRAdapter initialization failed (deferred)");
        // Note: adapter pointer remains valid (static instance), just not initialized
      }
    }
#endif
  } else {
    HW_ERROR_PRINTLN("[App] HeartRateSettings initialization failed");
    // Note: heartRateSettings pointer remains valid (static instance), just not initialized
    heartRateSettingsReady = false;
  }
#endif
}

void App::initOTA() {
#if HW_ENABLE_OTA
  if (otaReady) {
    return; // Already initialized
  }
  
  // OTA starts in background after PH_RUNNING
  if (currentPhase != BootPhase::PH_RUNNING) {
    return; // Not yet ready
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing FirmwareUpdateManager...");
  
  firmwareUpdateManager = &s_firmwareUpdateManager;  // Point to static instance
  
  // Check if HW_FIRMWARE_GITHUB_URL is defined
  #ifndef HW_FIRMWARE_GITHUB_URL
    HW_ERROR_PRINTLN("[App] HW_FIRMWARE_GITHUB_URL not defined - FirmwareUpdateManager disabled");
    otaReady = false;
    return;
  #endif
  
  if (firmwareUpdateManager->begin(BuildInfo::getVersion(), HW_FIRMWARE_GITHUB_URL)) {
    g_firmwareUpdateManager = firmwareUpdateManager;
    
    // Set web server if available
    #if HW_ENABLE_WEB
    if (webServer) {
      firmwareUpdateManager->setWebServer(webServer->getServer());
    }
    #endif
    
    otaReady = true;
    HW_DEBUG_PRINTLN("[App] FirmwareUpdateManager initialized");
#if HW_ENABLE_SERIAL_COMMANDS
    HeapDiagnostics::hwHeapDiagPrintTick("FirmwareUpdate");
#endif
  } else {
    HW_ERROR_PRINTLN("[App] FirmwareUpdateManager initialization failed");
    // Note: firmwareUpdateManager pointer remains valid (static instance), just not initialized
    otaReady = false;
  }
#endif
}

void App::loopSubsystems() {
  // WiFi service loop (non-blocking)
#if HW_ENABLE_WIFI

#if HW_WIFI_BACKEND_FIXED
  if (wifi) {
    wifi->loop();
    
    // Update ready state
    bool wasReady = wifiReady;
    wifiReady = wifi->ready();
    
    if (wifiReady != wasReady) {
      SystemState::setWiFiReady(wifiReady);
      SystemState::setStationOnline(wifi->isSTA());
      if (wifiReady) {
        HW_DEBUG_PRINTF("[App] WiFi ready (%s): %s\n", wifi->modeString(), wifi->ipString());
      }
    }
  }
#elif HW_WIFI_BACKEND_WML
  // WML backend loop
  s_wifiWML.loop();

  /* AP↔STA screen switching (Captive Portal) */
  static bool lastAPMode = false;
  bool currentAP = s_wifiWML.isAP() && !s_wifiWML.isSTA();
  if (currentAP != lastAPMode) {
    lastAPMode = currentAP;
#if HW_ENABLE_DISPLAY
    if (g_displayManager) {
      if (currentAP) {
        g_displayManager->showAPScreen();
      } else {
        g_displayManager->showMainScreen();
      }
    }
#endif
  }

  // Track state changes
  static bool lastStaOnline = false;
  bool currentStaOnline = s_wifiWML.isSTA();
  
  if (currentStaOnline != lastStaOnline) {
    lastStaOnline = currentStaOnline;
    SystemState::setStationOnline(currentStaOnline);
    
    if (currentStaOnline) {
      HW_DEBUG_PRINTF("[App] Station online: %s\n", s_wifiWML.ipString());

#if HW_ENABLE_DISPLAY
      if (g_displayManager) {
        char url[64];
        snprintf(url, sizeof(url), "http://%s", s_wifiWML.ipString());
        g_displayManager->setUrl(url);
      }
#endif

      // Start operational services when STA comes online (Level B)
      // These are deferred until station is connected
      // Optimized sequence: FirmwareUpdate → Fans → BLE (after SmartConnect)
      if (!otaReady) initOTA();  // First: Small, needs WebServer
      if (!fansReady) initFans(); // Second: Small, SmartConnect starts async
      
      // Initialize BLE after Fans are ready (SmartConnect runs async, doesn't block)
      // This reduces heap fragmentation: small Fans allocation before large BLE allocation
      if (!bleReady && fansReady) {
        initBLE();
      }
    } else {
      HW_DEBUG_PRINTLN("[App] Station offline (AP mode)");
    }
  }
#endif

#endif // HW_ENABLE_WIFI

#if HW_ENABLE_WEB
  if (webServer) {
    webServer->loop();
  }
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
  if (telemetry) {
    telemetry->loopFlush();
  }
#endif

#if HW_ENABLE_BLE
  if (bleManager && bleReady) {
    bleManager->loop();
  }
#endif

#if HW_ENABLE_FANS
  if (fanController && fansReady) {
    fanController->loop();
  }
#endif

#if HW_ENABLE_BLE
  // Initialize BLE after Fans POST_CTRL_DONE completes (optimized boot sequence)
  // This reduces heap fragmentation: small Fans allocation before large BLE allocation
  // POST_CTRL_DONE occurs after SmartConnect (~400ms) + control sequence (~3s) = ~4s total
  if (!bleReady && fansReady && fanController) {
    uint8_t fanCount = fanController->getFanCount();
    
    // Initialize BLE if:
    // - No fans configured (nothing to wait for), OR
    // - POST_CTRL_DONE reached (control sequence complete, heap settled)
    if (fanCount == 0 || fanController->isPostConnectDone()) {
      initBLE();
    }
  }
  
  // FanHRAdapter initialization (deferred if BLE initialized after Fans)
#if HW_ENABLE_FANS && HW_ENABLE_BLE
  if (!g_fanHrAdapter && fanController && fansReady && heartRateSettings && heartRateSettingsReady && bleReady) {
    FanHRAdapter* adapter = &s_fanHrAdapter;
    if (adapter->begin(fanController, heartRateSettings)) {
      g_fanHrAdapter = adapter;
      HW_DEBUG_PRINTLN("[App] FanHRAdapter initialized (deferred in loop)");
    }
  }
#endif

  // FanHRAdapter loop (HR-driven fan control)
  if (g_fanHrAdapter && g_fanHrAdapter->ready()) {
    g_fanHrAdapter->loop(millis());
  }
#endif

#if HW_ENABLE_OTA
  if (firmwareUpdateManager) {
    firmwareUpdateManager->loop();
  }
#endif

#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->loop();
  }
#endif
}

bool App::isPhaseTimeout() {
  unsigned long now = millis();
  return (now - phaseStartTime) > HW_BOOT_PHASE_TIMEOUT_MS;
}

void App::enterDegradedMode() {
  currentPhase = BootPhase::PH_DEGRADED;
  HW_DEBUG_PRINTLN("[App] Entered DEGRADED mode");
  // Services may still be partially functional
  // Update system state to reflect degraded status
  SystemState::setServicesReady(false);
}
