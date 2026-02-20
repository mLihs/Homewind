/**
 * @file FanController.h
 * @brief Fan Controller - SmartMiFanAsync integration
 * 
 * Manages fan discovery, connections, and control using SmartMiFanAsync library.
 * Integrates with NVS persistence, WebSocket telemetry, and API endpoints.
 * 
 * Uses deterministic struct-based NVS storage (no heap allocation for persistence).
 */

#ifndef HOMEWIND_FAN_CONTROLLER_H
#define HOMEWIND_FAN_CONTROLLER_H

#include <Arduino.h>
#include <strings.h>  // strcasecmp/strncasecmp
#include "../app/Config.h"

#if HW_ENABLE_FANS

#include <WiFiUdp.h>
#include <Preferences.h>

// Forward declarations to avoid SystemState name conflict
// (Homewind uses namespace SystemState, SmartMiFanAsync uses enum class SystemState)
// SmartMiFanAsync.h will be included only in .cpp file
struct SmartMiFanDiscoveredDevice;
struct SmartMiFanFastConnectEntry;
enum class FanParticipationState : int;
enum class DiscoveryState : int;
enum class SmartConnectState : int;

// Forward declarations
#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

/**
 * @struct FanConfig
 * @brief Fan configuration stored in NVS
 * 
 * Fast Connect uses: ip, token, model
 * Note: DID removed - not needed since SmartMiFanAsync v1.8+
 */
struct FanConfig {
  char token[33];    // 32-character hex token + null (fixed buffer, no heap)
  IPAddress ip;      // IP address (0.0.0.0 if not discovered)
  char model[24];    // Model string - enables faster Fast Connect (skips queryInfo)
  bool enabled;      // User intent: true = ACTIVE, false = INACTIVE (persisted in NVS)
  
  FanConfig() : ip(0, 0, 0, 0), enabled(true) {
    token[0] = '\0';
    model[0] = '\0';
  }
  
  void setToken(const char* t) {
    if (t) {
      strncpy(token, t, sizeof(token) - 1);
      token[sizeof(token) - 1] = '\0';
    } else {
      token[0] = '\0';
    }
  }
  
  void setToken(const String& t) {
    setToken(t.c_str());
  }
  
  bool tokenEquals(const char* other) const {
    if (!other) return token[0] == '\0';
    return strcasecmp(token, other) == 0;
  }
  
  bool tokenEquals(const String& other) const {
    return tokenEquals(other.c_str());
  }
};

/**
 * @class FanController
 * @brief Fan Controller - manages SmartMiFanAsync integration
 * 
 * Features:
 * - NVS persistence for fan configurations
 * - Async Discovery for new/updated fans
 * - Smart Connect on startup
 * - WebSocket telemetry integration
 * - API endpoint handlers
 */
// Forward declaration
class FanHRAdapter;

class FanController {
  friend class FanHRAdapter;  // Allow FanHRAdapter to access private fan control methods
  
public:
  FanController();
  ~FanController();
  
  /**
   * Initialize fan controller
   * @param telemetry Optional WebSocket telemetry for state updates
   * @return true on success
   */
#if HOMEWIND_ENABLE_WEBSOCKET
  bool begin(WebSocketTelemetry* telemetry = nullptr);
#else
  bool begin();
#endif
  
  /**
   * Main loop (call from App::loop())
   * Updates discovery and smart connect state machines
   */
  void loop();
  
  /**
   * Check if fan controller is ready
   * @return true if ready
   */
  bool ready() const { return initialized; }
  
  /**
   * Stop fan operations for maintenance mode
   * 
   * Stops all periodic operations, discovery, and network traffic.
   * Idempotent - safe to call multiple times.
   * Returns quickly (< 200ms target, < 500ms max).
   */
  void stopForMaintenance();
  
  /**
   * Check if stopped for maintenance
   * @return true if stopped for maintenance
   */
  bool isStoppedForMaintenance() const { return _maintenanceStopped; }
  
  // Fan Management API
  
  /**
   * Add a fan by token (starts discovery)
   * @param token 32-character hex token
   * @return true on success
   */
  bool addFan(const String& token);
  
  /**
   * Remove a fan (sets speed to 0, turns off, then deletes)
   * @param token Fan token to remove
   * @return true on success
   */
  bool removeFan(const String& token);
  
  /**
   * Fast removal - removes fan from memory and updates UI immediately
   * Use this when you want instant UI feedback, then call removeFanCleanup()
   * @param token Fan token to remove
   * @return true on success
   */
  bool removeFanFast(const String& token);
  
  /**
   * Cleanup after fast removal - controls fan and saves to NVS
   * Call this after sending API response for better UX
   * @param token Fan token that was removed
   */
  void removeFanCleanup(const String& token);
  
  /**
   * Update fan token (starts discovery for new token)
   * @param oldToken Old token
   * @param newToken New token
   * @return true on success
   */
  bool updateFanToken(const String& oldToken, const String& newToken);
  
  /**
   * Set fan participation state (ACTIVE/INACTIVE)
   * @param token Fan token
   * @param active true for ACTIVE, false for INACTIVE
   * @return true on success
   */
  bool setFanControlState(const String& token, bool active);
  
  /**
   * Get fan count
   * @return Number of configured fans
   */
  uint8_t getFanCount() const { return _fanCount; }
  
  /**
   * Get connected fan count (fans with ready=true after handshake)
   * @return Number of connected fans
   */
  uint8_t getConnectedFanCount() const;
  
  /**
   * Get fan configuration by index
   * @param index Fan index (0-based)
   * @param config Output: fan configuration
   * @return true if fan found
   */
  bool getFanConfig(uint8_t index, FanConfig& config) const;
  
  /**
   * Get fan configuration by token
   * @param token Fan token
   * @param config Output: fan configuration
   * @return true if fan found
   */
  bool getFanConfigByToken(const String& token, FanConfig& config) const;
  
  /**
   * Get discovered fans from SmartMiFanAsync
   * Used by WebSocket telemetry to build FANS_SNAPSHOT
   * @param fans Output array (must be at least MAX_FANS)
   * @param count Output: number of fans
   * @return true on success
   */
  bool getDiscoveredFans(void* fans, size_t& count) const;
  
  /**
   * Mark WebSocket telemetry dirty (call when fan state changes)
   */
  void markTelemetryDirty();

  /**
   * Fan recovery UI state for WebUI.
   * 0 = normal, 1 = recovering (auto), 2 = exhausted (needs user action)
   */
  uint8_t getFanRecoveryUiState(uint8_t index) const;
  
  /**
   * Get post-connection test speed (percent)
   * This speed is used during the brief on/off test after successful connection
   * @return Speed in percent (1-100)
   */
  uint8_t getPostConnectSpeed() const { return _postConnectSpeed; }

  /**
   * Effective participation for HR control (soft-error phase keeps fan ACTIVE)
   */
  bool isFanActiveForHr(size_t discoveredIndex, const SmartMiFanDiscoveredDevice& fan) const;
  
  /**
   * Set post-connection test speed (percent)
   * @param speed Speed in percent (1-100)
   * @param persist If true, save to NVS
   * @return true on success
   */
  bool setPostConnectSpeed(uint8_t speed, bool persist = true);
  
  /**
   * Check if POST-CONNECTION CONTROL SEQUENCE is complete
   * Used to determine when it's safe to initialize BLE (after heap settles)
   * @return true if POST_CTRL_DONE reached (sequence complete)
   */
  bool isPostConnectDone() const;

private:
  static const uint8_t MAX_FANS = 4;

  // Auto-recovery: attempt to recover ERROR fans in background.
  // Goal: try a few times automatically, then require explicit user action (UI toggle) to retry.
  // Tunables are defined in Config.h:
  // - HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS
  // - HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS
  // - HW_FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS
  static constexpr unsigned long FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS = HW_FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS;
  
  // NVS namespace and keys
  static constexpr const char* NVS_NAMESPACE = "homewind";
  static constexpr const char* NVS_KEY_DATA = "fans_data";     // New blob format
  
  /**
   * @brief NVS data structure for fan storage (deterministic, no heap)
   * 
   * Uses packed struct with CRC16 for data integrity validation.
   * Note: DID removed in v2 - not needed since SmartMiFanAsync v1.8+
   */
  #pragma pack(push, 1)
  struct NVSFanEntry {
    char token[33];     // 32 hex chars + null
    uint8_t ip[4];      // IPv4 address
    char model[24];     // Model string
    uint8_t enabled;    // 0 = disabled, 1 = enabled
    uint8_t reserved;   // Alignment padding
  };
  
  struct NVSFanData {
    uint8_t version;           // Schema version (current: 2)
    uint8_t fanCount;          // Number of configured fans
    uint8_t postConnectSpeed;  // Post-connection test speed (1-100)
    uint8_t reserved;          // Alignment padding
    NVSFanEntry fans[MAX_FANS];// Fan configurations
    uint16_t crc16;            // CRC16-CCITT checksum
  };
  #pragma pack(pop)
  
  static_assert(sizeof(NVSFanEntry) == 63, "NVSFanEntry size mismatch");
  static_assert(sizeof(NVSFanData) == 258, "NVSFanData size mismatch");
  
  static constexpr uint8_t NVS_DATA_VERSION = 2;  // v2: DID removed
  
  bool initialized;
  bool _maintenanceStopped;
  uint8_t _postConnectSpeed;  // Speed for post-connection test (1-100%)
  
  WiFiUDP _fanUdp;  // UDP socket for fan communication
  
#if HOMEWIND_ENABLE_WEBSOCKET
  WebSocketTelemetry* _telemetry;
#endif
  
  // Fan configurations (loaded from NVS)
  FanConfig _fans[MAX_FANS];
  uint8_t _fanCount;
  
  // Discovery state
  bool _discoveryInProgress;
  String _discoveryToken;  // Token being discovered
  unsigned long _discoveryStartTime;
  char _discoveryTokenStorage[33];  // Persistent storage for token string (32 chars + null)
  const char* _discoveryTokenPtr;    // Pointer to token storage
  const char* _discoveryTokensArray[1];  // Persistent array of token pointers for library

  // Discovery context (to interpret results)
  bool _discoveryIsRecovery;
  int _discoveryRecoveryNvsIndex;  // Which configured fan index (in _fans[]) initiated recovery discovery
  
  // Smart Connect state
  bool _smartConnectInProgress;
  unsigned long _smartConnectStartTime;
  
  // Post-connection control state machine (non-blocking)
  enum class PostConnectStep : uint8_t {
    IDLE,           // Not running
    POWER_ON,       // Turn on fans
    WAIT_STABILIZE, // Wait 500ms for fans to stabilize
    SET_SPEED,      // Set configured speed
    WAIT_RUN,       // Wait 3000ms for fans to run
    POWER_OFF,      // Turn off fans
    DONE            // Sequence complete
  };
  PostConnectStep _postConnectStep;
  unsigned long _postConnectStepStartTime;
  
  // Throttling for fan control state changes (prevents rapid toggle issues)
  static constexpr unsigned long FAN_CONTROL_THROTTLE_MS = 500;  // Min 500ms between state changes per fan
  unsigned long _lastControlStateChangeMs[MAX_FANS];  // Last change time per fan

  // Recovery throttling: avoid spamming discovery retries on flaky networks
  static constexpr unsigned long FAN_RECOVERY_DISCOVERY_THROTTLE_MS = 5000;  // Min 5s between discovery retries per fan
  unsigned long _lastRecoveryDiscoveryAttemptMs[MAX_FANS];  // Last recovery discovery attempt per fan

  // Auto-recovery state per configured fan (index in _fans[])
  uint8_t _autoRecoveryAttempts[MAX_FANS];
  bool _autoRecoveryExhausted[MAX_FANS];
  bool _autoRecoveryActive[MAX_FANS];
  unsigned long _lastAutoRecoveryAttemptMs[MAX_FANS];
  uint8_t _unhealthyStreak[MAX_FANS];  // consecutive unhealthy observations
  bool _softActive[MAX_FANS];          // soft-error override (keep ACTIVE despite lastError)
  
  // NVS persistence (deterministic struct-based storage)
  bool loadFansFromNVS();
  bool saveFansToNVS();
  int findFanIndex(const String& token) const;
  
  
  // Discovery management
  void startDiscoveryForToken(const String& token);
  void updateDiscovery();
  void onDiscoveryComplete();
  
  // Smart Connect management
  void startSmartConnect();
  void updateSmartConnect();
  void onSmartConnectComplete();
  
  // State synchronization: NVS enabled states → SmartMiFanAsync
  void syncEnabledStatesToSmartMiFanAsync();
  
  // Fan control
  bool setFanSpeed(const String& token, uint8_t speed);
  bool setFanPower(const String& token, bool on);
  void updateFanFromDiscovered(const SmartMiFanDiscoveredDevice& discovered);
  
  // Post-connection control sequence (non-blocking state machine)
  // Replaces blocking performPostConnectionControl() with millis-based state machine
  void startPostConnectionControl();    // Start the sequence
  void updatePostConnectionControl();   // Call from loop() to advance state machine
  
  // Helper: Convert token to fan index in SmartMiFanAsync
  int findDiscoveredFanIndex(const String& token) const;

  // Auto-recovery loop (detect ERROR transitions and attempt recovery)
  void updateAutoRecovery();
};

#endif // HW_ENABLE_FANS

#endif // HOMEWIND_FAN_CONTROLLER_H
