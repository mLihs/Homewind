/**
 * @file BLERelayManager.h
 * @brief BLE Relay Manager - integrates BluetoothBikeSensorServer with Homewind
 * 
 * Manages BLE sensor connections (HR, CSC), discovery, persistence,
 * and WebSocket telemetry updates.
 * 
 * Uses deterministic struct-based NVS storage (no heap allocation for persistence).
 */

#ifndef HOMEWIND_BLE_RELAY_MANAGER_H
#define HOMEWIND_BLE_RELAY_MANAGER_H

#include <Arduino.h>
#include <cstring>
#include "../app/Config.h"

#if HW_ENABLE_BLE

#include <BluetoothBikeSensorServer.h>
#include <Preferences.h>

// Forward declarations
#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

/**
 * @brief Name buffer size for BLE sensor names (deterministic, no heap)
 */
static const uint8_t BLE_SENSOR_NAME_MAX = 64;

/**
 * @struct SensorInfo
 * @brief Sensor information structure (fixed buffers, no heap)
 */
struct SensorInfo {
  char name[BLE_SENSOR_NAME_MAX];  // 64 bytes
  char type[4];                     // "HR", "CSC" + null
  char mac[18];                     // "AA:BB:CC:DD:EE:FF" + null
  bool connected;
  int battery;                      // 0-100, or -1 if unknown
  
  SensorInfo() : connected(false), battery(-1) {
    name[0] = '\0';
    type[0] = '\0';
    mac[0] = '\0';
  }
  
  void setName(const char* n) {
    if (n) {
      strncpy(name, n, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
    } else {
      name[0] = '\0';
    }
  }
  
  void setType(const char* t) {
    if (t) {
      strncpy(type, t, sizeof(type) - 1);
      type[sizeof(type) - 1] = '\0';
    } else {
      type[0] = '\0';
    }
  }
  
  void setMac(const char* m) {
    if (m) {
      strncpy(mac, m, sizeof(mac) - 1);
      mac[sizeof(mac) - 1] = '\0';
    } else {
      mac[0] = '\0';
    }
  }
};

/**
 * @struct DesiredSensorTarget
 * @brief Desired state for a sensor type (single-writer reconcile pattern)
 * 
 * Uses fixed-size buffers to avoid heap fragmentation on ESP32.
 */
struct DesiredSensorTarget {
  bool enabled;                         // true = sensor should be active
  SensorType type;                      // Sensor type (HR, CSC)
  char name[BLE_SENSOR_NAME_MAX];       // Target sensor name (fixed buffer)
  char mac[18];                          // Target sensor MAC address (fixed buffer)
  int8_t addrType;                      // BLE address type (0=Public, 1=Random, -1=Unknown)
  bool forceApplyNow;                   // true = controlled reconnect if connected
  bool dirty;                           // true = needs reconcile
  
  DesiredSensorTarget() : enabled(false), type(static_cast<SensorType>(0)), 
                          addrType(-1), forceApplyNow(false), dirty(false) {
    name[0] = '\0';
    mac[0] = '\0';
  }
  
  void setName(const char* n) {
    if (n) {
      strncpy(name, n, BLE_SENSOR_NAME_MAX - 1);
      name[BLE_SENSOR_NAME_MAX - 1] = '\0';
    } else {
      name[0] = '\0';
    }
  }
  
  void setMac(const char* m) {
    if (m && m[0] != '\0') {
      strncpy(mac, m, 17);
      mac[17] = '\0';
    } else {
      mac[0] = '\0';
    }
  }
  
  bool nameEquals(const char* other) const {
    if (!other) return name[0] == '\0';
    return strcmp(name, other) == 0;
  }
  
  bool macEquals(const char* other) const {
    if (!other) return mac[0] == '\0';
    return strcmp(mac, other) == 0;
  }
};

/**
 * @struct AppliedSensorState
 * @brief Applied (current) state for a sensor type
 */
struct AppliedSensorState {
  bool added;                           // true = sensor is added to library
  SensorType type;                      // Sensor type
  char name[BLE_SENSOR_NAME_MAX];       // Applied sensor name
  char mac[18];                          // Applied sensor MAC address
  int8_t addrType;                      // Applied BLE address type (0=Public, 1=Random, -1=Unknown)
  
  AppliedSensorState() : added(false), type(static_cast<SensorType>(0)), addrType(-1) {
    name[0] = '\0';
    mac[0] = '\0';
  }
  
  void setName(const char* n) {
    if (n) {
      strncpy(name, n, BLE_SENSOR_NAME_MAX - 1);
      name[BLE_SENSOR_NAME_MAX - 1] = '\0';
    } else {
      name[0] = '\0';
    }
  }
  
  void setMac(const char* m) {
    if (m && m[0] != '\0') {
      strncpy(mac, m, 17);
      mac[17] = '\0';
    } else {
      mac[0] = '\0';
    }
  }
};

/**
 * @struct DiscoveredSensor
 * @brief Discovered sensor information (fixed buffers, no heap)
 */
struct DiscoveredSensor {
  char name[BLE_SENSOR_NAME_MAX];  // 64 bytes
  char type[4];                     // "HR", "CSC" + null
  char mac[18];                     // "AA:BB:CC:DD:EE:FF" + null
  int8_t addrType;                  // BLE address type (0=Public, 1=Random, -1=Unknown)
  
  DiscoveredSensor() : addrType(-1) {
    name[0] = '\0';
    type[0] = '\0';
    mac[0] = '\0';
  }
  
  void setName(const char* n) {
    if (n) {
      strncpy(name, n, sizeof(name) - 1);
      name[sizeof(name) - 1] = '\0';
    } else {
      name[0] = '\0';
    }
  }
  
  void setType(const char* t) {
    if (t) {
      strncpy(type, t, sizeof(type) - 1);
      type[sizeof(type) - 1] = '\0';
    } else {
      type[0] = '\0';
    }
  }
  
  void setMac(const char* m) {
    if (m) {
      strncpy(mac, m, sizeof(mac) - 1);
      mac[sizeof(mac) - 1] = '\0';
    } else {
      mac[0] = '\0';
    }
  }
};

/**
 * @class BLERelayManager
 * @brief BLE Relay Manager - integrates BluetoothBikeSensorServer with Homewind
 * 
 * Manages:
 * - BLE sensor connections (HR, CSC)
 * - Sensor discovery
 * - Sensor configuration persistence (via NVSConfigBus)
 * - WebSocket telemetry updates
 * - BLE relay server (advertising to centrals)
 */
class BLERelayManager {
public:
  BLERelayManager();
  ~BLERelayManager();
  
  /**
   * Initialize BLE relay manager
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
   */
  void loop();
  
  /**
   * Check if BLE is ready
   */
  bool ready() const { return initialized; }
  
  // Sensor Management API
  /**
   * Load configured sensors from NVS and connect them
   */
  bool loadConfiguredSensors();
  
  /**
   * Get sensor connection state
   * @param sensorName Sensor name/ID
   * @param type Output: sensor type (HR, CSC)
   * @param mac Output: MAC address
   * @param connected Output: connection state
   * @param battery Output: battery level (0-100, or -1 if unavailable)
   * @return true if sensor found
   */
  bool getSensorConnectionState(const char* sensorName, String& type, 
                                String& mac, bool& connected, int& battery);
  
  /**
   * Get all configured sensors
   * @param sensors Output array of sensor info
   * @param maxSensors Maximum array size
   * @return Number of sensors returned
   */
  uint8_t getConfiguredSensors(SensorInfo* sensors, uint8_t maxSensors);
  
  /**
   * Add/configure a sensor
   * @param name Sensor name
   * @param type Sensor type ("HR", "CSC")
   * @param mac MAC address (optional, for faster reconnection)
   * @param forceApplyNow If true, force immediate reconnect (controlled disconnect if connected)
   *                      If false, apply-on-disconnect policy (no forced disconnects)
   * @return true on success
   */
  bool setConfiguredSensor(const char* name, const char* type, const char* mac = nullptr, int8_t addrType = -1, bool forceApplyNow = false);
  
  /**
   * Remove a configured sensor
   * @param name Sensor name
   * @return true on success
   */
  bool deleteConfiguredSensor(const char* name);
  
  /**
   * Connect to a sensor by name
   * @param name Sensor name
   * @return true on success
   */
  bool connectSensor(const char* name);
  
  /**
   * Disconnect a sensor
   * @param name Sensor name
   * @return true on success
   */
  bool disconnectSensor(const char* name);
  
  /**
   * Reload sensors from NVS and reconnect
   */
  bool reloadSensors();
  
  // Discovery API
  /**
   * Start sensor discovery
   * @param type Sensor type ("HR", "CSC") or empty for all
   * @return true on success
   */
  bool startDiscovery(const char* type = nullptr);
  
  /**
   * Stop sensor discovery
   */
  bool stopDiscovery();
  
  /**
   * Check if discovery is active
   */
  bool isDiscoveryActive() const { return _discoveryActive; }
  
  /**
   * Get discovery results
   * @param results Output array
   * @param maxResults Maximum array size
   * @return Number of results
   */
  uint8_t getDiscoveryResults(DiscoveredSensor* results, uint8_t maxResults);
  
  // Server Control
  /**
   * Pause BLE relay server (stop advertising)
   */
  bool pauseServer();
  
  /**
   * Resume BLE relay server (start advertising)
   */
  bool resumeServer();
  
  /**
   * Check if server is paused
   */
  bool isServerPaused() const { return _serverPaused; }
  
  /**
   * Get number of configured sensors
   */
  uint8_t getConfiguredSensorCount() const { return _sensorCount; }
  
  /**
   * Get number of connected sensors
   */
  uint8_t getConnectedSensorCount() const { return _connectedCount; }
  
  /**
   * Get number of discovery results (from last discovery)
   */
  uint8_t getLastDiscoveryCount() const { return _lastDiscoveryCount; }
  
  // Maintenance Mode
  /**
   * Stop BLE operations for maintenance mode
   * 
   * Disconnects all connected sensors/clients, stops advertising,
   * and stops notifications/queues/workers.
   * Idempotent - safe to call multiple times.
   */
  void stopForMaintenance();
  
  /**
   * Check if stopped for maintenance
   */
  bool isStoppedForMaintenance() const { return _maintenanceStopped; }

private:
  bool initialized;
  bool _maintenanceStopped;
  bool _serverPaused;
  uint8_t _connectedCount;      // Number of currently connected sensors
  uint8_t _lastDiscoveryCount;  // Results from last discovery
  
  // NVS namespace and keys
  static constexpr const char* NVS_NAMESPACE = "homewind";
  static constexpr const char* NVS_KEY_DATA = "ble_data";      // New blob format
  
  /**
   * @brief NVS data structure for sensor storage (deterministic, no heap)
   */
  #pragma pack(push, 1)
  struct NVSSensorEntry {
    char name[BLE_SENSOR_NAME_MAX];  // Sensor name
    char type[4];                     // "HR", "CSC" + null
    char mac[18];                     // MAC address string + null
    int8_t addrType;                  // BLE address type (0=Public, 1=Random, -1=Unknown)
    uint8_t reserved;                  // Padding
  };
  
  struct NVSSensorData {
    uint8_t version;                  // Schema version (current: 2)
    uint8_t sensorCount;              // Number of configured sensors
    uint8_t reserved[2];              // Padding
    NVSSensorEntry sensors[2];        // HR, CSC (MAX_SENSORS)
    uint16_t crc16;                   // CRC16-CCITT checksum
  };
  #pragma pack(pop)
  
  static_assert(sizeof(NVSSensorEntry) == 88, "NVSSensorEntry size mismatch");
  static_assert(sizeof(NVSSensorData) == 182, "NVSSensorData size mismatch");
  
  static constexpr uint8_t NVS_DATA_VERSION = 2;

#if HOMEWIND_ENABLE_WEBSOCKET
  WebSocketTelemetry* _telemetry;
#endif
  
  // Sensor configuration cache (fixed buffers, no heap fragmentation)
  struct SensorConfig {
    char name[BLE_SENSOR_NAME_MAX];  // 64 bytes
    char type[4];                     // "HR", "CSC" + null
    char mac[18];                     // "AA:BB:CC:DD:EE:FF" + null
    int8_t addrType;                  // BLE address type (0=Public, 1=Random, -1=Unknown)
    SensorType sensorType;            // Converted from string
    
    SensorConfig() : addrType(-1), sensorType(static_cast<SensorType>(0)) {
      name[0] = '\0';
      type[0] = '\0';
      mac[0] = '\0';
    }
    
    void setName(const char* n) {
      if (n) { strncpy(name, n, sizeof(name) - 1); name[sizeof(name) - 1] = '\0'; }
      else { name[0] = '\0'; }
    }
    void setType(const char* t) {
      if (t) { strncpy(type, t, sizeof(type) - 1); type[sizeof(type) - 1] = '\0'; }
      else { type[0] = '\0'; }
    }
    void setMac(const char* m) {
      if (m) { strncpy(mac, m, sizeof(mac) - 1); mac[sizeof(mac) - 1] = '\0'; }
      else { mac[0] = '\0'; }
    }
  };
  
  static const uint8_t MAX_SENSORS = 2;  // HR, CSC
  SensorConfig _configuredSensors[MAX_SENSORS];
  uint8_t _sensorCount;
  
  bool saveConfigToNVS();
  bool loadConfigFromNVS();
  
  // Discovery state
  bool _discoveryActive;
  char _discoveryType[4];  // "HR", "CSC" or empty
  
  // ============================================================
  // Single-Writer Reconcile Pattern State
  // ============================================================
  // Desired state: what we WANT the BLE library to be configured as
  // Applied state: what we HAVE actually configured in the BLE library
  // Only reconcileSensorTargets() is allowed to call Add/Remove on the library
  
  DesiredSensorTarget _desiredTargets[MAX_SENSORS];   // One per sensor type
  AppliedSensorState _appliedState[MAX_SENSORS];      // Tracks what's actually applied
  bool _reconcileScheduled;                           // Flag to trigger reconcile
  unsigned long _lastReconcileMs;                     // Throttle reconcile calls
  static const unsigned long RECONCILE_INTERVAL_MS = 250;  // Min interval between reconciles
  
  // Reconcile helpers
  int getDesiredIndex(SensorType type) const;
  void setDesiredTarget(SensorType type, const char* name, const char* mac, int8_t addrType, bool enabled, bool forceApplyNow);
  void scheduleReconcile();
  
  /**
   * @brief Single-writer reconcile function (ONLY place that calls BikeSensorServerAdd/Remove)
   * 
   * Policy:
   * - If desired enabled=false and not connected: RemoveSensor
   * - If desired enabled=true and name differs from applied:
   *   - If connected and forceApplyNow=false: defer (apply-on-disconnect)
   *   - If connected and forceApplyNow=true: controlled reconnect
   *   - If not connected: apply immediately
   */
  void reconcileSensorTargets();
  
  // Internal helpers
  SensorType stringToSensorType(const char* type);
  const char* sensorTypeToString(SensorType type);
  void updateTelemetry();
  void onSensorConnected(SensorType type, const char* name);
  void onSensorDisconnected(SensorType type, const char* name);
  void onHeartRateChanged(uint16_t hr);
  void onDiscoveryComplete(uint8_t count);
  
  // Static callbacks (trampoline to instance)
  static void staticOnHRConnected();
  static void staticOnHRDisconnected();
  static void staticOnCSCConnected();
  static void staticOnCSCDisconnected();
  static void staticOnHeartRateChanged(uint16_t hr);
  static void staticOnSpeedCadenceChanged(uint16_t speed, uint16_t cadence);
  static void staticOnDiscoveryComplete(uint8_t count);
  
  static BLERelayManager* _instance;  // For static callbacks
};

#endif // HW_ENABLE_BLE

// ============================================================
// Stub definitions when BLE is disabled
// ============================================================
#if !HW_ENABLE_BLE

// Forward declarations
#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

static const uint8_t BLE_SENSOR_NAME_MAX = 64;

struct SensorInfo {
  char name[BLE_SENSOR_NAME_MAX];
  char type[4];
  char mac[18];
  bool connected;
  int battery;
  
  SensorInfo() : connected(false), battery(-1) {
    name[0] = '\0';
    type[0] = '\0';
    mac[0] = '\0';
  }
  
  void setName(const char* n) {
    if (n) { strncpy(name, n, sizeof(name) - 1); name[sizeof(name) - 1] = '\0'; }
    else { name[0] = '\0'; }
  }
  void setType(const char* t) {
    if (t) { strncpy(type, t, sizeof(type) - 1); type[sizeof(type) - 1] = '\0'; }
    else { type[0] = '\0'; }
  }
  void setMac(const char* m) {
    if (m) { strncpy(mac, m, sizeof(mac) - 1); mac[sizeof(mac) - 1] = '\0'; }
    else { mac[0] = '\0'; }
  }
};

struct DiscoveredSensor {
  char name[BLE_SENSOR_NAME_MAX];
  char type[4];
  char mac[18];
  int8_t addrType;  // BLE address type (0=Public, 1=Random, -1=Unknown)
  
  DiscoveredSensor() : addrType(-1) {
    name[0] = '\0';
    type[0] = '\0';
    mac[0] = '\0';
  }
  
  void setName(const char* n) {
    if (n) { strncpy(name, n, sizeof(name) - 1); name[sizeof(name) - 1] = '\0'; }
    else { name[0] = '\0'; }
  }
  void setType(const char* t) {
    if (t) { strncpy(type, t, sizeof(type) - 1); type[sizeof(type) - 1] = '\0'; }
    else { type[0] = '\0'; }
  }
  void setMac(const char* m) {
    if (m) { strncpy(mac, m, sizeof(mac) - 1); mac[sizeof(mac) - 1] = '\0'; }
    else { mac[0] = '\0'; }
  }
};

/**
 * @class BLERelayManager (stub when BLE disabled)
 */
class BLERelayManager {
public:
  BLERelayManager();
  ~BLERelayManager();
  
#if HOMEWIND_ENABLE_WEBSOCKET
  bool begin(WebSocketTelemetry* telemetry);
#else
  bool begin();
#endif
  
  void loop();
  bool loadConfiguredSensors();
  bool getSensorConnectionState(const char* sensorName, String& type, String& mac, bool& connected, int& battery);
  uint8_t getConfiguredSensors(SensorInfo* sensors, uint8_t maxSensors);
  bool setConfiguredSensor(const char* name, const char* type, const char* mac = nullptr, int8_t addrType = -1, bool forceApplyNow = false);
  bool deleteConfiguredSensor(const char* name);
  bool connectSensor(const char* name);
  bool disconnectSensor(const char* name);
  bool reloadSensors();
  bool startDiscovery(const char* type = nullptr);
  bool stopDiscovery();
  bool isDiscoveryActive() const { return false; }
  uint8_t getDiscoveryResults(DiscoveredSensor* results, uint8_t maxResults);
  bool pauseServer();
  bool resumeServer();
  bool isServerPaused() const { return false; }
  uint8_t getConfiguredSensorCount() const { return 0; }
  uint8_t getConnectedSensorCount() const { return 0; }
  uint8_t getLastDiscoveryCount() const { return 0; }
  void stopForMaintenance();
  bool isStoppedForMaintenance() const { return false; }

private:
  bool initialized;
  bool _maintenanceStopped;
  bool _serverPaused;
  uint8_t _connectedCount;
  uint8_t _lastDiscoveryCount;
#if HOMEWIND_ENABLE_WEBSOCKET
  WebSocketTelemetry* _telemetry;
#endif
  uint8_t _sensorCount;
  bool _discoveryActive;
  char _discoveryType[4];  // Match main class
  bool _reconcileScheduled;
  unsigned long _lastReconcileMs;
  
  static uint16_t calcCRC16(const uint8_t* data, size_t len);
  bool saveConfigToNVS();
  bool loadConfigFromNVS();
  
  static BLERelayManager* _instance;
};

#endif // !HW_ENABLE_BLE

#endif // HOMEWIND_BLE_RELAY_MANAGER_H

