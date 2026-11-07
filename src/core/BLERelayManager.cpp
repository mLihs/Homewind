/**
 * @file BLERelayManager.cpp
 * @brief BLE Relay Manager implementation (BikeSensor 3.0)
 */

#include "../app/Config.h"

#if HW_ENABLE_BLE

#include "BLERelayManager.h"
#include "../core/DebugLog.h"
#include "../core/Crc16.h"
#include "MaintenanceMode.h"

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

#include <strings.h>

// Static instance pointer for callbacks
BLERelayManager* BLERelayManager::_instance = nullptr;

namespace {

constexpr uint8_t kAllTypes = hw::kBleTypeHR | hw::kBleTypeCSC;

BikeSensor::SensorType toSensorType(uint8_t typeBit) {
  return typeBit == hw::kBleTypeHR ? BikeSensor::SensorType::HEART_RATE
                                   : BikeSensor::SensorType::SPEED_CADENCE;
}

void copyStr(char* dst, const char* src, size_t len) {
  if (!src) { dst[0] = '\0'; return; }
  strncpy(dst, src, len - 1);
  dst[len - 1] = '\0';
}

void lowerMac(char* dst, const char* src) {
  size_t i = 0;
  for (; src && src[i] != '\0' && i < 17; ++i) {
    const char c = src[i];
    dst[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  }
  dst[i] = '\0';
}

void formatMac(char* out, const uint8_t* mac) {
  snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Reconcile's view of the library.
class LibraryPort : public hw::BleLibraryPort {
public:
  bool connected(uint8_t typeBit) override { return BikeSensor::connected(toSensorType(typeBit)); }
  bool add(uint8_t typeBit, const char* mac, int8_t addrType, const char* name) override {
    const bool nameFallback = addrType != 0 && name && name[0] != '\0';
    const bool ok = BikeSensor::addByMac(toSensorType(typeBit), mac, addrType, name, nameFallback);
    HW_DEBUG_PRINTF("[BLE] addByMac type=%u mac=%s addrType=%d -> %s\n",
                    typeBit, mac, addrType, ok ? "ok" : "rejected");
    return ok;
  }
  void remove(uint8_t typeBit) override {
    BikeSensor::remove(toSensorType(typeBit));
    HW_DEBUG_PRINTF("[BLE] remove type=%u\n", typeBit);
  }
};

}  // namespace

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
  , _bleMux(portMUX_INITIALIZER_UNLOCKED)
  , _pauseDesired(-1)
  , _discoveryActive(false)
  , _discoveryError(0)
  , _discoveryRequestedMask(0)
  , _discoveredMask(0)
  , _snapshotCount(0)
  , _discoveredCount(0)
  , _discoveredGeneration(0)
  , _reconcileScheduled(false)
  , _lastReconcileMs(0)
  , _discoveryStopIssued(false)
  , _maintDiscoveryDone(false)
  , _lastSnapshotMs(0)
  , _eventCount(0)
  , _eventsDropped(false)
  , _doneCount(0)
  , _hrBpm(0)
  , _hrValid(false)
  , _hrNew(false)
  , _cadenceRpm(0)
  , _cadenceNew(false)
{
  _instance = this;
  memset(_configuredSensors, 0, sizeof(_configuredSensors));
  memset(_discovered, 0, sizeof(_discovered));
}

BLERelayManager::~BLERelayManager() {
  if (_instance == this) {
    _instance = nullptr;
  }
}

#if HOMEWIND_ENABLE_WEBSOCKET
bool BLERelayManager::begin(WebSocketTelemetry* telemetry) {
#else
bool BLERelayManager::begin() {
#endif
  if (initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[BLE] Initializing BLE Relay Manager (BikeSensor 3.0)");
  
#if HOMEWIND_ENABLE_WEBSOCKET
  _telemetry = telemetry;
#endif
  
  if (!loadConfigFromNVS()) {
    HW_DEBUG_PRINTLN("[BLE] No sensor configuration found, starting fresh");
    _sensorCount = 0;
  }

  BikeSensor::setEventCallback(staticOnEvent);
  BikeSensor::setHeartRateCallback(staticOnHeartRate);
  BikeSensor::setCadenceCallback(staticOnCadence);

  if (!BikeSensor::begin("homewind", kAllTypes, BikeSensor::Mode::MAC)) {
    HW_ERROR_PRINTLN("[BLE] BikeSensor::begin() failed");
    return false;
  }

  initialized = true;

  // HR before CSC: a MAC saved for both types by 2.x stays with HR.
  setTargetsFromConfig();
  runReconcile(false);
  refreshSnapshot();
  HW_DEBUG_PRINTLN("[BLE] BLE Relay Manager initialized");
  
  markSensorsDirty();

#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->updateHRState();
    g_displayManager->updateCSCState();
  }
#endif
  
  return true;
}

void BLERelayManager::loop() {
  if (!initialized) {
    return;
  }

  BikeSensor::update();

  if (_maintenanceStopped) {
    _eventCount = 0;
    _doneCount = 0;
    return;
  }

  processEvents();

  if (_hrNew) {
    _hrNew = false;
#if HOMEWIND_ENABLE_WEBSOCKET
    if (_telemetry) {
      _telemetry->markDirty(TelemetryFrameType::HEART_RATE);
    }
#endif
#if HW_ENABLE_FANS
    if (g_fanHrAdapter) {
      g_fanHrAdapter->onHeartRateSample(_hrBpm, millis());
    }
#endif
#if HW_ENABLE_DISPLAY
    if (g_displayManager) {
      g_displayManager->setHeartRate(_hrBpm);
    }
#endif
  }
  if (_cadenceNew) {
    _cadenceNew = false;
#if HW_ENABLE_DISPLAY
    if (g_displayManager) {
      g_displayManager->setCadence(_cadenceRpm);
    }
#endif
  }

  // From maintenance REQUESTED on, queues and reconcile are blocked.
  if (MaintenanceMode::admissionClosed()) {
    return;
  }

  hw::BleCommand cmd;
  for (;;) {
    bool have;
    portENTER_CRITICAL(&_bleMux);
    have = _commands.pop(cmd);
    portEXIT_CRITICAL(&_bleMux);
    if (!have) break;
    applyCommand(cmd);
  }

  processDiscoveryOps();

  int8_t pause;
  portENTER_CRITICAL(&_bleMux);
  pause = _pauseDesired;
  _pauseDesired = -1;
  portEXIT_CRITICAL(&_bleMux);
  if (pause == 1 && !_serverPaused) {
    BikeSensor::pause();
    _serverPaused = true;
  } else if (pause == 0 && _serverPaused) {
    BikeSensor::resume();
    _serverPaused = false;
  }

  if (_reconcileScheduled) {
    runReconcile(true);
  }

  // Battery and link state for the snapshot.
  const unsigned long now = millis();
  if (now - _lastSnapshotMs >= 1000) {
    refreshSnapshot();
    if (!_serverPaused) {
      markSensorsDirty();
    }
  }
}

// =============================================================================
// Any-task readers
// =============================================================================

uint8_t BLERelayManager::getConfiguredSensors(HwSensorInfo* sensors, uint8_t maxSensors) {
  if (!sensors || maxSensors == 0) {
    return 0;
  }
  portENTER_CRITICAL(&_bleMux);
  uint8_t count = _snapshotCount < maxSensors ? _snapshotCount : maxSensors;
  for (uint8_t i = 0; i < count; i++) {
    sensors[i] = _snapshot[i];
  }
  portEXIT_CRITICAL(&_bleMux);
  return count;
}

bool BLERelayManager::isDiscoveryActive() {
  portENTER_CRITICAL(&_bleMux);
  const bool active = _discoveryActive;
  portEXIT_CRITICAL(&_bleMux);
  return active;
}

uint8_t BLERelayManager::discoveryError() {
  portENTER_CRITICAL(&_bleMux);
  const uint8_t err = _discoveryError;
  portEXIT_CRITICAL(&_bleMux);
  return err;
}

uint8_t BLERelayManager::discoverySnapshot(uint32_t& generation) {
  portENTER_CRITICAL(&_bleMux);
  const uint8_t count = _discoveredCount;
  generation = _discoveredGeneration;
  portEXIT_CRITICAL(&_bleMux);
  return count;
}

bool BLERelayManager::discoveredSensor(uint8_t index, HwDiscoveredSensor& out) {
  bool ok = false;
  portENTER_CRITICAL(&_bleMux);
  if (index < _discoveredCount) {
    out = _discovered[index];
    ok = true;
  }
  portEXIT_CRITICAL(&_bleMux);
  return ok;
}

// =============================================================================
// Any-task commands
// =============================================================================

BleEnqueueResult BLERelayManager::pushCommand(const hw::BleCommand& cmd) {
  if (MaintenanceMode::admissionClosed()) {
    return BleEnqueueResult::CLOSED;
  }
  portENTER_CRITICAL(&_bleMux);
  const bool ok = _commands.push(cmd);
  portEXIT_CRITICAL(&_bleMux);
  return ok ? BleEnqueueResult::ACCEPTED : BleEnqueueResult::FULL;
}

bool BLERelayManager::configuredNameExistsLocked(const char* name) const {
  for (uint8_t i = 0; i < _snapshotCount; i++) {
    if (strcmp(_snapshot[i].name, name) == 0) return true;
  }
  return false;
}

BleEnqueueResult BLERelayManager::enqueueByName(hw::BleCmdKind kind, const char* name) {
  if (!name || name[0] == '\0') {
    return BleEnqueueResult::NOT_FOUND;
  }
  portENTER_CRITICAL(&_bleMux);
  const bool exists = configuredNameExistsLocked(name);
  portEXIT_CRITICAL(&_bleMux);
  if (!exists) {
    return BleEnqueueResult::NOT_FOUND;
  }
  hw::BleCommand cmd;
  cmd.kind = kind;
  copyStr(cmd.name, name, sizeof(cmd.name));
  return pushCommand(cmd);
}

BleEnqueueResult BLERelayManager::enqueueSetFromDiscovery(uint32_t generation, uint8_t index, bool force) {
  portENTER_CRITICAL(&_bleMux);
  const bool valid = generation == _discoveredGeneration && index < _discoveredCount;
  portEXIT_CRITICAL(&_bleMux);
  if (!valid) {
    return BleEnqueueResult::STALE;
  }
  hw::BleCommand cmd;
  cmd.kind = hw::BleCmdKind::SET_FROM_DISCOVERY;
  cmd.set.generation = generation;
  cmd.set.index = index;
  cmd.set.force = force;
  return pushCommand(cmd);
}

BleEnqueueResult BLERelayManager::enqueueDelete(const char* name) {
  return enqueueByName(hw::BleCmdKind::DELETE, name);
}

BleEnqueueResult BLERelayManager::enqueueConnect(const char* name) {
  return enqueueByName(hw::BleCmdKind::CONNECT, name);
}

BleEnqueueResult BLERelayManager::enqueueDisconnect(const char* name) {
  return enqueueByName(hw::BleCmdKind::DISCONNECT, name);
}

BleEnqueueResult BLERelayManager::enqueueReload() {
  hw::BleCommand cmd;
  cmd.kind = hw::BleCmdKind::RELOAD;
  return pushCommand(cmd);
}

BleEnqueueResult BLERelayManager::requestDiscoveryStart(uint8_t typeMask) {
  if (MaintenanceMode::admissionClosed()) {
    return BleEnqueueResult::CLOSED;
  }
  portENTER_CRITICAL(&_bleMux);
  _discoveryOps.requestStart(typeMask);
  portEXIT_CRITICAL(&_bleMux);
  return BleEnqueueResult::ACCEPTED;
}

BleEnqueueResult BLERelayManager::requestDiscoveryStop() {
  if (MaintenanceMode::admissionClosed()) {
    return BleEnqueueResult::CLOSED;
  }
  portENTER_CRITICAL(&_bleMux);
  _discoveryOps.requestStop(_discoveryActive);
  portEXIT_CRITICAL(&_bleMux);
  return BleEnqueueResult::ACCEPTED;
}

BleEnqueueResult BLERelayManager::requestServerPaused(bool paused) {
  if (MaintenanceMode::admissionClosed()) {
    return BleEnqueueResult::CLOSED;
  }
  portENTER_CRITICAL(&_bleMux);
  _pauseDesired = paused ? 1 : 0;
  portEXIT_CRITICAL(&_bleMux);
  return BleEnqueueResult::ACCEPTED;
}

// =============================================================================
// Loop task
// =============================================================================

int BLERelayManager::findConfigByName(const char* name) const {
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (strcmp(_configuredSensors[i].name, name) == 0) return i;
  }
  return -1;
}

int BLERelayManager::findConfigByType(uint8_t typeBit) const {
  for (uint8_t i = 0; i < _sensorCount; i++) {
    if (_configuredSensors[i].typeBit == typeBit) return i;
  }
  return -1;
}

void BLERelayManager::setTargetsFromConfig() {
  for (uint8_t slot = 0; slot < hw::kBleSlots; slot++) {
    const uint8_t type = hw::bleTypeOfSlot(slot);
    const int i = findConfigByType(type);
    if (i < 0) continue;
    const SensorConfig& cfg = _configuredSensors[i];
    if (cfg.name[0] == '\0' || cfg.mac[0] == '\0') {
      HW_DEBUG_PRINTF("[BLE] Skipping sensor %s (%s) - no MAC address\n", cfg.name, cfg.type);
      continue;
    }
    char mac[18];
    lowerMac(mac, cfg.mac);
    _reconciler.setTarget(type, cfg.name, mac, cfg.addrType, true, false);
    HW_DEBUG_PRINTF("[BLE] Target: %s (%s) MAC=%s addrType=%d\n", cfg.name, cfg.type, mac, cfg.addrType);
  }
  _reconcileScheduled = true;
}

void BLERelayManager::applySetFromDiscovery(uint32_t generation, uint8_t index, bool force) {
  HwDiscoveredSensor rec = {};
  uint8_t requestedMask;
  bool valid;
  portENTER_CRITICAL(&_bleMux);
  valid = generation == _discoveredGeneration && index < _discoveredCount;
  if (valid) rec = _discovered[index];
  requestedMask = _discoveredMask;
  portEXIT_CRITICAL(&_bleMux);
  if (!valid) {
    HW_DEBUG_PRINTF("[BLE] setSensor: stale generation=%lu index=%u\n",
                    static_cast<unsigned long>(generation), index);
    return;
  }

  // A sensor advertising both services: the discovery's own type wins, else HR.
  uint8_t type = rec.typeMask & kAllTypes;
  if (type == kAllTypes) {
    type = (requestedMask == hw::kBleTypeCSC) ? hw::kBleTypeCSC : hw::kBleTypeHR;
  }
  if (type != hw::kBleTypeHR && type != hw::kBleTypeCSC) {
    HW_DEBUG_PRINTF("[BLE] setSensor: index %u has no HR/CSC type\n", index);
    return;
  }

  char mac[18];
  formatMac(mac, rec.mac);
  const char* name = rec.name[0] != '\0' ? rec.name : mac;
  const int8_t addrType = static_cast<int8_t>(rec.addrType);

  int i = findConfigByType(type);
  if (i < 0) {
    if (_sensorCount >= MAX_SENSORS) return;
    i = _sensorCount++;
  }
  SensorConfig& cfg = _configuredSensors[i];
  copyStr(cfg.name, name, sizeof(cfg.name));
  copyStr(cfg.type, typeBitToString(type), sizeof(cfg.type));
  copyStr(cfg.mac, mac, sizeof(cfg.mac));
  cfg.addrType = addrType;
  cfg.typeBit = type;

  HW_DEBUG_PRINTF("[BLE] setSensor: name=\"%s\" type=%s mac=%s addrType=%d force=%d\n",
                  cfg.name, cfg.type, mac, addrType, force);

  if (!saveConfigToNVS()) {
    HW_ERROR_PRINTLN("[BLE] Failed to save sensor configuration");
  }
  _reconciler.setTarget(type, cfg.name, mac, addrType, true, force);
  _reconcileScheduled = true;
}

void BLERelayManager::applyCommand(const hw::BleCommand& cmd) {
  switch (cmd.kind) {
    case hw::BleCmdKind::SET_FROM_DISCOVERY:
      applySetFromDiscovery(cmd.set.generation, cmd.set.index, cmd.set.force);
      break;

    case hw::BleCmdKind::DELETE: {
      const int i = findConfigByName(cmd.name);
      if (i < 0) break;
      HW_DEBUG_PRINTF("[BLE] deleteSensor: name=\"%s\"\n", cmd.name);
      _reconciler.setTarget(_configuredSensors[i].typeBit, "", "", -1, false, true);
      for (uint8_t j = i; j + 1 < _sensorCount; j++) {
        _configuredSensors[j] = _configuredSensors[j + 1];
      }
      _sensorCount--;
      saveConfigToNVS();
      _reconcileScheduled = true;
      break;
    }

    case hw::BleCmdKind::CONNECT: {
      const int i = findConfigByName(cmd.name);
      if (i < 0) break;
      const SensorConfig& cfg = _configuredSensors[i];
      if (cfg.mac[0] == '\0') break;
      char mac[18];
      lowerMac(mac, cfg.mac);
      _reconciler.setTarget(cfg.typeBit, cfg.name, mac, cfg.addrType, true, false);
      _reconcileScheduled = true;
      break;
    }

    case hw::BleCmdKind::DISCONNECT: {
      const int i = findConfigByName(cmd.name);
      if (i < 0) break;
      const SensorConfig& cfg = _configuredSensors[i];
      _reconciler.setTarget(cfg.typeBit, cfg.name, cfg.mac, cfg.addrType, false, true);
      _reconcileScheduled = true;
      break;
    }

    case hw::BleCmdKind::RELOAD:
      HW_DEBUG_PRINTLN("[BLE] reloadSensors: clearing and reloading configuration");
      for (uint8_t slot = 0; slot < hw::kBleSlots; slot++) {
        _reconciler.setTarget(hw::bleTypeOfSlot(slot), "", "", -1, false, true);
      }
      runReconcile(false);
      _sensorCount = 0;
      if (!loadConfigFromNVS()) {
        _sensorCount = 0;
      }
      setTargetsFromConfig();
      break;
  }
  refreshSnapshot();
  markSensorsDirty();
}

void BLERelayManager::processDiscoveryOps() {
  hw::BleDiscOp op;
  uint8_t mask;
  bool have;
  portENTER_CRITICAL(&_bleMux);
  have = _discoveryOps.peek(op);
  mask = _discoveryOps.mask();
  portEXIT_CRITICAL(&_bleMux);
  if (!have) return;

  if (op == hw::BleDiscOp::STOP) {
    // Stays at the head until DISCOVERY_DONE (processEvents pops it).
    if (!_discoveryStopIssued) {
      BikeSensor::stopDiscovery();
      _discoveryStopIssued = true;
      HW_DEBUG_PRINTLN("[BLE] Discovery stop requested");
    }
    return;
  }

  const bool started = BikeSensor::startDiscovery(mask, 15000);
  portENTER_CRITICAL(&_bleMux);
  _discoveryOps.pop();
  if (started) {
    _discoveryActive = true;
    _discoveryError = 0;
    _discoveryRequestedMask = mask;
  } else {
    _discoveryError = 1;
  }
  portEXIT_CRITICAL(&_bleMux);
  HW_DEBUG_PRINTF("[BLE] startDiscovery(mask=0x%02X) -> %s\n", mask, started ? "ok" : "busy");
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::DISCOVERY_STATUS);
  }
#endif
}

void BLERelayManager::processEvents() {
  if (_eventsDropped) {
    _eventsDropped = false;
    HW_ERROR_PRINTLN("[BLE] Event FIFO overflow, state resynced from the library");
    refreshSnapshot();
    if (!BikeSensor::connected(BikeSensor::SensorType::HEART_RATE)) {
      _hrValid = false;
      _hrNew = false;
#if HW_ENABLE_FANS
      if (g_fanHrAdapter) g_fanHrAdapter->onHeartRateDisconnected(millis());
#endif
    }
  }

  for (uint8_t n = 0; n < _eventCount; n++) {
    const EventRecord& e = _events[n];
    const BikeSensor::EventType type = static_cast<BikeSensor::EventType>(e.type);
    const uint8_t typeBit = e.sensorType;
    [[maybe_unused]] const char* typeStr = typeBitToString(typeBit);

    switch (type) {
      case BikeSensor::EventType::SENSOR_CONNECTED:
        HW_DEBUG_PRINTF("[BLE] Sensor connected: %s\n", typeStr);
        adoptSensorAddress(typeBit);
        refreshSnapshot();
        if (!_serverPaused) markSensorsDirty();
        notifyDisplay(typeBit);
        break;

      case BikeSensor::EventType::SENSOR_DISCONNECTED:
        HW_DEBUG_PRINTF("[BLE] Sensor disconnected: %s (reason=0x%02X: %s)\n", typeStr,
                        e.reason, BikeSensor::disconnectReasonString(e.reason));
        if (typeBit == hw::kBleTypeHR) {
          _hrValid = false;
          _hrNew = false;   // a sample from the same update() is stale now
#if HW_ENABLE_FANS
          if (g_fanHrAdapter) g_fanHrAdapter->onHeartRateDisconnected(millis());
#endif
        }
        if (_reconciler.pending()) {
          _reconcileScheduled = true;   // apply-on-disconnect
        }
        refreshSnapshot();
        if (!_serverPaused) markSensorsDirty();
        notifyDisplay(typeBit);
        break;

      case BikeSensor::EventType::SENSOR_FAILED:
        // reason is 0 (hard fail) or a NimBLE host rc, not a disconnect reason.
        HW_DEBUG_PRINTF("[BLE] Sensor connect failed: %s (rc=%d)\n", typeStr, e.reason);
        break;

      case BikeSensor::EventType::SENSOR_FOUND:
        HW_DEBUG_PRINTF("[BLE] Sensor found: %s\n", typeStr);
        break;

      case BikeSensor::EventType::HOST_RESET:
        HW_ERROR_PRINTF("[BLE] NimBLE host reset (reason=%d)\n", e.reason);
        break;

      default:
        break;
    }
  }
  _eventCount = 0;

  for (uint8_t n = 0; n < _doneCount; n++) {
    const DoneRecord& d = _done[n];
    uint8_t count = BikeSensor::discoveredCount();
    if (count > BLE_DISCOVERY_MAX) count = BLE_DISCOVERY_MAX;
    // Build the new list outside the mux, then swap list, count, generation
    // and mask in one section so readers never see a mix of two discoveries.
    HwDiscoveredSensor next[BLE_DISCOVERY_MAX];
    uint8_t nextCount = 0;
    for (uint8_t i = 0; i < count; i++) {
      BikeSensor::DiscoveredSensor src;
      if (!BikeSensor::discovered(i, src)) break;
      HwDiscoveredSensor& rec = next[nextCount++];
      memcpy(rec.name, src.name, sizeof(rec.name));
      rec.name[sizeof(rec.name) - 1] = '\0';
      memcpy(rec.mac, src.mac, sizeof(rec.mac));
      rec.addrType = src.addrType;
      rec.typeMask = src.typeMask;
    }
    hw::BleDiscOp op;
    portENTER_CRITICAL(&_bleMux);
    memcpy(_discovered, next, nextCount * sizeof(HwDiscoveredSensor));
    _discoveredCount = nextCount;
    _discoveredGeneration = d.generation;
    _discoveredMask = d.mask;
    _discoveryActive = false;
    if (_discoveryOps.peek(op) && op == hw::BleDiscOp::STOP) {
      _discoveryOps.pop();
    }
    _lastDiscoveryCount = _discoveredCount;
    portEXIT_CRITICAL(&_bleMux);
    _discoveryStopIssued = false;
    _maintDiscoveryDone = true;
    HW_DEBUG_PRINTF("[BLE] Discovery done: %u sensors (generation=%lu, reason=%u)\n",
                    _lastDiscoveryCount, static_cast<unsigned long>(d.generation), d.reason);
    (void)d.count;
#if HOMEWIND_ENABLE_WEBSOCKET
    if (_telemetry) {
      // Results before status: the UI sees inactive after the list arrived.
      _telemetry->markDirty(TelemetryFrameType::DISCOVERY_RESULTS);
      _telemetry->markDirty(TelemetryFrameType::DISCOVERY_STATUS);
    }
#endif
  }
  _doneCount = 0;
}

// A name-fallback slot may connect under a new random address: keep it in the
// config and NVS so the next boot uses it, without a re-add.
void BLERelayManager::adoptSensorAddress(uint8_t typeBit) {
  if (MaintenanceMode::admissionClosed()) return;
  const int i = findConfigByType(typeBit);
  BikeSensor::SensorInfo info;
  if (i < 0 || !BikeSensor::info(toSensorType(typeBit), info)) return;
  SensorConfig& cfg = _configuredSensors[i];
  char mac[18];
  formatMac(mac, info.mac);
  if (strcasecmp(cfg.mac, mac) == 0 && cfg.addrType == info.addrType) return;
  if (!_reconciler.adoptApplied(typeBit, mac, info.addrType)) {
    HW_DEBUG_PRINTF("[BLE] %s address %s/%d not adopted (not applied or change pending)\n", cfg.type, mac,
                    info.addrType);
    return;
  }
  HW_DEBUG_PRINTF("[BLE] %s address changed: %s/%d -> %s/%d\n", cfg.type, cfg.mac,
                  cfg.addrType, mac, info.addrType);
  copyStr(cfg.mac, mac, sizeof(cfg.mac));
  cfg.addrType = info.addrType;
  if (!saveConfigToNVS()) {
    HW_ERROR_PRINTLN("[BLE] Failed to save sensor configuration");
  }
  _reconcileScheduled = true;   // the other slot's duplicate may apply now
}

void BLERelayManager::runReconcile(bool throttled) {
  const unsigned long now = millis();
  if (throttled && now - _lastReconcileMs < RECONCILE_INTERVAL_MS) {
    return;
  }
  _lastReconcileMs = now;
  _reconcileScheduled = false;
  LibraryPort port;
  _reconciler.run(port);
  refreshSnapshot();
  if (!_serverPaused) markSensorsDirty();
}

void BLERelayManager::refreshSnapshot() {
  HwSensorInfo next[MAX_SENSORS];
  uint8_t count = 0;
  uint8_t connected = 0;
  for (uint8_t i = 0; i < _sensorCount && count < MAX_SENSORS; i++) {
    const SensorConfig& cfg = _configuredSensors[i];
    if (cfg.typeBit == 0) continue;
    HwSensorInfo& s = next[count++];
    copyStr(s.name, cfg.name, sizeof(s.name));
    copyStr(s.type, cfg.type, sizeof(s.type));
    copyStr(s.mac, cfg.mac, sizeof(s.mac));
    const BikeSensor::SensorType st = toSensorType(cfg.typeBit);
    s.connected = BikeSensor::connected(st);
    BikeSensor::SensorInfo info;
    s.battery = (BikeSensor::info(st, info) && info.hasBattery) ? info.battery : -1;
    s.duplicate = _reconciler.duplicate(cfg.typeBit);
    if (s.connected) connected++;
  }
  portENTER_CRITICAL(&_bleMux);
  for (uint8_t i = 0; i < count; i++) {
    _snapshot[i] = next[i];
  }
  _snapshotCount = count;
  portEXIT_CRITICAL(&_bleMux);
  _connectedCount = connected;
  _lastSnapshotMs = millis();
}

void BLERelayManager::markSensorsDirty() {
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::SENSORS_SNAPSHOT);
  }
#endif
}

void BLERelayManager::notifyDisplay(uint8_t typeBit) {
#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    if (typeBit == hw::kBleTypeHR) {
      g_displayManager->updateHRState();
    } else if (typeBit == hw::kBleTypeCSC) {
      g_displayManager->updateCSCState();
    }
  }
#else
  (void)typeBit;
#endif
}

// =============================================================================
// Maintenance (loop task)
// =============================================================================

bool BLERelayManager::quiesceForMaintenance() {
  bool wasOpen;
  portENTER_CRITICAL(&_bleMux);
  wasOpen = _discoveryActive;
  _commands.clear();
  _discoveryOps.clear();
  _pauseDesired = -1;
  portEXIT_CRITICAL(&_bleMux);
  _maintDiscoveryDone = false;
  BikeSensor::quiesce();
  HW_DEBUG_PRINTF("[BLE] quiesce() (discovery open=%d)\n", wasOpen);
  return wasOpen;
}

bool BLERelayManager::maintenanceQuiesced() {
  return BikeSensor::quiesced();
}

bool BLERelayManager::removeSensorsForMaintenance() {
  bool ok = true;
  for (uint8_t slot = 0; slot < hw::kBleSlots; slot++) {
    ok = BikeSensor::remove(toSensorType(hw::bleTypeOfSlot(slot))) && ok;
  }
  _reconciler.forgetApplied();
  refreshSnapshot();
  return ok;
}

void BLERelayManager::stopForMaintenance() {
  portENTER_CRITICAL(&_bleMux);
  _commands.clear();
  _discoveryOps.clear();
  _pauseDesired = -1;
  portEXIT_CRITICAL(&_bleMux);
  _reconcileScheduled = false;
  _maintenanceStopped = true;
  HW_DEBUG_PRINTLN("[BLE] Stopped for maintenance");
}

// =============================================================================
// Callbacks (run inside BikeSensor::update(), loop task): record only
// =============================================================================

void BLERelayManager::staticOnEvent(const BikeSensor::EventInfo& e) {
  BLERelayManager* self = _instance;
  if (!self) return;
  if (e.type == BikeSensor::EventType::DATA) return;
  if (e.type == BikeSensor::EventType::DISCOVERY_DONE) {
    if (self->_doneCount >= 2) {
      self->_done[0] = self->_done[1];
      self->_doneCount = 1;
    }
    DoneRecord& d = self->_done[self->_doneCount++];
    d.generation = e.generation;
    d.count = e.count;
    d.reason = static_cast<uint8_t>(e.reason);
    d.mask = self->_discoveryRequestedMask;
    return;
  }
  if (self->_eventCount >= EVENT_FIFO_DEPTH) {
    self->_eventsDropped = true;
    return;
  }
  EventRecord& r = self->_events[self->_eventCount++];
  r.type = static_cast<uint8_t>(e.type);
  r.sensorType = BikeSensor::typeBit(e.sensorType);
  r.reason = e.reason;
}

void BLERelayManager::staticOnHeartRate(uint16_t bpm) {
  BLERelayManager* self = _instance;
  if (!self) return;
  self->_hrBpm = bpm;
  self->_hrValid = true;
  self->_hrNew = true;
}

void BLERelayManager::staticOnCadence(uint16_t rpm) {
  BLERelayManager* self = _instance;
  if (!self) return;
  self->_cadenceRpm = rpm;
  self->_cadenceNew = true;
}

// Internal helpers

uint8_t BLERelayManager::stringToTypeBit(const char* type) {
  if (!type) {
    return 0;
  }
  // Supported types: "HR", "CSC", "heart_rate", "speed_cadence" (case-insensitive)
  if (strcasecmp(type, "HR") == 0 || strcasecmp(type, "heart_rate") == 0) {
    return hw::kBleTypeHR;
  }
  if (strcasecmp(type, "CSC") == 0 || strcasecmp(type, "speed_cadence") == 0) {
    return hw::kBleTypeCSC;
  }
  return 0;
}

const char* BLERelayManager::typeBitToString(uint8_t typeBit) {
  switch (typeBit) {
    case hw::kBleTypeHR: return "HR";
    case hw::kBleTypeCSC: return "CSC";
    default: return "UNKNOWN";
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
      
      copyStr(cfg.name, entry.name, sizeof(cfg.name));
      copyStr(cfg.type, entry.type, sizeof(cfg.type));
      copyStr(cfg.mac, entry.mac, sizeof(cfg.mac));
      cfg.addrType = entry.addrType;
      cfg.typeBit = stringToTypeBit(cfg.type);
      
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

#endif // HW_ENABLE_BLE
