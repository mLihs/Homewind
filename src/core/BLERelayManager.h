/**
 * @file BLERelayManager.h
 * @brief BLE Relay Manager - integrates BikeSensor 3.0 with Homewind
 * 
 * Manages BLE sensor connections (HR, CSC), discovery, persistence,
 * and WebSocket telemetry updates.
 * 
 * Uses deterministic struct-based NVS storage (no heap allocation for persistence).
 *
 * Task boundary (design §7.2, §7.3): web handlers only enqueue commands or
 * read the _bleMux-protected snapshots. loop() is the only caller of BikeSensor,
 * the only writer of _configuredSensors / NVS, and library callbacks only record.
 */

#ifndef HOMEWIND_BLE_RELAY_MANAGER_H
#define HOMEWIND_BLE_RELAY_MANAGER_H

#include <Arduino.h>
#include <cstring>
#include "../app/Config.h"

#if HW_ENABLE_BLE

#include <BikeSensor.h>
#include <Preferences.h>
#include "BleCommandState.h"

// Forward declarations
#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

/**
 * @brief Name buffer size for BLE sensor names (deterministic, no heap)
 */
static const uint8_t BLE_SENSOR_NAME_MAX = 64;

/**
 * @brief Discovery snapshot capacity: what always fits into one DISCOVERY_RESULTS
 * frame (static_assert in WebSocketTelemetry.cpp). tools/build.sh sets
 * -DBBS_MAX_DISCOVERED to the same value.
 */
static const uint8_t BLE_DISCOVERY_MAX = 15;
static_assert(BBS_MAX_DISCOVERED == BLE_DISCOVERY_MAX, "tools/build.sh must set -DBBS_MAX_DISCOVERED=15");

/**
 * @struct HwSensorInfo
 * @brief Configured sensor as seen by web/display (fixed buffers, no heap)
 */
struct HwSensorInfo {
  char name[BLE_SENSOR_NAME_MAX];  // 64 bytes
  char type[4];                     // "HR", "CSC" + null
  char mac[18];                     // "aa:bb:cc:dd:ee:ff" + null
  bool connected;
  int battery;                      // 0-100, or -1 if unknown
  bool duplicate;                   // library rejected the MAC: another type holds it
  
  HwSensorInfo() : connected(false), battery(-1), duplicate(false) {
    name[0] = '\0';
    type[0] = '\0';
    mac[0] = '\0';
  }
};

/**
 * @struct HwDiscoveredSensor
 * @brief One record of the discovery snapshot (copy of BikeSensor::DiscoveredSensor)
 */
struct HwDiscoveredSensor {
  char name[32];                    // "" = no name seen
  uint8_t mac[6];
  uint8_t addrType;                 // 0 public, 1 random
  uint8_t typeMask;                 // advertised ∩ requested types (bit 0 HR, bit 1 CSC)
};

/**
 * @brief Result of a web-side enqueue (HTTP mapping in ApiActions)
 */
enum class BleEnqueueResult : uint8_t {
  ACCEPTED,    // 200
  FULL,        // 503
  NOT_FOUND,   // 404 (unknown sensor name)
  STALE,       // 409 (discovery generation/index no longer valid)
  CLOSED,      // 409 (maintenance admission closed)
};

/**
 * @class BLERelayManager
 * @brief BLE Relay Manager - integrates BikeSensor 3.0 with Homewind
 */
class BLERelayManager {
public:
  BLERelayManager();
  ~BLERelayManager();
  
  /**
   * Initialize BLE relay manager (loop task). Loads NVS, calls BikeSensor::begin().
   * @return false if BikeSensor::begin() failed
   */
#if HOMEWIND_ENABLE_WEBSOCKET
  bool begin(WebSocketTelemetry* telemetry = nullptr);
#else
  bool begin();
#endif
  
  /**
   * Main loop (call from App::loop()). Calls BikeSensor::update() every pass,
   * also after maintenance ACK.
   */
  void loop();
  
  /**
   * Check if BLE is ready
   */
  bool ready() const { return initialized; }
  
  // ---- Any task: snapshot readers ----------------------------------------
  uint8_t getConfiguredSensors(HwSensorInfo* sensors, uint8_t maxSensors);
  bool isDiscoveryActive();
  uint8_t discoveryError();          // 0 none, 1 busy (last START refused)
  uint8_t discoverySnapshot(uint32_t& generation);
  bool discoveredSensor(uint8_t index, HwDiscoveredSensor& out);

  // ---- Any task: commands, applied in loop() ------------------------------
  BleEnqueueResult enqueueSetFromDiscovery(uint32_t generation, uint8_t index, bool force);
  BleEnqueueResult enqueueDelete(const char* name);
  BleEnqueueResult enqueueConnect(const char* name);
  BleEnqueueResult enqueueDisconnect(const char* name);
  BleEnqueueResult enqueueReload();
  BleEnqueueResult requestDiscoveryStart(uint8_t typeMask);
  BleEnqueueResult requestDiscoveryStop();
  BleEnqueueResult requestServerPaused(bool paused);

  // ---- Loop task ----------------------------------------------------------
  bool isServerPaused() const { return _serverPaused; }
  uint8_t getConfiguredSensorCount() const { return _sensorCount; }
  uint8_t getConnectedSensorCount() const { return _connectedCount; }
  uint8_t getLastDiscoveryCount() const { return _lastDiscoveryCount; }
  bool lastHeartRate(uint16_t& bpm) const { bpm = _hrBpm; return _hrValid; }

  // ---- Maintenance (loop task, called from MaintenanceMode::loop) --------
  bool quiesceForMaintenance();      // true: a discovery was open
  bool maintenanceDiscoveryDone() const { return _maintDiscoveryDone; }
  bool maintenanceQuiesced();
  bool removeSensorsForMaintenance();
  void stopForMaintenance();         // discard queues; afterwards loop() only calls update()
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
  
  // Sensor configuration cache (fixed buffers, no heap fragmentation). Loop task only.
  struct SensorConfig {
    char name[BLE_SENSOR_NAME_MAX];  // 64 bytes
    char type[4];                     // "HR", "CSC" + null
    char mac[18];                     // "aa:bb:cc:dd:ee:ff" + null
    int8_t addrType;                  // BLE address type (0=Public, 1=Random, -1=Unknown)
    uint8_t typeBit;                  // hw::kBleTypeHR / kBleTypeCSC, 0 = invalid
  };
  
  static const uint8_t MAX_SENSORS = 2;  // HR, CSC
  SensorConfig _configuredSensors[MAX_SENSORS];
  uint8_t _sensorCount;
  
  bool saveConfigToNVS();
  bool loadConfigFromNVS();

  // Guards everything below that web tasks touch.
  portMUX_TYPE _bleMux;
  hw::BleCommandQueue _commands;
  hw::BleDiscoveryQueue _discoveryOps;
  int8_t _pauseDesired;              // -1 none, 0 resume, 1 pause
  bool _discoveryActive;             // clears only on DISCOVERY_DONE
  uint8_t _discoveryError;
  uint8_t _discoveryRequestedMask;   // mask of the discovery started last
  uint8_t _discoveredMask;           // mask of the discovery in the snapshot
  HwSensorInfo _snapshot[MAX_SENSORS];
  uint8_t _snapshotCount;
  HwDiscoveredSensor _discovered[BLE_DISCOVERY_MAX];
  uint8_t _discoveredCount;
  uint32_t _discoveredGeneration;

  // Reconcile (loop task only)
  hw::BleReconciler _reconciler;
  bool _reconcileScheduled;
  unsigned long _lastReconcileMs;
  static const unsigned long RECONCILE_INTERVAL_MS = 250;  // Min interval between reconciles
  bool _discoveryStopIssued;
  bool _maintDiscoveryDone;
  unsigned long _lastSnapshotMs;

  // Callback records. Library callbacks run inside BikeSensor::update() on the
  // loop task; they only append here and the same loop() pass drains it.
  struct EventRecord { uint8_t type; uint8_t sensorType; int reason; };
  struct DoneRecord { uint32_t generation; uint8_t count; uint8_t reason; uint8_t mask; };
  static const uint8_t EVENT_FIFO_DEPTH = BBS_EVENT_QUEUE_DEPTH;
  EventRecord _events[EVENT_FIFO_DEPTH];
  uint8_t _eventCount;
  bool _eventsDropped;
  DoneRecord _done[2];
  uint8_t _doneCount;
  uint16_t _hrBpm;
  bool _hrValid;
  bool _hrNew;
  uint16_t _cadenceRpm;
  bool _cadenceNew;

  BleEnqueueResult pushCommand(const hw::BleCommand& cmd);
  BleEnqueueResult enqueueByName(hw::BleCmdKind kind, const char* name);
  bool configuredNameExistsLocked(const char* name) const;
  int findConfigByName(const char* name) const;
  int findConfigByType(uint8_t typeBit) const;
  void applyCommand(const hw::BleCommand& cmd);
  void applySetFromDiscovery(uint32_t generation, uint8_t index, bool force);
  void setTargetsFromConfig();
  void processDiscoveryOps();
  void processEvents();
  void adoptSensorAddress(uint8_t typeBit);
  void runReconcile(bool throttled);
  void refreshSnapshot();
  void markSensorsDirty();
  void notifyDisplay(uint8_t typeBit);

  static uint8_t stringToTypeBit(const char* type);
  static const char* typeBitToString(uint8_t typeBit);

  static void staticOnEvent(const BikeSensor::EventInfo& e);
  static void staticOnHeartRate(uint16_t bpm);
  static void staticOnCadence(uint16_t rpm);
  
  static BLERelayManager* _instance;  // For static callbacks
};

#endif // HW_ENABLE_BLE

// ============================================================
// Stub definitions when BLE is disabled
// ============================================================
#if !HW_ENABLE_BLE

#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

static const uint8_t BLE_SENSOR_NAME_MAX = 64;

struct HwSensorInfo {
  char name[BLE_SENSOR_NAME_MAX];
  char type[4];
  char mac[18];
  bool connected;
  int battery;
  bool duplicate;
  HwSensorInfo() : connected(false), battery(-1), duplicate(false) {
    name[0] = '\0';
    type[0] = '\0';
    mac[0] = '\0';
  }
};

/**
 * @class BLERelayManager (stub when BLE disabled)
 */
class BLERelayManager {
public:
#if HOMEWIND_ENABLE_WEBSOCKET
  bool begin(WebSocketTelemetry*) { return false; }
#else
  bool begin() { return false; }
#endif
  void loop() {}
  bool ready() const { return false; }
  uint8_t getConfiguredSensors(HwSensorInfo*, uint8_t) { return 0; }
  bool isServerPaused() const { return false; }
  uint8_t getConfiguredSensorCount() const { return 0; }
  uint8_t getConnectedSensorCount() const { return 0; }
  uint8_t getLastDiscoveryCount() const { return 0; }
};

#endif // !HW_ENABLE_BLE

#endif // HOMEWIND_BLE_RELAY_MANAGER_H
