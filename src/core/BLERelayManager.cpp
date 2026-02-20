/**
 * @file BLERelayManager.cpp
 * @brief BLE Relay Manager implementation
 */

#include "../app/Config.h"

#if HW_ENABLE_BLE

#include "BLERelayManager.h"
#include "../core/DebugLog.h"
#include "../core/Crc16.h"

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
#endif

#if HW_ENABLE_FANS && HW_ENABLE_BLE
#include "../core/FanHRAdapter.h"
extern FanHRAdapter* g_fanHrAdapter;
#endif

#if HW_ENABLE_DISPLAY
#include "../core/DisplayManager.h"
extern DisplayManager* g_displayManager;
#endif

// Static instance pointer for callbacks
BLERelayManager* BLERelayManager::_instance = nullptr;

BLERelayManager::BLERelayManager()
  : initialized(false)
  , _maintenanceStopped(false)
  , _serverPaused(false)
  , _connectedCount(0)
  , _lastDiscoveryCount(0)
#if HOMEWIND_ENABLE_WEBSOCKET
  , _telemetry(nullptr)
#endif
  , _sensorCount(0)
  , _discoveryActive(false)
  , _reconcileScheduled(false)
  , _lastReconcileMs(0)
{
  _instance = this;
  memset(_configuredSensors, 0, sizeof(_configuredSensors));
  _discoveryType[0] = '\0';  // Initialize discovery type
  
  // Initialize reconcile state arrays
  for (uint8_t i = 0; i < MAX_SENSORS; i++) {
    _desiredTargets[i] = DesiredSensorTarget();
    _appliedState[i] = AppliedSensorState();
  }
}

BLERelayManager::~BLERelayManager() {
  if (_instance == this) {
    _instance = nullptr;
  }
  // No heap allocations to clean up (deterministic storage)
}

#if HOMEWIND_ENABLE_WEBSOCKET
bool BLERelayManager::begin(WebSocketTelemetry* telemetry) {
#else
bool BLERelayManager::begin() {
#endif
  if (initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[BLE] Initializing BLE Relay Manager (deterministic storage)");
  
#if HOMEWIND_ENABLE_WEBSOCKET
  _telemetry = telemetry;
#endif
  
  // STEP 1: Load sensor configurations from NVS BEFORE initializing BLE
  // This ensures MAC-based connections can be established immediately on boot
  HW_DEBUG_PRINTLN("[BLE] Loading sensor configurations from NVS (before BLE init)...");
  if (!loadConfigFromNVS()) {
    HW_DEBUG_PRINTLN("[BLE] No sensor configuration found, starting fresh");
    _sensorCount = 0;
  }
  
  // STEP 2: Initialize BluetoothBikeSensorServer with MAC-based mode (no scanning)
  // Default: HR and CSC sensor types enabled, device name prefix "homewind"
  BluetoothBikeSensorServerBegin("homewind", 
                                 SENSOR_TYPE_HEART_RATE | 
                                 SENSOR_TYPE_SPEED_CADENCE,
                                 BBS_MODE_MAC);
  
  // Set callbacks
  BluetoothBikeSensorServerSetHeartRateCallback(staticOnHeartRateChanged);
  BluetoothBikeSensorServerSetSpeedCadenceCallback(staticOnSpeedCadenceChanged);
  BikeSensorServerSetDiscoveryCompleteCallback(staticOnDiscoveryComplete);
  
  // Set connection callbacks for each sensor type
  BikeSensorServerSetSensorConn(SENSOR_TYPE_HEART_RATE, staticOnHRConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_HEART_RATE, staticOnHRDisconnected);
  BikeSensorServerSetSensorConn(SENSOR_TYPE_SPEED_CADENCE, staticOnCSCConnected);
  BikeSensorServerSetSensorDisc(SENSOR_TYPE_SPEED_CADENCE, staticOnCSCDisconnected);
  
  // MAC-based mode doesn't need preserve MAC setting (MAC is always used)
  
  // Configure discovery settings (matching DiscoveryExample for optimal results)
  // Default is 20 seconds, but 15 seconds matches the working example
  BikeSensorServerSetDiscoveryDuration(15000);  // 15 seconds scan duration (matching example)
  BikeSensorServerSetMaxSensors(50);            // Allow up to 50 sensors (matching example)
  HW_DEBUG_PRINTF("[BLE] Discovery configured: Duration=%u ms, MaxSensors=%u\n", 
                  BikeSensorServerGetDiscoveryDuration(), BikeSensorServerGetMaxSensors());
  
  // STEP 3: Populate desired state from loaded configuration (Single-Writer Reconcile Pattern)
  // Instead of calling BikeSensorServerAddSensorByMac() directly, we set desired targets
  // and let reconcileSensorTargets() be the single point of Add/Remove calls.
  // This eliminates race conditions and instant disconnects from duplicate Add calls.
  for (uint8_t i = 0; i < _sensorCount; i++) {
    SensorConfig& cfg = _configuredSensors[i];
    if (strlen(cfg.name) > 0 && cfg.sensorType != static_cast<SensorType>(0)) {
      // Set desired target with MAC address (reconcile will apply it)
      // Only set if MAC is available (required for MAC-based mode)
      if (strlen(cfg.mac) > 0) {
        setDesiredTarget(cfg.sensorType, cfg.name, cfg.mac, cfg.addrType, true, false);
        HW_DEBUG_PRINTF("[BLE] Set desired target: %s (%s) MAC=%s addrType=%d\n", 
                       cfg.name, cfg.type, cfg.mac, cfg.addrType);
      } else {
        HW_DEBUG_PRINTF("[BLE] Skipping sensor %s (%s) - no MAC address\n", 
                       cfg.name, cfg.type);
      }
    }
  }
  
  initialized = true;
  
  // STEP 4: Run initial reconcile to apply desired targets
  // This is the ONLY place that will call BikeSensorServerAddSensorByMac()
  HW_DEBUG_PRINTLN("[BLE] Running initial reconcile...");
  reconcileSensorTargets();
  HW_DEBUG_PRINTLN("[BLE] BLE Relay Manager initialized");
  
  updateTelemetry();

#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->updateHRState();
    g_displayManager->updateCSCState();
  }
#endif
  
  return true;
}

void BLERelayManager::loop() {
  if (!initialized || _maintenanceStopped) {
    return;
  }
  
  // Call library main loop
  BluetoothBikeSensorServerLoop();
  
  // Run reconcile if scheduled or periodically (throttled internally)
  // This is the single point where BLE Add/Remove operations happen
  if (_reconcileScheduled) {
    reconcileSensorTargets();
  }
  
  // Periodically update telemetry (throttled)
  // Skip telemetry updates when server is paused to avoid UI interference
  if (!_serverPaused) {
    static unsigned long lastTelemetryUpdate = 0;
    unsigned long now = millis();
    if (now - lastTelemetryUpdate > 1000) {  // Update every 1 second
      updateTelemetry();
      lastTelemetryUpdate = now;
    }
  }
}

bool BLERelayManager::loadConfiguredSensors() {
  HW_DEBUG_PRINTLN("[BLE] Loading configured sensors from NVS (BLE side-effect free)");
  
  if (!loadConfigFromNVS()) {
    HW_DEBUG_PRINTLN("[BLE] No sensor configuration found, starting fresh");
    _sensorCount = 0;
    return true;  // Not an error - just no config yet
  }
  
  // Set desired targets for all loaded sensors
  for (uint8_t i = 0; i < _sensorCount; i++) {
    SensorConfig& cfg = _configuredSensors[i];
    if (strlen(cfg.name) > 0 && cfg.sensorType != static_cast<SensorType>(0)) {
      // Set desired target with MAC (DO NOT call BikeSensorServerAddSensorByMac here!)
      // Reconcile will apply the change at the appropriate time
      // Only set if MAC is available (required for MAC-based mode)
      if (strlen(cfg.mac) > 0) {
        setDesiredTarget(cfg.sensorType, cfg.name, cfg.mac, cfg.addrType, true, false);
        HW_DEBUG_PRINTF("[BLE] Loaded sensor config: %s (%s) MAC=%s addrType=%d - reconcile will apply\n", 
                       cfg.name, cfg.type, cfg.mac, cfg.addrType);
      } else {
        HW_DEBUG_PRINTF("[BLE] Skipping sensor %s (%s) - no MAC address\n", 
                       cfg.name, cfg.type);
      }
    }
  }
  
  // Schedule reconcile to apply changes
  scheduleReconcile();
  
  updateTelemetry();
  return true;
}

bool BLERelayManager::getSensorConnectionState(const char* sensorName, String& type, 
                                                String& mac, bool& connected, int& battery) {
  if (!sensorName) {
    return false;
  }
  
  // Find sensor in configured list
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (strcmp(_configuredSensors[i].name, sensorName) == 0) {
      type = _configuredSensors[i].type;
      
      // Get connection state from library
      connected = BikeSensorServerIsSensorConnected(_configuredSensors[i].sensorType);
      
      // Get MAC address
      char macBuf[18];
      if (BikeSensorServerGetSensorMac(_configuredSensors[i].sensorType, macBuf, sizeof(macBuf))) {
        mac = macBuf;
      } else {
        mac = _configuredSensors[i].mac;  // Fallback to stored MAC
      }
      
      // Get battery level
      uint8_t batLevel;
      if (BikeSensorServerGetBatteryLevel(_configuredSensors[i].sensorType, &batLevel)) {
        battery = batLevel;
      } else {
        battery = -1;  // Unknown
      }
      
      return true;
    }
  }
  
  return false;
}

uint8_t BLERelayManager::getConfiguredSensors(SensorInfo* sensors, uint8_t maxSensors) {
  if (!sensors || maxSensors == 0) {
    return 0;
  }
  
  uint8_t count = 0;
  for (uint8_t i = 0; i < _sensorCount && count < maxSensors; i++) {
    sensors[count].setName(_configuredSensors[i].name);
    sensors[count].setType(_configuredSensors[i].type);
    
    // Get connection state
    sensors[count].connected = BikeSensorServerIsSensorConnected(_configuredSensors[i].sensorType);
    
    // Get MAC address
    char macBuf[18];
    if (BikeSensorServerGetSensorMac(_configuredSensors[i].sensorType, macBuf, sizeof(macBuf))) {
      sensors[count].setMac(macBuf);
    } else {
      sensors[count].setMac(_configuredSensors[i].mac);
    }
    
    // Get battery level
    uint8_t batLevel;
    if (BikeSensorServerGetBatteryLevel(_configuredSensors[i].sensorType, &batLevel)) {
      sensors[count].battery = batLevel;
    } else {
      sensors[count].battery = -1;
    }
    
    count++;
  }
  
  return count;
}

bool BLERelayManager::setConfiguredSensor(const char* name, const char* type, const char* mac, int8_t addrType, bool forceApplyNow) {
  if (!name || !type) {
    return false;
  }
  
  SensorType sensorType = stringToSensorType(type);
  if (sensorType == static_cast<SensorType>(0)) {
    return false;
  }
  
  HW_DEBUG_PRINTF("[BLE] setConfiguredSensor: name=\"%s\" type=%s mac=%s addrType=%d force=%d\n",
                 name, type, mac ? mac : "", addrType, forceApplyNow);
  
  // Check if sensor already exists (by type, since we only support one per type)
  int existingIndex = -1;
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (_configuredSensors[i].sensorType == sensorType) {
      existingIndex = i;
      break;
    }
  }
  
  // Update or add sensor config (DO NOT call BikeSensorServerRemoveSensor here!)
  if (existingIndex >= 0) {
    _configuredSensors[existingIndex].setName(name);
    _configuredSensors[existingIndex].setType(type);
    _configuredSensors[existingIndex].setMac(mac);
    // Update addrType if provided (>= 0), otherwise keep existing
    if (addrType >= 0) {
      _configuredSensors[existingIndex].addrType = addrType;
    }
  } else {
    if (_sensorCount >= MAX_SENSORS) {
      return false;  // Too many sensors
    }
    SensorConfig& cfg = _configuredSensors[_sensorCount];
    cfg.setName(name);
    cfg.setType(type);
    cfg.setMac(mac);
    cfg.addrType = (addrType >= 0) ? addrType : -1;  // Use provided addrType or -1 (Unknown/auto-detect)
    cfg.sensorType = sensorType;
    _sensorCount++;
  }
  
  // Save to NVS (deterministic struct-based storage)
  if (!saveConfigToNVS()) {
    HW_ERROR_PRINTLN("[BLE] Failed to save sensor configuration");
    return false;
  }
  
  // Set desired target with MAC (DO NOT call BikeSensorServerAddSensorByMac here!)
  // Reconcile will apply the change according to policy:
  // - forceApplyNow=true: controlled reconnect (disconnect then add)
  // - forceApplyNow=false: apply-on-disconnect (wait until natural disconnect)
  // Look up MAC from config and determine addrType to use
  const char* macAddr = nullptr;
  int8_t addrTypeToUse = addrType;  // Use provided addrType by default
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (_configuredSensors[i].sensorType == sensorType && 
        strcmp(_configuredSensors[i].name, name) == 0) {
      macAddr = _configuredSensors[i].mac;
      // If addrType not provided (== -1) but exists in config, use existing
      if (addrType < 0 && _configuredSensors[i].addrType >= 0) {
        addrTypeToUse = _configuredSensors[i].addrType;
      }
      break;
    }
  }
  if (macAddr && strlen(macAddr) > 0) {
    setDesiredTarget(sensorType, name, macAddr, addrTypeToUse, true, forceApplyNow);
  } else {
    HW_DEBUG_PRINTF("[BLE] setConfiguredSensor: No MAC address for %s (%s), cannot set target\n",
                   name, type);
    return false;
  }
  
  // Schedule reconcile (will run in loop() or immediately if not connected)
  scheduleReconcile();
  
  updateTelemetry();
  return true;
}

bool BLERelayManager::deleteConfiguredSensor(const char* name) {
  if (!name) {
    return false;
  }
  
  HW_DEBUG_PRINTF("[BLE] deleteConfiguredSensor: name=\"%s\"\n", name);
  
  // Find and remove sensor
  int foundIndex = -1;
  SensorType sensorType = static_cast<SensorType>(0);
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (strcmp(_configuredSensors[i].name, name) == 0) {
      foundIndex = i;
      sensorType = _configuredSensors[i].sensorType;
      break;
    }
  }
  
  if (foundIndex < 0) {
    return false;  // Sensor not found
  }
  
  // Set desired target disabled (DO NOT call BikeSensorServerRemoveSensor here!)
  // Force apply to disconnect immediately
  setDesiredTarget(sensorType, "", "", -1, false, true);
  
  // Remove from config array (shift remaining)
  for (uint8_t i = foundIndex; i < _sensorCount - 1; i++) {
    _configuredSensors[i] = _configuredSensors[i + 1];
  }
  _sensorCount--;
  
  // Save to NVS (deterministic struct-based storage)
  saveConfigToNVS();
  
  // Run reconcile immediately to remove from library
  reconcileSensorTargets();
  
  updateTelemetry();
  
  return true;
}

bool BLERelayManager::connectSensor(const char* name) {
  if (!name) {
    return false;
  }
  
  HW_DEBUG_PRINTF("[BLE] connectSensor: name=\"%s\"\n", name);
  
  // Find sensor in config
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (strcmp(_configuredSensors[i].name, name) == 0) {
      // Set desired target enabled with MAC (DO NOT call BikeSensorServerAddSensorByMac here!)
      // Reconcile will apply the change - library will auto-connect once added
      if (strlen(_configuredSensors[i].mac) > 0) {
        setDesiredTarget(_configuredSensors[i].sensorType, 
                         _configuredSensors[i].name,
                         _configuredSensors[i].mac,
                         _configuredSensors[i].addrType,
                         true,    // enabled
                         false);  // don't force if already connected
      } else {
        HW_DEBUG_PRINTF("[BLE] connectSensor: No MAC address for %s\n", name);
        return false;
      }
      
      // Schedule reconcile
      scheduleReconcile();
      
      updateTelemetry();
      return true;
    }
  }
  
  return false;  // Sensor not found
}

bool BLERelayManager::disconnectSensor(const char* name) {
  if (!name) {
    return false;
  }
  
  HW_DEBUG_PRINTF("[BLE] disconnectSensor: name=\"%s\"\n", name);
  
  // Find sensor in config
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (strcmp(_configuredSensors[i].name, name) == 0) {
      // Set desired target disabled (DO NOT call BikeSensorServerRemoveSensor here!)
      // We use forceApplyNow=true to actually disconnect now
      setDesiredTarget(_configuredSensors[i].sensorType, 
                       _configuredSensors[i].name,
                       _configuredSensors[i].mac,
                       _configuredSensors[i].addrType,
                       false,   // disabled
                       true);   // force apply now (disconnect immediately)
      
      // Schedule reconcile
      scheduleReconcile();
      
      // Run reconcile immediately for disconnect
      reconcileSensorTargets();
      
      updateTelemetry();
      return true;
    }
  }
  
  return false;  // Sensor not found
}

bool BLERelayManager::reloadSensors() {
  HW_DEBUG_PRINTLN("[BLE] reloadSensors: clearing and reloading configuration");
  
  // Mark all current sensors as disabled (force remove)
  for (uint8_t i = 0; i < _sensorCount; i++) {
    setDesiredTarget(_configuredSensors[i].sensorType, "", "", -1, false, true);
  }
  
  // Run reconcile to remove all sensors
  reconcileSensorTargets();
  
  // Clear config
  _sensorCount = 0;
  
  // Reload from NVS (will set desired targets and schedule reconcile)
  return loadConfiguredSensors();
}

bool BLERelayManager::startDiscovery(const char* type) {
  if (_discoveryActive) {
    return false;  // Already discovering
  }
  
  uint8_t sensorTypeMask = 0;
  
  if (!type || strlen(type) == 0) {
    // Discover all types - match library example format exactly
    sensorTypeMask = SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE;
    HW_DEBUG_PRINTLN("[BLE] Starting discovery for all sensor types");
    HW_DEBUG_PRINTF("[BLE] Calling BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE | SENSOR_TYPE_SPEED_CADENCE)\n");
  } else {
    SensorType st = stringToSensorType(type);
    if (st == static_cast<SensorType>(0)) {
      HW_DEBUG_PRINTF("[BLE] Invalid sensor type for discovery: %s\n", type);
      return false;
    }
    
    // Use SensorType enum directly - matches library example format exactly
    sensorTypeMask = static_cast<uint8_t>(st);
    HW_DEBUG_PRINTF("[BLE] Starting discovery for type: %s\n", type);
    
    // Pass enum constant directly for clarity (matches library examples)
    if (st == SENSOR_TYPE_HEART_RATE) {
      HW_DEBUG_PRINTLN("[BLE] Calling BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE)");
      BikeSensorServerStartDiscovery(SENSOR_TYPE_HEART_RATE);
    } else if (st == SENSOR_TYPE_SPEED_CADENCE) {
      HW_DEBUG_PRINTLN("[BLE] Calling BikeSensorServerStartDiscovery(SENSOR_TYPE_SPEED_CADENCE)");
      BikeSensorServerStartDiscovery(SENSOR_TYPE_SPEED_CADENCE);
    } else {
      HW_DEBUG_PRINTF("[BLE] ERROR: Unknown sensor type: %d\n", static_cast<int>(st));
      return false;
    }
    
    _discoveryActive = true;
    if (type) {
      strncpy(_discoveryType, type, sizeof(_discoveryType) - 1);
      _discoveryType[sizeof(_discoveryType) - 1] = '\0';
    } else {
      _discoveryType[0] = '\0';
    }
    
#if HOMEWIND_ENABLE_WEBSOCKET
    if (_telemetry) {
      _telemetry->markDirty(TelemetryFrameType::DISCOVERY_STATUS);
    }
#endif
    
    return true;
  }
  
  // For "all types" case
  _discoveryActive = true;
  _discoveryType[0] = '\0';
  
  // Pass combined mask - matches library example format
  BikeSensorServerStartDiscovery(sensorTypeMask);
  
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::DISCOVERY_STATUS);
  }
#endif
  
  return true;
}

bool BLERelayManager::stopDiscovery() {
  if (!_discoveryActive) {
    return false;
  }
  
  BikeSensorServerStopDiscovery();
  _discoveryActive = false;
  
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::DISCOVERY_STATUS);
  }
#endif
  
  return true;
}

uint8_t BLERelayManager::getDiscoveryResults(DiscoveredSensor* results, uint8_t maxResults) {
  if (!results || maxResults == 0) {
    return 0;
  }
  
  uint8_t count = BikeSensorServerGetSensorCount();
  if (count > maxResults) {
    count = maxResults;
  }
  
  // Get formatted list string to extract address type
  // Format: "[HR] Name (MAC) 0\n" - last number before \n is address type
  const char* listStr = BikeSensorServerListSensors(LIST_FORMAT_FULL);
  
  for (uint8_t i = 0; i < count; i++) {
    SensorType type;
    char name[64];
    uint8_t mac[6];
    char macStr[18];
    
    if (BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
      snprintf(macStr, sizeof(macStr), "%02x:%02x:%02x:%02x:%02x:%02x",
               mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
      results[i].setName(name);
      results[i].setType(sensorTypeToString(type));
      results[i].setMac(macStr);
      
      // Extract address type from formatted list string
      // Format: "[HR] Name (MAC) 0\n" - find line with this MAC and parse last number
      results[i].addrType = -1;  // Default: Unknown
      if (listStr && macStr[0] != '\0') {
        // Find the line containing this MAC address
        // Pattern: " (MAC) " followed by number, then \n or end of string
        char searchPattern[25];
        snprintf(searchPattern, sizeof(searchPattern), " (%s) ", macStr);
        const char* macPos = strstr(listStr, searchPattern);
        if (macPos) {
          // Find the address type: number after " (MAC) "
          const char* afterMac = macPos + strlen(searchPattern);
          // Skip any spaces
          while (*afterMac == ' ') afterMac++;
          // Parse the number (should be 0 or 1, followed by \n or end of string)
          if (*afterMac >= '0' && *afterMac <= '1') {
            int addrType = (*afterMac - '0');
            // Verify it's followed by \n, \r, \0, or space (for next line)
            char nextChar = *(afterMac + 1);
            if (nextChar == '\n' || nextChar == '\0' || nextChar == '\r' || nextChar == ' ') {
              results[i].addrType = (int8_t)addrType;
              HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: Extracted addrType=%d for MAC=%s\n", 
                             results[i].addrType, macStr);
            } else {
              HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: Invalid addrType format for MAC=%s (next='%c', ascii=%d)\n", 
                             macStr, nextChar, nextChar);
            }
          } else {
            HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: Failed to parse addrType for MAC=%s (char='%c', ascii=%d)\n", 
                           macStr, *afterMac ? *afterMac : '?', *afterMac);
            // Debug: show context around MAC
            if (macPos) {
              char context[60];
              size_t contextLen = strlen(searchPattern) + 10;
              if (contextLen > sizeof(context) - 1) contextLen = sizeof(context) - 1;
              strncpy(context, macPos, contextLen);
              context[contextLen] = '\0';
              HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: Context: '%s'\n", context);
            }
          }
        } else {
          HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: MAC=%s not found in list string (pattern='%s')\n", 
                         macStr, searchPattern);
          // Debug: print first 200 chars of listStr to help diagnose
          if (listStr) {
            char debugBuf[201];
            strncpy(debugBuf, listStr, 200);
            debugBuf[200] = '\0';
            HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: listStr preview: '%s'\n", debugBuf);
          }
        }
      } else {
        HW_DEBUG_PRINTF("[BLE] getDiscoveryResults: listStr=%p, macLen=%zu\n", 
                       listStr, strlen(macStr));
      }
    }
  }
  
  return count;
}

bool BLERelayManager::pauseServer() {
  if (_serverPaused) {
    return true;  // Already paused
  }
  
  BikeSensorServerPause();
  _serverPaused = true;
  return true;
}

bool BLERelayManager::resumeServer() {
  if (!_serverPaused) {
    return true;  // Already running
  }
  
  BikeSensorServerResume();
  _serverPaused = false;
  return true;
}

void BLERelayManager::stopForMaintenance() {
  if (_maintenanceStopped) {
    return;
  }
  
  HW_DEBUG_PRINTLN("[BLE] Stopping for maintenance...");
  
  _maintenanceStopped = true;
  
  // Stop discovery if active
  if (_discoveryActive) {
    stopDiscovery();
  }
  
  // Pause server
  pauseServer();
  
  // Mark all sensors as disabled (force remove) and apply immediately
  // Note: We call BikeSensorServerRemoveSensor directly here because
  // reconcile is blocked when _maintenanceStopped is true
  for (uint8_t i = 0; i < MAX_SENSORS; i++) {
    _desiredTargets[i].enabled = false;
    _desiredTargets[i].dirty = false;
    _desiredTargets[i].forceApplyNow = false;
    
    if (_appliedState[i].added) {
      BikeSensorServerRemoveSensor(_appliedState[i].type);
      _appliedState[i].added = false;
      _appliedState[i].name[0] = '\0';
    }
  }
  
  HW_DEBUG_PRINTLN("[BLE] Stopped for maintenance");
}

// Internal helpers

SensorType BLERelayManager::stringToSensorType(const char* type) {
  if (!type) {
    HW_DEBUG_PRINTLN("[BLE] stringToSensorType: null type");
    return static_cast<SensorType>(0);  // Invalid type
  }
  
  // Case-insensitive comparison without String allocation
  // Supported types: "HR", "CSC", "heart_rate", "speed_cadence"
  size_t len = strlen(type);
  
  HW_DEBUG_PRINTF("[BLE] stringToSensorType: input='%s' (len=%zu)\n", type, len);
  
  if (len == 2) {
    // Check for "HR"
    if ((type[0] == 'h' || type[0] == 'H') && (type[1] == 'r' || type[1] == 'R')) {
    HW_DEBUG_PRINTLN("[BLE] stringToSensorType: matched HEART_RATE");
    return SENSOR_TYPE_HEART_RATE;
    }
  } else if (len == 3) {
    // Check for "CSC"
    if ((type[0] == 'c' || type[0] == 'C') && (type[1] == 's' || type[1] == 'S') && (type[2] == 'c' || type[2] == 'C')) {
    HW_DEBUG_PRINTLN("[BLE] stringToSensorType: matched SPEED_CADENCE");
    return SENSOR_TYPE_SPEED_CADENCE;
    }
  } else if (len == 10 && strncasecmp(type, "heart_rate", 10) == 0) {
    HW_DEBUG_PRINTLN("[BLE] stringToSensorType: matched HEART_RATE");
    return SENSOR_TYPE_HEART_RATE;
  } else if (len == 13 && strncasecmp(type, "speed_cadence", 13) == 0) {
    HW_DEBUG_PRINTLN("[BLE] stringToSensorType: matched SPEED_CADENCE");
    return SENSOR_TYPE_SPEED_CADENCE;
  }
  
  HW_DEBUG_PRINTF("[BLE] stringToSensorType: no match for '%s'\n", type);
  return static_cast<SensorType>(0);  // Invalid type
}

const char* BLERelayManager::sensorTypeToString(SensorType type) {
  switch (type) {
    case SENSOR_TYPE_HEART_RATE: return "HR";
    case SENSOR_TYPE_SPEED_CADENCE: return "CSC";
    default: return "UNKNOWN";
  }
}

// ============================================================
// Single-Writer Reconcile Pattern Implementation
// ============================================================

int BLERelayManager::getDesiredIndex(SensorType type) const {
  // Map sensor type to array index (0=HR, 1=CSC)
  switch (type) {
    case SENSOR_TYPE_HEART_RATE: return 0;
    case SENSOR_TYPE_SPEED_CADENCE: return 1;
    default: return -1;
  }
}

void BLERelayManager::setDesiredTarget(SensorType type, const char* name, const char* mac, int8_t addrType, bool enabled, bool forceApplyNow) {
  int idx = getDesiredIndex(type);
  if (idx < 0) {
    HW_DEBUG_PRINTF("[BLE] setDesiredTarget: invalid type %d\n", static_cast<int>(type));
    return;
  }
  
  DesiredSensorTarget& target = _desiredTargets[idx];
  
  // Check if anything actually changed
  bool nameChanged = !target.nameEquals(name);
  bool macChanged = !target.macEquals(mac);
  bool addrTypeChanged = (target.addrType != addrType);
  bool enabledChanged = (target.enabled != enabled);
  bool forceChanged = forceApplyNow && !target.forceApplyNow;
  
  if (!nameChanged && !macChanged && !addrTypeChanged && !enabledChanged && !forceChanged && !target.dirty) {
    return; // No change
  }
  
  // Update desired state
  target.type = type;
  target.setName(name);
  target.setMac(mac);
  target.addrType = addrType;
  target.enabled = enabled;
  if (forceApplyNow) {
    target.forceApplyNow = true; // Only set, never clear here (cleared after apply)
  }
  target.dirty = true;
  
  HW_DEBUG_PRINTF("[BLE] Desired target set: type=%s name=\"%s\" mac=%s addrType=%d enabled=%d force=%d\n",
                  sensorTypeToString(type), name ? name : "", mac ? mac : "", addrType, enabled, target.forceApplyNow);
  
  scheduleReconcile();
}

void BLERelayManager::scheduleReconcile() {
  _reconcileScheduled = true;
}

void BLERelayManager::reconcileSensorTargets() {
  if (!initialized || _maintenanceStopped) {
    return;
  }
  
  // Throttle reconcile calls
  unsigned long now = millis();
  if (now - _lastReconcileMs < RECONCILE_INTERVAL_MS) {
    return;
  }
  _lastReconcileMs = now;
  _reconcileScheduled = false;
  
  // Process each sensor type
  for (uint8_t i = 0; i < MAX_SENSORS; i++) {
    DesiredSensorTarget& desired = _desiredTargets[i];
    AppliedSensorState& applied = _appliedState[i];
    
    // Skip if not dirty
    if (!desired.dirty) {
      continue;
    }
    
    SensorType type = desired.type;
    if (type == static_cast<SensorType>(0)) {
      // No type set - skip
      desired.dirty = false;
      continue;
    }
    
    bool isConnected = BikeSensorServerIsSensorConnected(type);
    bool macDiffer = (strcmp(desired.mac, applied.mac) != 0);
    
    // Case 1: Desired disabled
    if (!desired.enabled) {
      if (isConnected && !desired.forceApplyNow) {
        // Don't auto-disconnect unless explicitly requested
        HW_DEBUG_PRINTF("[BLE] Reconcile: skip remove (connected, no force): type=%s\n", 
                       sensorTypeToString(type));
        // Keep dirty so we can remove after natural disconnect
      } else if (applied.added) {
        // Either not connected, or forceApplyNow is true (explicit disconnect request)
        if (isConnected) {
          HW_DEBUG_PRINTF("[BLE] Reconcile: forced RemoveSensor (explicit disconnect): type=%s\n", 
                         sensorTypeToString(type));
        } else {
          HW_DEBUG_PRINTF("[BLE] Reconcile: apply RemoveSensor: type=%s\n", 
                         sensorTypeToString(type));
        }
        BikeSensorServerRemoveSensor(type);
        applied.added = false;
        applied.name[0] = '\0';
        applied.mac[0] = '\0';
        applied.addrType = -1;
        desired.dirty = false;
        desired.forceApplyNow = false;
      } else {
        // Not added, nothing to do
        desired.dirty = false;
        desired.forceApplyNow = false;
      }
      continue;
    }
    
    // Case 2: Desired enabled
    if (desired.enabled) {
      // Check if MAC is available (required for MAC-based mode)
      if (strlen(desired.mac) == 0) {
        HW_DEBUG_PRINTF("[BLE] Reconcile: skipping type=%s - no MAC address\n",
                       sensorTypeToString(type));
        desired.dirty = false;
        continue;
      }
      
      // Check if we need to apply a change (compare by MAC and addrType, not name)
      bool macDiffer = (strcmp(desired.mac, applied.mac) != 0);
      bool addrTypeDiffer = (desired.addrType != applied.addrType);
      bool needsApply = !applied.added || macDiffer || addrTypeDiffer;
      
      if (!needsApply) {
        // Already applied with correct MAC
        desired.dirty = false;
        desired.forceApplyNow = false;
        continue;
      }
      
      if (isConnected) {
        if (desired.forceApplyNow) {
          // Controlled reconnect: disconnect first, then apply
          HW_DEBUG_PRINTF("[BLE] Reconcile: controlled reconnect: type=%s pendingMAC=\"%s\" appliedMAC=\"%s\"\n",
                         sensorTypeToString(type), desired.mac, applied.mac);
          
          // Remove sensor (will disconnect)
          BikeSensorServerRemoveSensor(type);
          
          // Apply new target using MAC
          HW_DEBUG_PRINTF("[BLE] Reconcile: apply AddSensorByMac: type=%s mac=\"%s\" name=\"%s\" addrType=%d\n",
                         sensorTypeToString(type), desired.mac, desired.name, desired.addrType);
          BikeSensorServerAddSensorByMac(type, desired.name, desired.mac, desired.addrType);
          
          // Update applied state
          applied.added = true;
          applied.type = type;
          applied.setName(desired.name);
          applied.setMac(desired.mac);
          applied.addrType = desired.addrType;
          
          desired.dirty = false;
          desired.forceApplyNow = false;
        } else {
          // Apply-on-disconnect: defer until disconnected
          HW_DEBUG_PRINTF("[BLE] Reconcile: defer apply (connected): type=%s pendingMAC=\"%s\" appliedMAC=\"%s\"\n",
                         sensorTypeToString(type), desired.mac, applied.mac);
          // Keep dirty=true so we re-check after disconnect
        }
      } else {
        // Not connected - safe to apply immediately
        HW_DEBUG_PRINTF("[BLE] Reconcile: apply AddSensorByMac: type=%s mac=\"%s\" name=\"%s\" addrType=%d\n",
                       sensorTypeToString(type), desired.mac, desired.name, desired.addrType);
        BikeSensorServerAddSensorByMac(type, desired.name, desired.mac, desired.addrType);
        
        // Update applied state
        applied.added = true;
        applied.type = type;
        applied.setName(desired.name);
        applied.setMac(desired.mac);
        applied.addrType = desired.addrType;
        
        desired.dirty = false;
        desired.forceApplyNow = false;
      }
    }
  }
}

void BLERelayManager::updateTelemetry() {
#if HOMEWIND_ENABLE_WEBSOCKET
  if (!_telemetry) {
    return;
  }
  
  // Mark sensor snapshot as dirty
  _telemetry->markDirty(TelemetryFrameType::SENSORS_SNAPSHOT);
#endif
}

void BLERelayManager::onSensorConnected(SensorType type, const char* name) {
  // Try to get name from configured sensors if library name not available
  const char* displayName = name;
  if (!displayName || strlen(displayName) == 0) {
    for (uint8_t i = 0; i < _sensorCount; i++) {
      if (_configuredSensors[i].sensorType == type) {
        displayName = _configuredSensors[i].name;
        break;
      }
    }
  }
  if (!displayName || strlen(displayName) == 0) {
    displayName = "unknown";
  }
  
  HW_DEBUG_PRINTF("[BLE] Sensor connected: %s (type=%d)\n", displayName, type);
  _connectedCount++;
  
  // Update MAC address in config if available (only if changed to avoid NVS writes)
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (_configuredSensors[i].sensorType == type) {
      char macBuf[18];
      if (BikeSensorServerGetSensorMac(type, macBuf, sizeof(macBuf))) {
        // Only update and save if MAC actually changed (avoids unnecessary heap churn and NVS writes)
        if (strcmp(_configuredSensors[i].mac, macBuf) != 0) {
          _configuredSensors[i].setMac(macBuf);
          // Note: addrType should be updated from library if available, but for now keep existing
          // Save updated MAC to NVS (deterministic storage)
          saveConfigToNVS();
          HW_DEBUG_PRINTF("[BLE] MAC updated for sensor type %d: %s\n", type, macBuf);
        }
      }
      break;
    }
  }
  
  // Skip telemetry updates when server is paused to avoid UI interference
  if (!_serverPaused) {
    updateTelemetry();
    
#if HOMEWIND_ENABLE_WEBSOCKET
    if (_telemetry) {
      // Use SENSORS_SNAPSHOT instead of SENSOR_CONN_STATE (sends all sensors correctly)
      _telemetry->markDirty(TelemetryFrameType::SENSORS_SNAPSHOT);
    }
#endif
  }

#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    if (type == SENSOR_TYPE_HEART_RATE) {
      g_displayManager->updateHRState();
    } else if (type == SENSOR_TYPE_SPEED_CADENCE) {
      g_displayManager->updateCSCState();
    }
  }
#endif
}

void BLERelayManager::onSensorDisconnected(SensorType type, const char* name) {
  // Try to get name from configured sensors if library name not available
  const char* displayName = name;
  if (!displayName || strlen(displayName) == 0) {
    for (uint8_t i = 0; i < _sensorCount; i++) {
      if (_configuredSensors[i].sensorType == type) {
        displayName = _configuredSensors[i].name;
        break;
      }
    }
  }
  if (!displayName || strlen(displayName) == 0) {
    displayName = "unknown";
  }
  
  // Get and log disconnect reason for debugging
  int disconnectReason = BikeSensorServerGetLastDisconnectReason(type);
  const char* reasonStr = BikeSensorServerGetDisconnectReasonString(disconnectReason);
  HW_DEBUG_PRINTF("[BLE] Sensor disconnected: %s (type=%s, reason=0x%02X: %s)\n", 
                  displayName, sensorTypeToString(type), disconnectReason, reasonStr);
  if (_connectedCount > 0) _connectedCount--;
  
  // Update applied state to reflect disconnection
  int idx = getDesiredIndex(type);
  if (idx >= 0) {
    // Applied state remains (sensor still added), but it's now disconnected
    // Check if there's a pending desired change (apply-on-disconnect)
    if (_desiredTargets[idx].dirty) {
      HW_DEBUG_PRINTF("[BLE] Disconnect triggered: applying deferred change for type=%s\n",
                     sensorTypeToString(type));
      scheduleReconcile();
    }
  }
  
  // Skip telemetry updates when server is paused to avoid UI interference
  if (!_serverPaused) {
    updateTelemetry();
    
#if HOMEWIND_ENABLE_WEBSOCKET
    if (_telemetry) {
      // Use SENSORS_SNAPSHOT instead of SENSOR_CONN_STATE (sends all sensors correctly)
      _telemetry->markDirty(TelemetryFrameType::SENSORS_SNAPSHOT);
    }
#endif
  }
  
#if HW_ENABLE_FANS && HW_ENABLE_BLE
  // Forward HR disconnect to FanHRAdapter
  if (type == SENSOR_TYPE_HEART_RATE && g_fanHrAdapter) {
    g_fanHrAdapter->onHeartRateDisconnected(millis());
  }
#endif

#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    if (type == SENSOR_TYPE_HEART_RATE) {
      g_displayManager->updateHRState();
    } else if (type == SENSOR_TYPE_SPEED_CADENCE) {
      g_displayManager->updateCSCState();
    }
  }
#endif
}

void BLERelayManager::onHeartRateChanged(uint16_t hr) {
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::HEART_RATE);
  }
#endif
#if HW_ENABLE_FANS && HW_ENABLE_BLE
  // Forward HR sample to FanHRAdapter
  if (g_fanHrAdapter) {
    g_fanHrAdapter->onHeartRateSample(hr, millis());
  }
#endif
#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->setHeartRate(hr);
  }
#endif
}

void BLERelayManager::onDiscoveryComplete(uint8_t count) {
  HW_DEBUG_PRINTF("[BLE] Discovery complete: %u sensors found\n", count);
  _lastDiscoveryCount = count;
  
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    // Mark results dirty FIRST (while discovery is still active)
    // This ensures results are available when the frame is built
    _telemetry->markDirty(TelemetryFrameType::DISCOVERY_RESULTS);
    
    // Then mark discovery as inactive and update status
    // This ensures status frame shows inactive AFTER results are sent
    _discoveryActive = false;
    _telemetry->markDirty(TelemetryFrameType::DISCOVERY_STATUS);
  } else {
    _discoveryActive = false;
  }
#else
  _discoveryActive = false;
#endif
}

// Static callback implementations

void BLERelayManager::staticOnHRConnected() {
  if (_instance) {
    char nameBuf[64];
    const char* name = nullptr;
    if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, nameBuf, sizeof(nameBuf))) {
      name = nameBuf;
    }
    _instance->onSensorConnected(SENSOR_TYPE_HEART_RATE, name);
  }
}

void BLERelayManager::staticOnHRDisconnected() {
  if (_instance) {
    char nameBuf[64];
    const char* name = nullptr;
    if (BikeSensorServerGetSensorName(SENSOR_TYPE_HEART_RATE, nameBuf, sizeof(nameBuf))) {
      name = nameBuf;
    }
    _instance->onSensorDisconnected(SENSOR_TYPE_HEART_RATE, name);
  }
}

void BLERelayManager::staticOnCSCConnected() {
  if (_instance) {
    char nameBuf[64];
    const char* name = nullptr;
    if (BikeSensorServerGetSensorName(SENSOR_TYPE_SPEED_CADENCE, nameBuf, sizeof(nameBuf))) {
      name = nameBuf;
    }
    _instance->onSensorConnected(SENSOR_TYPE_SPEED_CADENCE, name);
  }
}

void BLERelayManager::staticOnCSCDisconnected() {
  if (_instance) {
    char nameBuf[64];
    const char* name = nullptr;
    if (BikeSensorServerGetSensorName(SENSOR_TYPE_SPEED_CADENCE, nameBuf, sizeof(nameBuf))) {
      name = nameBuf;
    }
    _instance->onSensorDisconnected(SENSOR_TYPE_SPEED_CADENCE, name);
  }
}

void BLERelayManager::staticOnHeartRateChanged(uint16_t hr) {
  if (_instance) {
    _instance->onHeartRateChanged(hr);
  }
}

void BLERelayManager::staticOnSpeedCadenceChanged(uint16_t speed, uint16_t cadence) {
  (void)speed;
#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->setCadence(cadence);
  }
#endif
}

void BLERelayManager::staticOnDiscoveryComplete(uint8_t count) {
  if (_instance) {
    _instance->onDiscoveryComplete(count);
  }
}

// =============================================================================
// Deterministic NVS Storage Functions
// =============================================================================


/**
 * @brief Load sensor configurations from NVS (deterministic struct-based storage)
 * Zero heap allocation during load.
 */
bool BLERelayManager::loadConfigFromNVS() {
  HW_DEBUG_PRINTLN("[BLE] [NVS] loadConfigFromNVS() called (deterministic storage)");
  
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) {  // Read-only mode
    HW_DEBUG_PRINTLN("[BLE] [NVS] Failed to open NVS namespace");
    return false;
  }
  
  // Try to load new blob format first
  NVSSensorData data;
  size_t len = prefs.getBytesLength(NVS_KEY_DATA);
  
  if (len == sizeof(NVSSensorData)) {
    // New format exists
    prefs.getBytes(NVS_KEY_DATA, &data, sizeof(data));
    prefs.end();
    
    // Verify CRC
    uint16_t storedCrc = data.crc16;
    data.crc16 = 0;
    uint16_t calcedCrc = Crc16::calcCRC16((uint8_t*)&data, sizeof(data) - sizeof(data.crc16));
    
    if (storedCrc != calcedCrc) {
      HW_ERROR_PRINTF("[BLE] [NVS] CRC mismatch: stored=%04X, calculated=%04X\n", 
                      storedCrc, calcedCrc);
      return false;
    }
    
    // Check version
    if (data.version != NVS_DATA_VERSION) {
      HW_DEBUG_PRINTF("[BLE] [NVS] Version mismatch: stored=%u, expected=%u\n",
                      data.version, NVS_DATA_VERSION);
      return false;
    }
    
    // Validate sensor count (clamp if old config had more sensors, e.g. PWR removed)
    if (data.sensorCount > MAX_SENSORS) {
      HW_DEBUG_PRINTF("[BLE] [NVS] Clamping sensorCount from %u to %u (PWR support removed)\n", 
                      data.sensorCount, MAX_SENSORS);
      data.sensorCount = MAX_SENSORS;
    }
    
    // Copy data to runtime structures
    _sensorCount = data.sensorCount;
    
    for (uint8_t i = 0; i < _sensorCount; i++) {
      const NVSSensorEntry& entry = data.sensors[i];
      SensorConfig& cfg = _configuredSensors[i];
      
      cfg.setName(entry.name);
      cfg.setType(entry.type);
      cfg.setMac(entry.mac);
      cfg.addrType = entry.addrType;
      cfg.sensorType = stringToSensorType(cfg.type);
      
      HW_DEBUG_PRINTF("[BLE] [NVS] → Loaded sensor %u: %s (%s) addrType=%d\n",
                     i, cfg.name, cfg.type, cfg.addrType);
    }
    
    HW_DEBUG_PRINTF("[BLE] [NVS] ✓ Loaded %u sensor(s) from blob\n", _sensorCount);
    return true;
  }
  
  prefs.end();
  
  HW_DEBUG_PRINTLN("[BLE] [NVS] No config found in NVS");
  return false;
}

/**
 * @brief Save sensor configurations to NVS (deterministic struct-based storage)
 * Zero heap allocation during save.
 */
bool BLERelayManager::saveConfigToNVS() {
  HW_DEBUG_PRINTF("[BLE] [NVS] saveConfigToNVS() called with %u sensors (deterministic storage)\n", _sensorCount);
  
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) {  // Read-write mode
    HW_ERROR_PRINTLN("[BLE] [NVS] Failed to open NVS namespace for writing");
    return false;
  }
  
  // Prepare data structure
  NVSSensorData data;
  memset(&data, 0, sizeof(data));
  data.version = NVS_DATA_VERSION;
  data.sensorCount = _sensorCount;
  
  for (uint8_t i = 0; i < _sensorCount; i++) {
    const SensorConfig& cfg = _configuredSensors[i];
    NVSSensorEntry& entry = data.sensors[i];
    
    strncpy(entry.name, cfg.name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    
    strncpy(entry.type, cfg.type, sizeof(entry.type) - 1);
    entry.type[sizeof(entry.type) - 1] = '\0';
    
    strncpy(entry.mac, cfg.mac, sizeof(entry.mac) - 1);
    entry.mac[sizeof(entry.mac) - 1] = '\0';
    
    entry.addrType = cfg.addrType;
    
    HW_DEBUG_PRINTF("[BLE] [NVS] → Sensor %u: %s (%s) addrType=%d\n", i, cfg.name, cfg.type, cfg.addrType);
  }
  
  // Calculate CRC
  data.crc16 = 0;
  data.crc16 = Crc16::calcCRC16((uint8_t*)&data, sizeof(data) - sizeof(data.crc16));
  
  // Write blob
  size_t written = prefs.putBytes(NVS_KEY_DATA, &data, sizeof(data));
  prefs.end();
  
  if (written != sizeof(data)) {
    HW_ERROR_PRINTF("[BLE] [NVS] ✗ Failed to write NVS: wrote %u, expected %u\n",
                    written, sizeof(data));
    return false;
  }
  
  HW_DEBUG_PRINTF("[BLE] [NVS] ✓ Saved %u sensor(s) to NVS blob, crc=%04X\n", _sensorCount, data.crc16);
  return true;
}

#else // HW_ENABLE_BLE == 0

// Stub implementations when BLE is disabled
// This ensures the file always compiles and generates symbols for the linker

#include "BLERelayManager.h"
#include "../core/DebugLog.h"

BLERelayManager* BLERelayManager::_instance = nullptr;

BLERelayManager::BLERelayManager()
  : initialized(false)
  , _maintenanceStopped(false)
  , _serverPaused(false)
  , _connectedCount(0)
  , _lastDiscoveryCount(0)
#if HOMEWIND_ENABLE_WEBSOCKET
  , _telemetry(nullptr)
#endif
  , _sensorCount(0)
  , _discoveryActive(false)
  , _reconcileScheduled(false)
  , _lastReconcileMs(0)
{
  _discoveryType[0] = '\0';
}

BLERelayManager::~BLERelayManager() {
}

#if HOMEWIND_ENABLE_WEBSOCKET
bool BLERelayManager::begin(WebSocketTelemetry* telemetry) {
#else
bool BLERelayManager::begin() {
#endif
  HW_DEBUG_PRINTLN("[BLE] BLE disabled by compile-time flag");
  return false;
}

void BLERelayManager::loop() {
}

bool BLERelayManager::loadConfiguredSensors() { return false; }
bool BLERelayManager::getSensorConnectionState(const char*, String&, String&, bool&, int&) { return false; }
uint8_t BLERelayManager::getConfiguredSensors(SensorInfo*, uint8_t) { return 0; }
bool BLERelayManager::setConfiguredSensor(const char*, const char*, const char*, int8_t, bool) { return false; }
bool BLERelayManager::deleteConfiguredSensor(const char*) { return false; }
bool BLERelayManager::connectSensor(const char*) { return false; }
bool BLERelayManager::disconnectSensor(const char*) { return false; }
bool BLERelayManager::reloadSensors() { return false; }
bool BLERelayManager::startDiscovery(const char*) { return false; }
bool BLERelayManager::stopDiscovery() { return false; }
uint8_t BLERelayManager::getDiscoveryResults(DiscoveredSensor*, uint8_t) { return 0; }
bool BLERelayManager::pauseServer() { return false; }
bool BLERelayManager::resumeServer() { return false; }
void BLERelayManager::stopForMaintenance() {}
bool BLERelayManager::saveConfigToNVS() { return false; }
bool BLERelayManager::loadConfigFromNVS() { return false; }

#endif // HW_ENABLE_BLE

