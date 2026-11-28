/**
 * @file BLERelayManager.cpp
 * @brief BLE Relay Manager implementation (BikeSensor 3.0)
 */

#include "../app/Config.h"

#if HW_ENABLE_BLE

#include "BLERelayManager.h"
#include "../core/DebugLog.h"
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

// Sensor link events: always on in serial-command (diagnostic) builds.
#if HW_ENABLE_SERIAL_COMMANDS
#define BLE_LINK_PRINTF HW_ERROR_PRINTF
#else
#define BLE_LINK_PRINTF HW_DEBUG_PRINTF
#endif

// HR, CSC, power and one central: the core's sdkconfig.h allows 3 links, the
// sketch folder's sdkconfig.h raises it and must come first on the include path.
static_assert(CONFIG_BT_NIMBLE_MAX_CONNECTIONS >= 4, "Homewind/sdkconfig.h (4 BLE links) not in effect");

// Static instance pointer for callbacks
BLERelayManager* BLERelayManager::_instance = nullptr;

namespace {

constexpr uint8_t kAllTypes = hw::kBleTypeHR | hw::kBleTypeCSC | hw::kBleTypePWR;

BikeSensor::SensorType toSensorType(uint8_t typeBit) {
  switch (typeBit) {
    case hw::kBleTypeHR: return BikeSensor::SensorType::HEART_RATE;
    case hw::kBleTypePWR: return BikeSensor::SensorType::POWER;
    default: return BikeSensor::SensorType::SPEED_CADENCE;
  }
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
  , _pauseStartMs(0)
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
  , _advertisedMask(kAllTypes)   // must equal the begin() mask: advertised until the first refresh
  , _discoveryStopIssued(false)
  , _maintDiscoveryDone(false)
  , _lastSnapshotMs(0)
  , _eventCount(0)
  , _eventsDropped(false)
  , _appLeftCleanly(false)
  , _doneCount(0)
  , _hrBpm(0)
  , _hrValid(false)
  , _hrNew(false)
  , _cadenceNew(false)
  , _powerNew(false)
  , _bikeMetricsMs(0)
{
  _instance = this;
  memset(_configuredSensors, 0, sizeof(_configuredSensors));
  memset(_discovered, 0, sizeof(_discovered));
#if HW_ENABLE_TRAINER_MIRROR
  memset(&_trainer, 0, sizeof(_trainer));
  _trainer.setup = kTrainerSetupIdle;
  memset(&_trainerPending, 0, sizeof(_trainerPending));
  _trainerSetupScan = false;
  _trainerPowerNew = false;
  _trainerSpeedDkmh = 0;
  _trainerSpeedValid = false;
#endif
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
#if HW_ENABLE_TRAINER_MIRROR
  loadTrainerFromNVS();
  BikeSensor::setTrainerMirror(_trainer.enabled);
#endif
  
  BikeSensor::setEventCallback(staticOnEvent);
  BikeSensor::setHeartRateCallback(staticOnHeartRate);
  BikeSensor::setCadenceCallback(staticOnCadence);
  BikeSensor::setPowerCallback(staticOnPower);
#if HW_ENABLE_TRAINER_MIRROR
  BikeSensor::setTrainerPowerCallback(staticOnTrainerPower);
  BikeSensor::setTrainerDataCallback(staticOnTrainerData);
#endif
  if (!BikeSensor::begin("homewind", kAllTypes, BikeSensor::Mode::MAC)) {
    HW_ERROR_PRINTLN("[BLE] BikeSensor::begin() failed");
    return false;
  }
#if HW_ENABLE_TRAINER_MIRROR && HW_ENABLE_SERIAL_COMMANDS
  Serial.printf("[BLE] Trainer runtime in %s\n", BikeSensor::trainerInPsram() ? "PSRAM" : "internal RAM");
#endif
  
  initialized = true;
  
  // Slot order HR, CSC, PWR: a MAC saved for several types stays with the first.
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
  const bool cadenceNew = _cadenceNew;
  _cadenceNew = false;
#if HW_ENABLE_TRAINER_MIRROR
  const bool trainerPowerNew = _trainerPowerNew;
  _trainerPowerNew = false;
#else
  constexpr bool trainerPowerNew = false;
#endif
#if HW_ENABLE_FANS && HW_ENABLE_FAN_POWER_MODE
  if (g_fanHrAdapter && (_powerNew || cadenceNew || trainerPowerNew)) {
    const uint32_t now = millis();
    uint32_t ageMs = 0;
    BikeSensor::PowerSample power{};
    if (_powerNew && BikeSensor::lastPower(power, ageMs)) {
      g_fanHrAdapter->onPowerSample(power.watts, now - ageMs);
    }
#if HW_ENABLE_TRAINER_MIRROR
    const bool trainerValid = trainerPowerNew && BikeSensor::trainerPower(power, ageMs);
    if (hw::bikePowerFromTrainer(BikeSensor::connected(BikeSensor::SensorType::POWER), trainerValid)) {
      g_fanHrAdapter->onPowerSample(power.watts, now - ageMs);
    }
#endif
    uint16_t rpm = 0;
    if (cadenceNew && BikeSensor::lastCadence(rpm, ageMs)) {
      g_fanHrAdapter->onCadenceSample(rpm, now - ageMs);
    }
  }
#endif
  if (_powerNew || cadenceNew || trainerPowerNew || millis() - _bikeMetricsMs >= BIKE_METRICS_POLL_MS) {
    _powerNew = false;
    updateBikeMetrics();
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
  // The WebUI pauses for a sensor search (15 s scan); a page that disappears during the
  // search must not leave the relay paused until the next WebUI resumes it
  static constexpr unsigned long kServerPauseMaxMs = 30000;
  const bool pauseExpired = _serverPaused && millis() - _pauseStartMs >= kServerPauseMaxMs;
  if (pause == 1) {
    if (!_serverPaused) {
      BikeSensor::pause();
      _serverPaused = true;
    }
    _pauseStartMs = millis();
  } else if ((pause == 0 || pauseExpired) && _serverPaused) {
    BikeSensor::resume();
    _serverPaused = false;
    markSensorsDirty();   // changes seen while paused were not sent
  }

  if (_reconcileScheduled) {
    runReconcile(true);
  }

  // Battery and link state for the snapshot; sent only on a change (WebSocketTelemetry
  // still refreshes it every 30 s, 120 s with two clients)
  const unsigned long now = millis();
  if (now - _lastSnapshotMs >= 1000) {
    if (refreshSnapshot() && !_serverPaused) {
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
#if HW_ENABLE_TRAINER_MIRROR
  const HwDiscoveredSensor rec = valid ? _discovered[index] : HwDiscoveredSensor{};
  const uint8_t requestedMask = _discoveredMask;
  const bool setupRunning = _trainer.setup == kTrainerSetupRunning;
#endif
  portEXIT_CRITICAL(&_bleMux);
  if (!valid) {
    return BleEnqueueResult::STALE;
  }
#if HW_ENABLE_TRAINER_MIRROR
  if (setupRunning) {
    return BleEnqueueResult::BUSY;
  }
  switch (sensorRule(rec, requestedMask)) {
    case hw::BleDeviceRule::TOO_MANY: return BleEnqueueResult::TOO_MANY;
    case hw::BleDeviceRule::TRAINER_MAC: return BleEnqueueResult::TRAINER_MAC;
    default: break;
  }
#endif
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

#if HW_ENABLE_TRAINER_MIRROR
BleEnqueueResult BLERelayManager::enqueueTrainerSetup(const char* mac, const char* name) {
  hw::BleCommand cmd;
  cmd.kind = hw::BleCmdKind::TRAINER_SETUP;
  lowerMac(cmd.trainer.mac, mac);
  copyStr(cmd.trainer.name, name, sizeof(cmd.trainer.name));
  uint8_t removeMask;
  const BleEnqueueResult check = trainerSetupCheck(cmd.trainer.mac, removeMask);
  return check == BleEnqueueResult::ACCEPTED ? pushCommand(cmd) : check;
}

// The WebUI only has the index of DISCOVERY_RESULTS. trainer/status sends the
// name unescaped: '"', '\' and control characters become '?'.
BleEnqueueResult BLERelayManager::enqueueTrainerSetupFromDiscovery(uint32_t generation, uint8_t index) {
  portENTER_CRITICAL(&_bleMux);
  const bool valid = generation == _discoveredGeneration && index < _discoveredCount;
  const HwDiscoveredSensor rec = valid ? _discovered[index] : HwDiscoveredSensor{};
  portEXIT_CRITICAL(&_bleMux);
  if (!valid) {
    return BleEnqueueResult::STALE;
  }
  char mac[18];
  formatMac(mac, rec.mac);
  char name[sizeof(rec.name)];
  copyStr(name, rec.name, sizeof(name));
  for (char* c = name; *c; c++) {
    if (*c == '"' || *c == '\\' || static_cast<unsigned char>(*c) < 0x20) *c = '?';
  }
  return enqueueTrainerSetup(mac, name);
}

BleEnqueueResult BLERelayManager::enqueueTrainerEnable(bool on) {
  portENTER_CRITICAL(&_bleMux);
  const bool known = _trainer.mac[0] != '\0';
  const bool setupRunning = _trainer.setup == kTrainerSetupRunning;
  portEXIT_CRITICAL(&_bleMux);
  if (setupRunning) {
    return BleEnqueueResult::BUSY;   // the finished setup would store the trainer again
  }
  if (on && !known) {
    return BleEnqueueResult::NO_TRAINER;
  }
  hw::BleCommand cmd;
  cmd.kind = hw::BleCmdKind::TRAINER_ENABLE;
  cmd.on = on;
  return pushCommand(cmd);
}

BleEnqueueResult BLERelayManager::enqueueTrainerForget() {
  portENTER_CRITICAL(&_bleMux);
  const bool setupRunning = _trainer.setup == kTrainerSetupRunning;
  portEXIT_CRITICAL(&_bleMux);
  if (setupRunning) {
    return BleEnqueueResult::BUSY;
  }
  hw::BleCommand cmd;
  cmd.kind = hw::BleCmdKind::TRAINER_FORGET;
  return pushCommand(cmd);
}

void BLERelayManager::trainerStatus(TrainerStatus& out) {
  portENTER_CRITICAL(&_bleMux);
  out = _trainer;
  portEXIT_CRITICAL(&_bleMux);
}

void BLERelayManager::slotMacsLocked(const char* out[hw::kBleSlots]) const {
  for (uint8_t slot = 0; slot < hw::kBleSlots; slot++) out[slot] = "";
  for (uint8_t i = 0; i < _snapshotCount; i++) {
    const int slot = hw::bleSlotOf(stringToTypeBit(_snapshot[i].type));
    if (slot >= 0) out[slot] = _snapshot[i].mac;
  }
}

hw::BleDeviceRule BLERelayManager::sensorRule(const HwDiscoveredSensor& rec, uint8_t requestedMask) {
  char mac[18];
  formatMac(mac, rec.mac);
  const uint8_t type = hw::bleResolveDiscoveryType(rec.typeMask, requestedMask);
  const char* slots[hw::kBleSlots];
  portENTER_CRITICAL(&_bleMux);
  slotMacsLocked(slots);
  const hw::BleDeviceRule rule = hw::bleRuleForSensor(slots, _trainer.mac, type, mac);
  portEXIT_CRITICAL(&_bleMux);
  return rule;
}

// BUSY while a Homewind discovery or a setup is open, so the setup's
// DISCOVERY_DONE cannot replace the WebUI's discovery snapshot.
BleEnqueueResult BLERelayManager::trainerSetupCheck(const char* mac, uint8_t& removeMask) {
  const char* slots[hw::kBleSlots];
  portENTER_CRITICAL(&_bleMux);
  const bool open = _discoveryActive || _discoveryOps.size() > 0 || _trainer.setup == kTrainerSetupRunning;
  slotMacsLocked(slots);
  const bool fits = hw::bleRuleForTrainer(slots, mac, removeMask) == hw::BleDeviceRule::OK;
  portEXIT_CRITICAL(&_bleMux);
  if (open) return BleEnqueueResult::BUSY;
  return fits ? BleEnqueueResult::ACCEPTED : BleEnqueueResult::TOO_MANY;
  }
#endif
  
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
#if HW_ENABLE_TRAINER_MIRROR
    if (trainerHolds(cfg.mac)) {
      HW_ERROR_PRINTF("[BLE] %s slot holds the trainer %s: not started\n", cfg.type, cfg.mac);
      continue;
    }
#endif
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

  const uint8_t type = hw::bleResolveDiscoveryType(rec.typeMask, requestedMask);
  if (type == 0) {
    HW_DEBUG_PRINTF("[BLE] setSensor: index %u has no HR/CSC/PWR type\n", index);
    return;
  }
#if HW_ENABLE_TRAINER_MIRROR
  if (_trainer.setup == kTrainerSetupRunning || sensorRule(rec, requestedMask) != hw::BleDeviceRule::OK) {
    HW_DEBUG_PRINTF("[BLE] setSensor: index %u refused (trainer setup or device rule)\n", index);
    return;
  }
#endif

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
      deleteConfig(i);
      break;
    }

    case hw::BleCmdKind::CONNECT: {
      const int i = findConfigByName(cmd.name);
      if (i < 0) break;
      const SensorConfig& cfg = _configuredSensors[i];
      if (cfg.mac[0] == '\0') break;
#if HW_ENABLE_TRAINER_MIRROR
      if (trainerHolds(cfg.mac)) break;
#endif
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
      runReconcile(false, false);   // the reconcile after setTargetsFromConfig() advertises
      _sensorCount = 0;
      if (!loadConfigFromNVS()) {
        _sensorCount = 0;
      }
      setTargetsFromConfig();
      break;

#if HW_ENABLE_TRAINER_MIRROR
    case hw::BleCmdKind::TRAINER_SETUP:
      applyTrainerSetup(cmd.trainer.mac, cmd.trainer.name);
      break;

    case hw::BleCmdKind::TRAINER_ENABLE:
      if (_trainer.setup == kTrainerSetupRunning) {   // queued just behind a TRAINER_SETUP
        HW_ERROR_PRINTLN("[BLE] Trainer enable ignored: setup running");
        break;
      }
      saveTrainerToNVS(cmd.on && _trainer.mac[0] != '\0', nullptr);
      setTrainerSetupResult(kTrainerSetupIdle);   // no stale "ok"
      break;

    case hw::BleCmdKind::TRAINER_FORGET: {
      if (_trainer.setup == kTrainerSetupRunning) {
        HW_ERROR_PRINTLN("[BLE] Trainer forget ignored: setup running");
        break;
      }
      if (!BikeSensor::trainerForget()) {
        HW_ERROR_PRINTLN("[BLE] trainerForget() failed");
      }
      const TrainerDev none = {};
      saveTrainerToNVS(false, &none);
      setTrainerSetupResult(kTrainerSetupIdle);
      break;
    }
#else
    default:
      break;
#endif
  }
  refreshSnapshot();
  markSensorsDirty();
  if (cmd.kind != hw::BleCmdKind::CONNECT && cmd.kind != hw::BleCmdKind::DISCONNECT) {
    // Configured set may have changed (a sensor that never connects sends no event)
    notifyDisplay(hw::kBleTypeHR);
    notifyDisplay(hw::kBleTypeCSC);
  }
}

void BLERelayManager::deleteConfig(int i) {
  _reconciler.setTarget(_configuredSensors[i].typeBit, "", "", -1, false, true);
  for (uint8_t j = i; j + 1 < _sensorCount; j++) {
    _configuredSensors[j] = _configuredSensors[j + 1];
  }
  _sensorCount--;
  saveConfigToNVS();
  _reconcileScheduled = true;
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
    _appLeftCleanly = false;   // the app's CENTRAL_DISCONNECTED may be among the dropped events
    refreshSnapshot();
    notifyDisplay(hw::kBleTypeHR);
    notifyDisplay(hw::kBleTypeCSC);
    if (!BikeSensor::connected(BikeSensor::SensorType::HEART_RATE)) {
      _hrValid = false;
      _hrNew = false;
#if HW_ENABLE_FANS
      if (g_fanHrAdapter) g_fanHrAdapter->onHeartRateDisconnected(millis());
#endif
    }
#if HW_ENABLE_TRAINER_MIRROR
    // A dropped TRAINER_SETUP_DONE would leave the result at running.
    BikeSensor::TrainerInfo trainer;
    BikeSensor::trainerInfo(trainer);
    if (_trainer.setup == kTrainerSetupRunning && trainer.state != BikeSensor::TrainerState::SETUP) {
      onTrainerSetupDone(static_cast<uint8_t>(BikeSensor::TrainerCheck::LINK_FAILED));
    }
    // Its DISCOVERY_DONE may be lost too: it must not swallow the next discovery's.
    if (_trainer.setup != kTrainerSetupRunning) _trainerSetupScan = false;
#endif
  }

  for (uint8_t n = 0; n < _eventCount; n++) {
    const EventRecord& e = _events[n];
    const BikeSensor::EventType type = static_cast<BikeSensor::EventType>(e.type);
    const uint8_t typeBit = e.sensorType;
    [[maybe_unused]] const char* typeStr = typeBitToString(typeBit);

    switch (type) {
      case BikeSensor::EventType::SENSOR_CONNECTED:
        BLE_LINK_PRINTF("[BLE] Sensor connected: %s\n", typeStr);
        adoptSensorAddress(typeBit);
        refreshSnapshot();
        if (!_serverPaused) markSensorsDirty();
        notifyDisplay(typeBit);
        break;

      case BikeSensor::EventType::SENSOR_DISCONNECTED:
        BLE_LINK_PRINTF("[BLE] Sensor disconnected: %s (reason=0x%02X: %s)\n", typeStr,
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
        BLE_LINK_PRINTF("[BLE] Sensor connect failed: %s (rc=%d)\n", typeStr, e.reason);
        break;

      case BikeSensor::EventType::SENSOR_FOUND:
        HW_DEBUG_PRINTF("[BLE] Sensor found: %s\n", typeStr);
        break;

      case BikeSensor::EventType::HOST_RESET:
        HW_ERROR_PRINTF("[BLE] NimBLE host reset (reason=%d)\n", e.reason);
        _appLeftCleanly = false;
        break;

      case BikeSensor::EventType::CENTRAL_DISCONNECTED:
        BLE_LINK_PRINTF("[BLE] App disconnected (reason=0x%02X: %s)\n", e.reason,
                        BikeSensor::disconnectReasonString(e.reason));
        // NimBLE reports HCI reasons as BLE_HS_ERR_HCI_BASE (0x200) + code
        _appLeftCleanly = e.reason == 0x200 + 0x13;  // Remote User Terminated
        break;

#if HW_ENABLE_TRAINER_MIRROR
      case BikeSensor::EventType::TRAINER_CONNECTED:
        if (e.reason == 1) {
          HW_ERROR_PRINTLN("[BLE] Trainer GATT changed: not mirrored, run the trainer setup again");
        } else {
          HW_DEBUG_PRINTLN("[BLE] Trainer connected");
        }
        refreshSnapshot();
        notifyDisplay(hw::kBleTypeCSC);
        break;

      case BikeSensor::EventType::TRAINER_DISCONNECTED:
        HW_DEBUG_PRINTF("[BLE] Trainer disconnected (reason=0x%02X)\n", e.reason);
        refreshSnapshot();
        notifyDisplay(hw::kBleTypeCSC);
        break;

      case BikeSensor::EventType::TRAINER_SETUP_DONE:
        onTrainerSetupDone(static_cast<uint8_t>(e.reason));
        break;
#endif

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

void BLERelayManager::runReconcile(bool throttled, bool advertise) {
  const unsigned long now = millis();
  if (throttled && now - _lastReconcileMs < RECONCILE_INTERVAL_MS) {
    return;
  }
  _lastReconcileMs = now;
  _reconcileScheduled = false;
  LibraryPort port;
  _reconciler.run(port);
  if (advertise) updateAdvertisedTypes();
  refreshSnapshot();
  if (!_serverPaused) markSensorsDirty();
}

// While paused, refreshAdvertising() applies the data without restarting
// advertising; resume() starts it with the new data.
void BLERelayManager::updateAdvertisedTypes() {
  if (MaintenanceMode::admissionClosed()) return;
  uint8_t configured = 0;
  for (uint8_t i = 0; i < _sensorCount; i++) configured |= _configuredSensors[i].typeBit;
  const uint8_t mask = hw::bleAdvertisedTypes(_reconciler.addedMask(), configured);
  if (mask == _advertisedMask) return;
  BikeSensor::setAdvertisedTypes(mask);
  if (!BikeSensor::refreshAdvertising()) {
    HW_ERROR_PRINTF("[BLE] refreshAdvertising(0x%02X) failed, retry at the next reconcile\n", mask);
    return;
  }
  HW_DEBUG_PRINTF("[BLE] Advertised types 0x%02X -> 0x%02X\n", _advertisedMask, mask);
  _advertisedMask = mask;
}

bool BLERelayManager::refreshSnapshot() {
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
#if HW_ENABLE_TRAINER_MIRROR
  BikeSensor::TrainerInfo trainer;
  BikeSensor::trainerInfo(trainer);
#endif
  portENTER_CRITICAL(&_bleMux);
  bool changed = count != _snapshotCount;
  for (uint8_t i = 0; i < count; i++) {
    const HwSensorInfo& o = _snapshot[i];
    const HwSensorInfo& s = next[i];
    if (!changed && (o.connected != s.connected || o.battery != s.battery || o.duplicate != s.duplicate ||
                     strcmp(o.name, s.name) != 0 || strcmp(o.type, s.type) != 0 || strcmp(o.mac, s.mac) != 0)) {
      changed = true;
    }
    _snapshot[i] = next[i];
  }
  _snapshotCount = count;
#if HW_ENABLE_TRAINER_MIRROR
  const bool trainerChanged = _trainer.mirror != trainer.mirror ||
                              _trainer.appConnected != trainer.appConnected ||
                              _trainer.state != static_cast<uint8_t>(trainer.state);
  _trainer.mirror = trainer.mirror;
  _trainer.appConnected = trainer.appConnected;
  _trainer.state = static_cast<uint8_t>(trainer.state);
#endif
  portEXIT_CRITICAL(&_bleMux);
#if HW_ENABLE_TRAINER_MIRROR
  if (trainerChanged) markTrainerDirty();
#endif
  _connectedCount = connected;
  _lastSnapshotMs = millis();
  return changed;
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
    } else if (typeBit == hw::kBleTypeCSC || typeBit == hw::kBleTypePWR) {
      g_displayManager->updateCSCState();
    }
  }
#else
  (void)typeBit;
#endif
}

// Power is valid while the meter (or, without a connected meter, the trainer)
// is subscribed and has delivered; a value older than hw::kBikeStaleMs is sent
// as 0 (BikeMetrics.h).
void BLERelayManager::updateBikeMetrics() {
  _bikeMetricsMs = millis();
  BikeSensor::PowerSample power{};
  uint32_t powerAgeMs = 0;
#if HW_ENABLE_TRAINER_MIRROR
  bool powerValid = BikeSensor::lastPower(power, powerAgeMs);
  BikeSensor::PowerSample trainerSample{};
  uint32_t trainerAgeMs = 0;
  // Like the trainer power, the wheel speed ends with the ready link.
  const bool trainerLive = BikeSensor::trainerPower(trainerSample, trainerAgeMs);
  if (!trainerLive) {
    _trainerWheel = hw::WheelSpeed{};
  }
  _trainerSpeedDkmh = hw::wheelSpeedDkmh(_trainerWheel, _bikeMetricsMs);
  _trainerSpeedValid = _trainerWheel.valid;
  const bool fromTrainer = hw::bikePowerFromTrainer(BikeSensor::connected(BikeSensor::SensorType::POWER),
                                                    trainerLive);
  if (fromTrainer) {
    power = trainerSample;
    powerAgeMs = trainerAgeMs;
    powerValid = true;
  }
#else
  const bool powerValid = BikeSensor::lastPower(power, powerAgeMs);
  constexpr bool fromTrainer = false;
#endif
  uint16_t rpm = 0;
  uint32_t cadenceAgeMs = 0;
  const bool cadenceValid = BikeSensor::lastCadence(rpm, cadenceAgeMs);
  BikeSensor::SensorInfo info;
  const bool fromPower = BikeSensor::info(BikeSensor::SensorType::POWER, info) && info.providesCadence;

  const hw::BikeMetrics next = hw::makeBikeMetrics(powerValid, power.watts, powerAgeMs,
                                                   cadenceValid, rpm, cadenceAgeMs, fromPower,
                                                   fromTrainer);
  if (next == _bikeMetrics) {
    return;
  }
  _bikeMetrics = next;
#if HW_ENABLE_DISPLAY
  if (g_displayManager) {
    g_displayManager->setBikeMetrics(next);
  }
#endif
#if HOMEWIND_ENABLE_WEBSOCKET
    if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::BIKE_METRICS);
    }
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
#if HW_ENABLE_TRAINER_MIRROR
    if (self->_trainerSetupScan) {
      self->_trainerSetupScan = false;
      return;
    }
#endif
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

void BLERelayManager::staticOnCadence(uint16_t) {
  BLERelayManager* self = _instance;
  if (!self) return;
  self->_cadenceNew = true;
}

void BLERelayManager::staticOnPower(const BikeSensor::PowerSample&) {
  BLERelayManager* self = _instance;
  if (!self) return;
  self->_powerNew = true;
}

#if HW_ENABLE_TRAINER_MIRROR
void BLERelayManager::staticOnTrainerPower(const BikeSensor::PowerSample&) {
  BLERelayManager* self = _instance;
  if (!self) return;
  self->_trainerPowerNew = true;
}

void BLERelayManager::staticOnTrainerData(uint16_t uuid16, const uint8_t* data, uint8_t len) {
  BLERelayManager* self = _instance;
  if (!self || uuid16 != 0x2A63) return;
  hw::wheelSpeedUpdate(self->_trainerWheel, data, len, millis());
}

void BLERelayManager::markTrainerDirty() {
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) {
    _telemetry->markDirty(TelemetryFrameType::TRAINER_STATUS);
  }
#endif
  }
#endif

#if HW_ENABLE_TRAINER_MIRROR
// =============================================================================
// Trainer mirror (loop task)
// =============================================================================

// D8 boot rule: with trn_on, a sensor slot holding the trainer's MAC stays off.
bool BLERelayManager::trainerHolds(const char* mac) const {
  return _trainer.enabled && _trainer.mac[0] != '\0' && strcasecmp(mac, _trainer.mac) == 0;
}

void BLERelayManager::setTrainerSetupResult(uint8_t result) {
  portENTER_CRITICAL(&_bleMux);
  _trainer.setup = result;
  portEXIT_CRITICAL(&_bleMux);
  markTrainerDirty();
}

void BLERelayManager::removeSlots(uint8_t typeMask) {
  for (uint8_t slot = 0; slot < hw::kBleSlots; slot++) {
    const uint8_t type = hw::bleTypeOfSlot(slot);
    const int i = (typeMask & type) ? findConfigByType(type) : -1;
    if (i < 0) continue;
    HW_ERROR_PRINTF("[BLE] %s slot %s is the trainer: removed\n", _configuredSensors[i].type,
                    _configuredSensors[i].mac);
    deleteConfig(i);
  }
  runReconcile(false);
  markSensorsDirty();   // also while paused: the open sensor dialog shows the removal
}

void BLERelayManager::applyTrainerSetup(const char* mac, const char* name) {
  uint8_t removeMask = 0;
  const BleEnqueueResult check = trainerSetupCheck(mac, removeMask);
  if (check != BleEnqueueResult::ACCEPTED) {
    HW_DEBUG_PRINTF("[BLE] Trainer setup %s refused (%s)\n", mac,
                    check == BleEnqueueResult::BUSY ? "busy" : "device rule");
    setTrainerSetupResult(check == BleEnqueueResult::BUSY ? kTrainerSetupBusy : kTrainerSetupRule);
    return;
  }
  // The trainer stops advertising while a sensor slot holds its link (G2).
  if (removeMask != 0) removeSlots(removeMask);
  if (!BikeSensor::trainerSetup(mac)) {
    HW_DEBUG_PRINTF("[BLE] trainerSetup(%s) refused\n", mac);
    setTrainerSetupResult(kTrainerSetupBusy);
    return;
  }
  copyStr(_trainerPending.mac, mac, sizeof(_trainerPending.mac));
  copyStr(_trainerPending.name, name, sizeof(_trainerPending.name));
  _trainerSetupScan = true;
  setTrainerSetupResult(kTrainerSetupRunning);
  HW_DEBUG_PRINTF("[BLE] Trainer setup %s (%s) started\n", mac, name);
}

// D6: setup OK turns the mirror on (after a restart).
void BLERelayManager::onTrainerSetupDone(uint8_t check) {
  HW_DEBUG_PRINTF("[BLE] Trainer setup %s done: check %u\n", _trainerPending.mac, check);
  if (check == static_cast<uint8_t>(BikeSensor::TrainerCheck::OK) && MaintenanceMode::admissionClosed()) {
    check = static_cast<uint8_t>(BikeSensor::TrainerCheck::STORE_FAILED);   // trn_on/trn_dev not stored
  } else if (check == static_cast<uint8_t>(BikeSensor::TrainerCheck::OK)) {
    uint8_t removeMask = 0;
    trainerSetupCheck(_trainerPending.mac, removeMask);   // a slot set to the trainer during the setup
    if (removeMask != 0) removeSlots(removeMask);
    if (!saveTrainerToNVS(true, &_trainerPending)) {
      check = static_cast<uint8_t>(BikeSensor::TrainerCheck::STORE_FAILED);
    }
  }
  setTrainerSetupResult(check);
}

void BLERelayManager::loadTrainerFromNVS() {
  TrainerDev dev = {};
  bool on = false;
  Preferences prefs;
  if (prefs.begin(NVS_NAMESPACE, true)) {
    on = prefs.getUChar(NVS_KEY_TRAINER_ON, 0) != 0;
    if (prefs.getBytesLength(NVS_KEY_TRAINER_DEV) == sizeof(dev)) {
      prefs.getBytes(NVS_KEY_TRAINER_DEV, &dev, sizeof(dev));
    }
    prefs.end();
  }
  dev.mac[sizeof(dev.mac) - 1] = '\0';
  dev.name[sizeof(dev.name) - 1] = '\0';
  portENTER_CRITICAL(&_bleMux);
  _trainer.enabled = on;
  memcpy(_trainer.mac, dev.mac, sizeof(dev.mac));
  memcpy(_trainer.name, dev.name, sizeof(dev.name));
  portEXIT_CRITICAL(&_bleMux);
  HW_DEBUG_PRINTF("[BLE] [NVS] Trainer \"%s\" %s, mirror %s\n", dev.name, dev.mac, on ? "on" : "off");
}

// dev: nullptr keeps trn_dev; mac "" erases it.
bool BLERelayManager::saveTrainerToNVS(bool on, const TrainerDev* dev) {
  Preferences prefs;
  bool ok = prefs.begin(NVS_NAMESPACE, false);
  if (ok) {
    ok = prefs.putUChar(NVS_KEY_TRAINER_ON, on ? 1 : 0) == 1;
    if (dev && dev->mac[0] != '\0') {
      ok = prefs.putBytes(NVS_KEY_TRAINER_DEV, dev, sizeof(*dev)) == sizeof(*dev) && ok;
    } else if (dev) {
      prefs.remove(NVS_KEY_TRAINER_DEV);
    }
    prefs.end();
  }
  if (!ok) {
    HW_ERROR_PRINTLN("[BLE] [NVS] Failed to save the trainer");
    return false;
  }
  portENTER_CRITICAL(&_bleMux);
  _trainer.enabled = on;
  if (dev) {
    memcpy(_trainer.mac, dev->mac, sizeof(dev->mac));
    memcpy(_trainer.name, dev->name, sizeof(dev->name));
  }
  portEXIT_CRITICAL(&_bleMux);
  markTrainerDirty();
  return true;
}
#endif

// Internal helpers

uint8_t BLERelayManager::stringToTypeBit(const char* type) {
  if (!type) {
    return 0;
  }
  // Supported types: "HR", "CSC", "PWR", "heart_rate", "speed_cadence", "power" (case-insensitive)
  if (strcasecmp(type, "HR") == 0 || strcasecmp(type, "heart_rate") == 0) {
    return hw::kBleTypeHR;
  }
  if (strcasecmp(type, "CSC") == 0 || strcasecmp(type, "speed_cadence") == 0) {
    return hw::kBleTypeCSC;
  }
  if (strcasecmp(type, "PWR") == 0 || strcasecmp(type, "power") == 0) {
    return hw::kBleTypePWR;
  }
  return 0;
}

const char* BLERelayManager::typeBitToString(uint8_t typeBit) {
  switch (typeBit) {
    case hw::kBleTypeHR: return "HR";
    case hw::kBleTypeCSC: return "CSC";
    case hw::kBleTypePWR: return "PWR";
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
  
  // v3 blob, or a v2 blob (1.4.45 - 1.6.x) converted to v3
  uint8_t blob[sizeof(hw::BleNvsData)];
  const size_t len = prefs.getBytesLength(NVS_KEY_DATA);
  if (len == 0) {
    prefs.end();
    HW_DEBUG_PRINTLN("[BLE] [NVS] No config found in NVS");
      return false;
    }
  const size_t got = len <= sizeof(blob) ? prefs.getBytes(NVS_KEY_DATA, blob, len) : 0;
  prefs.end();

  hw::BleNvsData data;
  const hw::BleNvsLoad result = hw::bleNvsDecode(blob, got, data);
  if (result != hw::BleNvsLoad::OK && result != hw::BleNvsLoad::MIGRATED) {
    HW_ERROR_PRINTF("[BLE] [NVS] ble_data rejected (%u bytes): bad %s\n", static_cast<unsigned>(len),
                    result == hw::BleNvsLoad::BAD_SIZE ? "size"
                    : result == hw::BleNvsLoad::BAD_VERSION ? "version" : "CRC");
      return false;
    }
    
    if (data.sensorCount > MAX_SENSORS) {
    HW_DEBUG_PRINTF("[BLE] [NVS] Clamping sensorCount from %u to %u\n", data.sensorCount, MAX_SENSORS);
      data.sensorCount = MAX_SENSORS;
    }
    
    // Copy data to runtime structures
    _sensorCount = data.sensorCount;
    
    for (uint8_t i = 0; i < _sensorCount; i++) {
    const hw::BleNvsEntry& entry = data.sensors[i];
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
  if (result == hw::BleNvsLoad::MIGRATED) {
    HW_DEBUG_PRINTLN("[BLE] [NVS] Migrating ble_data v2 -> v3");
    if (!saveConfigToNVS()) {
      HW_ERROR_PRINTLN("[BLE] [NVS] v3 write failed, v2 stays and migrates again next boot");
    }
  }
    return true;
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
  hw::BleNvsData data;
  memset(&data, 0, sizeof(data));
  data.version = hw::kBleNvsVersion;
  data.sensorCount = _sensorCount;
  
  for (uint8_t i = 0; i < _sensorCount; i++) {
    const SensorConfig& cfg = _configuredSensors[i];
    hw::BleNvsEntry& entry = data.sensors[i];
    
    strncpy(entry.name, cfg.name, sizeof(entry.name) - 1);
    entry.name[sizeof(entry.name) - 1] = '\0';
    
    strncpy(entry.type, cfg.type, sizeof(entry.type) - 1);
    entry.type[sizeof(entry.type) - 1] = '\0';
    
    strncpy(entry.mac, cfg.mac, sizeof(entry.mac) - 1);
    entry.mac[sizeof(entry.mac) - 1] = '\0';
    
    entry.addrType = cfg.addrType;
    
    HW_DEBUG_PRINTF("[BLE] [NVS] → Sensor %u: %s (%s) addrType=%d\n", i, cfg.name, cfg.type, cfg.addrType);
  }
  
  hw::bleNvsSeal(data);
  
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
