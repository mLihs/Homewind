/**
 * @file App.h
 * @brief Main application coordinator with deterministic boot orchestrator
 */

#ifndef HOMEWIND_APP_H
#define HOMEWIND_APP_H

#include <Arduino.h>
#include "Config.h"
#include "BuildInfo.h"

// Forward declarations
#if HW_ENABLE_NVS
class SettingsStore;
#endif

#if HW_ENABLE_WIFI && HW_WIFI_BACKEND_FIXED
class WiFiService;
#endif

#if HW_ENABLE_WEB
class WebServerManager;
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

#if HW_ENABLE_BLE
class BLERelayManager;
#endif

#if HW_ENABLE_FANS
class FanController;
#endif

#if HW_ENABLE_OTA
class FirmwareUpdateManager;
#endif

#if HW_ENABLE_NVS
class HeartRateSettings;
#endif

#if HW_ENABLE_DISPLAY
class DisplayManager;
#endif

/**
 * @enum BootPhase
 * @brief Boot phase enumeration (deterministic order)
 */
enum class BootPhase {
  PH_CORE = 0,      // Core initialization (always first)
  PH_NVS,           // NVS/Settings initialization (if enabled)
  PH_WIFI,          // WiFi connection (if enabled, with timeout)
  PH_WEB,           // Web server initialization (if enabled)
  PH_BLE,           // BLE initialization (background, if enabled)
  PH_FANS,          // Fan controller initialization (background, if enabled)
  PH_RUNNING,       // All critical services ready
  PH_DEGRADED       // Degraded mode (some services failed)
};

/**
 * @class App
 * @brief Main application coordinator with deterministic boot state machine
 * 
 * Boot phases execute in fixed order with timeouts:
 * 1. PH_CORE: Minimal core initialization
 * 2. PH_NVS: Settings store (if enabled)
 * 3. PH_WIFI: WiFi connection with timeout
 * 4. PH_WEB: Web server (if WiFi ready or degraded)
 * 5. PH_BLE: BLE services (background, after PH_RUNNING)
 * 6. PH_FANS: Fan controller (background, after PH_RUNNING)
 * 7. PH_RUNNING: All critical services operational
 */
class App {
public:
  App();
  ~App();
  
  /**
   * Initialize boot orchestrator (non-blocking start)
   * Sets initial phase to PH_CORE
   * @return true if boot process started, false on critical failure
   */
  bool begin();
  
  /**
   * Main loop - call from Arduino loop()
   * Advances boot phases non-blocking, then runs subsystems
   */
  void loop();
  
  /**
   * Check if all critical services are ready
   * @return true if ready, false otherwise
   */
  bool isReady() const;
  
  /**
   * Get current boot phase
   * @return Current boot phase
   */
  BootPhase getPhase() const { return currentPhase; }

private:
  bool initialized;
  BootPhase currentPhase;
  unsigned long phaseStartTime;
  unsigned long bootStartTime;
  
  // Subsystem instances (only allocated if enabled)
#if HW_ENABLE_NVS
  SettingsStore* settings;
  bool nvsReady;
#endif

#if HW_ENABLE_WIFI
  #if HW_WIFI_BACKEND_FIXED
  WiFiService* wifi;
  #endif
  bool wifiReady;
#endif

#if HW_ENABLE_WEB
  WebServerManager* webServer;
  bool webReady;
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
  WebSocketTelemetry* telemetry;
  bool wsReady;
#endif

#if HW_ENABLE_BLE
  BLERelayManager* bleManager;
  bool bleReady;
#endif

#if HW_ENABLE_FANS
  FanController* fanController;
  bool fansReady;
#endif

#if HW_ENABLE_OTA
  FirmwareUpdateManager* firmwareUpdateManager;
  bool otaReady;
#endif

#if HW_ENABLE_NVS
  HeartRateSettings* heartRateSettings;
  bool heartRateSettingsReady;
#endif

  /**
   * Boot phase machine - advances phases non-blocking
   */
  void advanceBootPhase();
  
  /**
   * Initialize core subsystems
   */
  void initCore();
  
  /**
   * Initialize NVS/Settings
   */
  void initNVS();
  
  /**
   * Initialize WiFi (non-blocking with timeout)
   * Uses WiFiService for STA connection and AP fallback
   */
  void initWiFi();
  
  /**
   * Initialize Web server
   */
  void initWeb();
  
  /**
   * Initialize BLE (background, after PH_RUNNING)
   */
  void initBLE();
  
  /**
   * Initialize Fan controller (background, after PH_RUNNING)
   */
  void initFans();
  
  /**
   * Initialize OTA (background, after PH_RUNNING)
   */
  void initOTA();
  
  /**
   * Initialize Heart Rate Settings (after NVS ready)
   */
  void initHeartRateSettings();
  
  /**
   * Loop subsystems (call from loop())
   */
  void loopSubsystems();
  
  /**
   * Check if phase timeout expired
   */
  bool isPhaseTimeout();
  
  /**
   * Transition to degraded mode
   */
  void enterDegradedMode();
  
#if HW_ENABLE_SERIAL_COMMANDS
  /**
   * Handle serial debug commands (h=heap, s=status, ?=help)
   */
  void handleSerialCommands();
#endif
};

#endif // HOMEWIND_APP_H

