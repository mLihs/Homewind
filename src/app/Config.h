/**
 * @file Config.h
 * @brief Compile-time configuration and feature gates
 * 
 * This file contains all compile-time switches for optional subsystems.
 * All features default to OFF to ensure minimal, deterministic builds.
 */

#ifndef HOMEWIND_CONFIG_H
#define HOMEWIND_CONFIG_H

// ============================================================
// Development Mode (compile-time only)
// ============================================================
// HW_DEV_MODE controls WiFi backend selection:
//   1 = Development: Fixed WiFi credentials (WiFiService)
//   0 = Production: WiFi via Captive Portal (wifiMangerLite + NVS)
//
// In DEV mode: Fast boot with compile-time SSID/PASS
// In PROD mode: AP/Portal for initial setup, NVS for credentials
#ifndef HW_DEV_MODE
#define HW_DEV_MODE 0
#endif

// WiFi Backend Selection (derived from HW_DEV_MODE)
#if HW_DEV_MODE == 1
  #define HW_WIFI_BACKEND_FIXED 1
  #define HW_WIFI_BACKEND_WML   0
#else
  #define HW_WIFI_BACKEND_FIXED 0
  #define HW_WIFI_BACKEND_WML   1
#endif

// ============================================================
// Feature Gates (compile-time only, no runtime overhead)
// Boot order is FIXED - these flags only enable/disable modules
// ============================================================

// NVS / Settings Store (required for settings persistence)
// Note: Always enabled in PROD mode (WML needs NVS for credentials)
#ifndef HW_ENABLE_NVS
  #if HW_WIFI_BACKEND_WML == 1
    #define HW_ENABLE_NVS 1  // Required for wifiMangerLite
  #else
    #define HW_ENABLE_NVS 1
  #endif
#endif

// WiFi support (required for web services)
#ifndef HW_ENABLE_WIFI
#define HW_ENABLE_WIFI 1
#endif

// Web Server support (requires ESPAsyncWebServer library)
// Depends on: HW_ENABLE_WIFI
#ifndef HW_ENABLE_WEB
#define HW_ENABLE_WEB 1
#endif

// BLE Relay + Sensors (background service, starts after PH_RUNNING)
#ifndef HW_ENABLE_BLE
#define HW_ENABLE_BLE 1
#endif

// Experimental: Two-phase BLE init
// If enabled, Homewind will pre-initialize ONLY the NimBLE stack at PH_RUNNING entry,
// and later start the full BLE server (BluetoothBikeSensorServerBegin) after Fans.
// This is intended to reduce heap fragmentation by placing NimBLE allocations earlier.
#ifndef HW_BLE_PREINIT_NIMBLE_STACK
#define HW_BLE_PREINIT_NIMBLE_STACK 1
#endif

// Fan Controller + Fans (background service, starts after PH_RUNNING)
#ifndef HW_ENABLE_FANS
#define HW_ENABLE_FANS 1
#endif

// OTA Update support (optional, starts after PH_RUNNING)
#ifndef HW_ENABLE_OTA
#define HW_ENABLE_OTA 1
#endif

// Display support (requires HomeWindWSAmoled library + compatible hardware)
// Set to 1 to enable AMOLED display (HR, CSC, Fans, QR), 0 to disable (headless)
#ifndef HW_ENABLE_DISPLAY
#define HW_ENABLE_DISPLAY 1  // Default: ON (Touch board); use 0 for headless/Basic
#endif

// Firmware update GitHub URL (raw JSON file)
// Example: "https://raw.githubusercontent.com/username/repo/main/latest.json"
#ifndef HW_FIRMWARE_GITHUB_URL
//#define HW_FIRMWARE_GITHUB_URL "https://raw.githubusercontent.com/mLihs/PulseFanSync/main/firmware/latest.json"
#if HW_ENABLE_DISPLAY == 1
#define HW_FIRMWARE_GITHUB_URL "http://homewind.io/src/firmware/homewind-touch/latest.json"
#else
#define HW_FIRMWARE_GITHUB_URL "http://homewind.io/src/firmware/homewind-basic/latest.json"
#endif
#endif

// Legacy feature gate aliases (for backward compatibility)
#ifndef HOMEWIND_ENABLE_SETTINGS
#define HOMEWIND_ENABLE_SETTINGS HW_ENABLE_NVS
#endif

#ifndef HOMEWIND_ENABLE_WEBSERVER
#if HW_ENABLE_WEB == 1
#define HOMEWIND_ENABLE_WEBSERVER 1
#else
#define HOMEWIND_ENABLE_WEBSERVER 0
#endif
#endif

// WebSocket Telemetry support (uses AsyncWebSocket from ESPAsyncWebServer)
// Depends on: HW_ENABLE_WEB
#ifndef HOMEWIND_ENABLE_WEBSOCKET
#define HOMEWIND_ENABLE_WEBSOCKET 1
#endif

// WebSocket client limit (applied on server-side)
#ifndef HW_WEBSOCKET_MAX_CLIENTS
#define HW_WEBSOCKET_MAX_CLIENTS 2
#endif

// API Actions support (depends on HW_ENABLE_WEB)
#ifndef HOMEWIND_ENABLE_API_ACTIONS
#define HOMEWIND_ENABLE_API_ACTIONS HW_ENABLE_WEB
#endif

// API Settings support (depends on HW_ENABLE_WEB && HW_ENABLE_NVS)
#ifndef HOMEWIND_ENABLE_API_SETTINGS
#define HOMEWIND_ENABLE_API_SETTINGS (HW_ENABLE_WEB && HW_ENABLE_NVS)
#endif

// ============================================================
// Default Values & Timeouts
// ============================================================

// Web server port (only used if HW_ENABLE_WEB == 1)
#define HOMEWIND_WEBSERVER_PORT 80

// WebSocket port (only used if HOMEWIND_ENABLE_WEBSOCKET == 1)
#define HOMEWIND_WEBSOCKET_PORT 81

// mDNS hostname (only used if HW_ENABLE_WIFI == 1)
#define HOMEWIND_MDNS_HOSTNAME "esp32-setup"

// WiFi connection timeout (milliseconds)
// If timeout expires, falls back to degraded mode or AP mode
#ifndef HW_WIFI_TIMEOUT_MS
#define HW_WIFI_TIMEOUT_MS 20000
#endif

// WiFi AP fallback mode (enables AP mode if STA connection fails)
#ifndef HW_WIFI_AP_FALLBACK
#define HW_WIFI_AP_FALLBACK 1
#endif

// WiFi compile-time credentials (DEV mode only - HW_WIFI_BACKEND_FIXED)
// In PROD mode (HW_WIFI_BACKEND_WML), credentials come from NVS via Captive Portal
#if HW_WIFI_BACKEND_FIXED == 1
  #ifndef HW_WIFI_SSID
  #define HW_WIFI_SSID "YOUR_WIFI_SSID"
  #endif

  #ifndef HW_WIFI_PASS
  #define HW_WIFI_PASS "YOUR_WIFI_PASSWORD"
  #endif

  // WiFi AP mode credentials (fallback mode - DEV only)
  #ifndef HW_WIFI_AP_SSID
  #define HW_WIFI_AP_SSID "Homewind"
  #endif

  #ifndef HW_WIFI_AP_PASS
  #define HW_WIFI_AP_PASS "homewind123"
  #endif
#endif

// WiFi hostname (used by both backends)
#ifndef HW_WIFI_HOSTNAME
#define HW_WIFI_HOSTNAME "homewind"
#endif

// WiFi identity base name (for wifiMangerLite AP SSID + mDNS)
// AP SSID will be: "Homewind-XXXX" (with MAC suffix)
#ifndef HW_WIFI_IDENTITY_BASE
#define HW_WIFI_IDENTITY_BASE "Homewind"
#endif

// Boot phase timeout (milliseconds per phase)
// Maximum time to wait for a phase before moving to next/degraded
#define HW_BOOT_PHASE_TIMEOUT_MS 5000

// Fan discovery timeout (milliseconds)
// Timeout for Async Discovery process when adding/updating fans
#ifndef HW_FAN_DISCOVERY_TIMEOUT_MS
#define HW_FAN_DISCOVERY_TIMEOUT_MS 5000  // Changed from 3000 to 5000
#endif

// Fan smart connect timeout (milliseconds)
// Timeout for Smart Connect process on startup
#ifndef HW_FAN_SMART_CONNECT_TIMEOUT_MS
#define HW_FAN_SMART_CONNECT_TIMEOUT_MS 3000
#endif

// Fan post-connection test speed (percent, 1-100)
// Speed used during the brief on/off test after successful connection
// Default: 25% - low enough to be quiet but high enough to verify fan works
#ifndef HW_FAN_POST_CONNECT_SPEED
#define HW_FAN_POST_CONNECT_SPEED 25
#endif

// Fan maximum speed limit (percent, 1-100)
// This is a global cap applied by Homewind when computing/clamping fan speeds.
// Set < 100 to reduce noise, power draw, or avoid instability at full speed.
//
// NOTE: SmartMiFan expects speed in percent. Keep this within 1..100.
#ifndef HW_FAN_MAX_SPEED_PERCENT
#define HW_FAN_MAX_SPEED_PERCENT 95
#endif

// Fan auto-recovery (compile-time tuning)
// - unhealthy checks: how many consecutive "unhealthy" observations are required before auto-recovery triggers
// - max attempts: how many recovery discovery cycles to try automatically before requiring explicit user action
#ifndef HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS
#define HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS 5
#endif

#ifndef HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS
#define HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS 3
#endif

#ifndef HW_FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS
#define HW_FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS 15000
#endif

// Debug logging (production builds should disable this)
#ifndef HW_ENABLE_DEBUG_LOGS
#define HW_ENABLE_DEBUG_LOGS 0
#endif

// Module-specific debug logging (only effective if HW_ENABLE_DEBUG_LOGS=1)
// Set to 0 to disable verbose logs for specific modules
#ifndef HW_DEBUG_FANHR
#define HW_DEBUG_FANHR 0  // FanHRAdapter: HR samples, speed calculations (very verbose)
#endif

// Serial debug commands (type 'h' for heap info, '?' for help)
// Disable in production to save code size
#ifndef HW_ENABLE_SERIAL_COMMANDS
#define HW_ENABLE_SERIAL_COMMANDS 1
#endif

// Heap Diagnostics Runtime Tick interval (milliseconds)
// RUNTIME_TICK logs every N milliseconds with operational status
#ifndef HW_HEAP_DIAG_TICK_INTERVAL_MS
#define HW_HEAP_DIAG_TICK_INTERVAL_MS 30000UL
#endif

// WebSocket Telemetry diagnostics (debug-only, compile-time)
// - Minimal-invasive debug to observe heap + telemetry behavior over time
// - No String usage, no dynamic allocations
// - Recommended to keep OFF in production builds
#ifndef HW_ENABLE_TELEMETRY_DIAG
#define HW_ENABLE_TELEMETRY_DIAG 0
#endif

// Summary interval for telemetry diagnostics (ms)
#ifndef HW_TELEMETRY_DIAG_SUMMARY_MS
#define HW_TELEMETRY_DIAG_SUMMARY_MS 60000UL
#endif

// Print a short frame-name list in the 60s summary (0/1)
#ifndef HW_TELEMETRY_DIAG_PRINT_FRAME_LIST
#define HW_TELEMETRY_DIAG_PRINT_FRAME_LIST 0
#endif

// ============================================================
// Build Configuration Checks
// ============================================================

// Ensure we're on ESP32
#ifndef ESP32
#error "Homewind requires ESP32 platform"
#endif

#endif // HOMEWIND_CONFIG_H

