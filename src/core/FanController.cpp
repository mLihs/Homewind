/**
 * @file FanController.cpp
 * @brief Fan controller on SmartMiFan 2.0
 */

#include "../app/Config.h"

#if HW_ENABLE_FANS

#include "FanController.h"
#include "../core/DebugLog.h"
#include "../core/HeapDiagnostics.h"
#include "../core/Crc16.h"
#include "../core/TextUtil.h"
#include "../core/SystemState.h"
#include "MaintenanceMode.h"

#if HW_ENABLE_BLE
#include "../core/FanHRAdapter.h"
extern FanHRAdapter* g_fanHrAdapter;
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
#endif

#if HW_ENABLE_DISPLAY
#include "DisplayManager.h"
#endif

#ifndef HW_FAN_HANDSHAKE_TTL_S
#define HW_FAN_HANDSHAKE_TTL_S 60
#endif

static FanController* s_fanControllerSelf = nullptr;

FanController::FanController()
  : initialized(false)
  , _maintenanceStopped(false)
  , _postConnectSpeed(HW_FAN_POST_CONNECT_SPEED)
  , _handshakeTtlS(HW_FAN_HANDSHAKE_TTL_S)
#if HOMEWIND_ENABLE_WEBSOCKET
  , _telemetry(nullptr)
#endif
  , _fanCount(0)
  , _discoveryIsRecovery(false)
  , _discoveryRecoveryNvsIndex(-1)
  , _connectInProgress(false)
  , _postConnectCompleted(false)
  , _postConnectStep(PostConnectStep::IDLE)
  , _postConnectStepStartTime(0)
  , _postConnectSeq(0)
  , _reqHead(0)
  , _reqCount(0)
#if defined(ARDUINO_ARCH_ESP32)
  , _reqMux(nullptr)
#endif
  , _cleanupIndex(0xFF)
  , _cleanupStartMs(0)
  , _cleanupPending(false)
  , _cleanupSpeedDone(false)
  , _cleanupPowerDone(false)
  , _nvsDirty(false)
{
  memset(_fans, 0, sizeof(_fans));
  memset(_lastControlStateChangeMs, 0, sizeof(_lastControlStateChangeMs));
  memset(_lastRecoveryDiscoveryAttemptMs, 0, sizeof(_lastRecoveryDiscoveryAttemptMs));
  memset(_autoRecoveryAttempts, 0, sizeof(_autoRecoveryAttempts));
  memset(_autoRecoveryExhausted, 0, sizeof(_autoRecoveryExhausted));
  memset(_autoRecoveryActive, 0, sizeof(_autoRecoveryActive));
  memset(_lastAutoRecoveryAttemptMs, 0, sizeof(_lastAutoRecoveryAttemptMs));
  memset(_unhealthyStreak, 0, sizeof(_unhealthyStreak));
  memset(_lastUnhealthyCheckMs, 0, sizeof(_lastUnhealthyCheckMs));
  memset(_softActive, 0, sizeof(_softActive));
  memset(_req, 0, sizeof(_req));
}

FanController::~FanController() {
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux) {
    vSemaphoreDelete(_reqMux);
    _reqMux = nullptr;
  }
#endif
  if (s_fanControllerSelf == this) s_fanControllerSelf = nullptr;
}

#if HOMEWIND_ENABLE_WEBSOCKET
bool FanController::begin(WebSocketTelemetry* telemetry) {
#else
bool FanController::begin() {
#endif
  if (initialized) return true;

  HW_DEBUG_PRINTLN("[Fans] Initializing Fan Controller (SmartMiFan 2.0)");
#if HOMEWIND_ENABLE_WEBSOCKET
  _telemetry = telemetry;
#endif
#if defined(ARDUINO_ARCH_ESP32)
  if (!_reqMux) _reqMux = xSemaphoreCreateMutex();
#endif

  if (!SmartMiFan::begin(_fanUdp)) {
    HW_ERROR_PRINTLN("[Fans] SmartMiFan::begin failed");
    return false;
  }
  SmartMiFan::setEventCallback(&FanController::onLibraryEvent);
  s_fanControllerSelf = this;

  if (!loadFansFromNVS()) {
    HW_DEBUG_PRINTLN("[Fans] No fans in NVS");
    _fanCount = 0;
  }
  SmartMiFan::setHandshakeTtl(_handshakeTtlS);
  SmartMiFan::clear();
  syncEnabledToLibrary();

  if (_fanCount > 0) startSmartConnect();
  initialized = true;
  markTelemetryDirty();
  return true;
}

void FanController::onLibraryEvent(const EventInfo& ev) {
  if (s_fanControllerSelf) s_fanControllerSelf->handleEvent(ev);
}

void FanController::loop() {
  if (!initialized || _maintenanceStopped) return;
  if (_nvsDirty.exchange(false)) saveFansToNVS();
  drainQueue();
  SmartMiFan::update();
  if (_postConnectStep != PostConnectStep::IDLE) updatePostConnectionControl();
  if (_cleanupPending) {
    const unsigned long elapsed = millis() - _cleanupStartMs;
    if ((_cleanupSpeedDone && _cleanupPowerDone) || elapsed >= 4000) {
      SmartMiFan::remove(_cleanupIndex);
      _cleanupPending = false;
      _cleanupIndex = 0xFF;
    }
  }
  updateAutoRecovery();
}

void FanController::stopForMaintenance() {
  if (_maintenanceStopped) return;
  _maintenanceStopped = true;
  if (!initialized) return;
  // No wait for pending commands: the maintenance budget is 15 s in total (§7.4).
  // prepareForSleep() cancels the active waves itself.
  SmartMiFan::prepareForSleep(true);
  clearSeqWaiters();
  HW_DEBUG_PRINTLN("[Fans] Stopped for maintenance");
}

void FanController::discardQueue() {
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux && xSemaphoreTake(_reqMux, pdMS_TO_TICKS(10)) != pdTRUE) return;
#endif
  _reqHead = 0;
  _reqCount = 0;
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux) xSemaphoreGive(_reqMux);
#endif
}

bool FanController::admissionClosed() const {
  return _maintenanceStopped || MaintenanceMode::admissionClosed();
}

void FanController::recountUsed() {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if (_fans[i].used) n++;
  }
  _fanCount = n;
}

int FanController::firstFreePlace() const {
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if (!_fans[i].used) return static_cast<int>(i);
  }
  return -1;
}

bool FanController::addFan(const String& token) {
  if (admissionClosed()) return false;
  if (token.length() != 32) return false;
  if (findFanIndex(token) >= 0) return true;
  if (firstFreePlace() < 0) return false;
  return enqueue(FanReqType::ADD_SLOT, 0, 0, token.c_str());
}

bool FanController::removeFan(const String& token) {
  return removeFanFast(token);
}

bool FanController::removeFanFast(const String& token) {
  if (admissionClosed()) return false;
  const int index = findFanIndex(token);
  if (index < 0) return false;
  return enqueue(FanReqType::REMOVE_CLEANUP, static_cast<uint8_t>(index), 0, token.c_str());
}

bool FanController::updateFanToken(const String& oldToken, const String& newToken) {
  if (admissionClosed()) return false;
  if (newToken.length() != 32) return false;
  const int index = findFanIndex(oldToken);
  if (index < 0) return false;
  if (findFanIndex(newToken) >= 0 && oldToken != newToken) return false;
  return enqueue(FanReqType::UPDATE_SLOT, static_cast<uint8_t>(index), 0, newToken.c_str());
}

bool FanController::setFanControlState(const String& token, bool active) {
  if (admissionClosed()) return false;
  const int nvsIndex = findFanIndex(token);
  if (nvsIndex < 0) return false;
  if (active && _autoRecoveryActive[nvsIndex] && !_autoRecoveryExhausted[nvsIndex]) return false;
  const unsigned long now = millis();
  const unsigned long lastChange = _lastControlStateChangeMs[nvsIndex];
  if (lastChange != 0 && (now - lastChange) < FAN_CONTROL_THROTTLE_MS) return false;
  _lastControlStateChangeMs[nvsIndex] = now;
  return enqueue(FanReqType::SET_ENABLED, static_cast<uint8_t>(nvsIndex), active ? 1 : 0);
}

bool FanController::enqueueSetSpeed(uint8_t index, uint8_t pct) {
  return enqueue(FanReqType::SET_SPEED, index, pct);
}

bool FanController::enqueueSetPower(uint8_t index, bool on) {
  return enqueue(FanReqType::SET_POWER, index, on ? 1 : 0);
}

bool FanController::enqueue(FanReqType type, uint8_t index, uint8_t value, const char* token) {
  if (admissionClosed()) return false;
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux && xSemaphoreTake(_reqMux, pdMS_TO_TICKS(10)) != pdTRUE) return false;
#endif
  bool ok = false;
  if (_reqCount < REQ_DEPTH) {
    const uint8_t tail = static_cast<uint8_t>((_reqHead + _reqCount) % REQ_DEPTH);
    FanReq& r = _req[tail];
    r.type = type;
    r.index = index;
    r.value = value;
    strncpy(r.token, token ? token : "", sizeof(r.token) - 1);
    r.token[sizeof(r.token) - 1] = '\0';
    _reqCount++;
    ok = true;
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux) xSemaphoreGive(_reqMux);
#endif
  return ok;
}

void FanController::drainQueue() {
  FanReq local[REQ_DEPTH];
  uint8_t n = 0;
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux && xSemaphoreTake(_reqMux, 0) != pdTRUE) return;
#endif
  while (_reqCount > 0 && n < REQ_DEPTH) {
    local[n++] = _req[_reqHead];
    _reqHead = static_cast<uint8_t>((_reqHead + 1) % REQ_DEPTH);
    _reqCount--;
  }
#if defined(ARDUINO_ARCH_ESP32)
  if (_reqMux) xSemaphoreGive(_reqMux);
#endif

  for (uint8_t i = 0; i < n; ++i) {
    const FanReq& r = local[i];
    switch (r.type) {
      case FanReqType::SET_ENABLED:
        if (!_fans[r.index].used) break;
        _fans[r.index].enabled = r.value != 0;
        if (!r.value) {
          _autoRecoveryActive[r.index] = false;
          _autoRecoveryExhausted[r.index] = false;
          _autoRecoveryAttempts[r.index] = 0;
          _lastAutoRecoveryAttemptMs[r.index] = 0;
          _lastRecoveryDiscoveryAttemptMs[r.index] = 0;
          _unhealthyStreak[r.index] = 0;
        }
        _nvsDirty = true;
        markTelemetryDirty();
        SmartMiFan::setEnabled(r.index, r.value != 0);
        if (!r.value) {
          SmartMiFan::setPower(r.index, false);
        } else {
          SmartMiFanInfo info;
          if (!SmartMiFan::info(r.index, info) || !info.ready) {
            if (_autoRecoveryExhausted[r.index]) {
              if (!SmartMiFan::connecting() && !SmartMiFan::discovering()) {
                enqueue(FanReqType::SMART_CONNECT, r.index, 0);
              }
            } else {
              SmartMiFan::handshake(r.index);
            }
          }
        }
        break;
      case FanReqType::SET_SPEED:
        SmartMiFan::setSpeed(r.index, r.value);
        break;
      case FanReqType::SET_POWER:
        SmartMiFan::setPower(r.index, r.value != 0);
        break;
      case FanReqType::REMOVE_CLEANUP:
        if (!_fans[r.index].used || !_fans[r.index].tokenEquals(r.token)) break;
        _fans[r.index] = FanConfig();
        recountUsed();
        _nvsDirty = true;
        markTelemetryDirty();
        _cleanupIndex = r.index;
        _cleanupStartMs = millis();
        _cleanupPending = true;
        _cleanupSpeedDone = false;
        _cleanupPowerDone = false;
        SmartMiFan::setSpeed(r.index, 1);
        SmartMiFan::setPower(r.index, false);
        break;
      case FanReqType::ADD_SLOT: {
        if (findFanIndex(String(r.token)) >= 0) break;
        const int place = firstFreePlace();
        if (place < 0) {
          HW_ERROR_PRINTLN("[Fans] Add dropped: no free place");
          break;
        }
        FanConfig& fan = _fans[place];
        fan.setToken(r.token);
        fan.ip = IPAddress(0, 0, 0, 0);
        fan.model[0] = '\0';
        fan.enabled = true;
        fan.used = true;
        recountUsed();
        _nvsDirty = true;
        markTelemetryDirty();
        const uint8_t slot = static_cast<uint8_t>(place);
        SmartMiFan::add(slot, "0.0.0.0", fan.token, nullptr);
        SmartMiFan::setEnabled(slot, true);
        startDiscoveryForIndex(slot, false);
        break;
      }
      case FanReqType::UPDATE_SLOT:
        if (!_fans[r.index].used) break;
        _fans[r.index].setToken(r.token);
        _fans[r.index].ip = IPAddress(0, 0, 0, 0);
        _fans[r.index].model[0] = '\0';
        _nvsDirty = true;
        markTelemetryDirty();
        SmartMiFan::remove(r.index);
        SmartMiFan::add(r.index, "0.0.0.0", _fans[r.index].token, nullptr);
        SmartMiFan::setEnabled(r.index, _fans[r.index].enabled);
        startDiscoveryForIndex(r.index, false);
        break;
      case FanReqType::SMART_CONNECT:
        startSmartConnect();
        break;
    }
  }
}

uint8_t FanController::getConnectedFanCount() const {
  uint8_t n = 0;
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if (!_fans[i].used) continue;
    SmartMiFanInfo info;
    if (SmartMiFan::info(i, info) && info.ready) n++;
  }
  return n;
}

bool FanController::getFanConfig(uint8_t index, FanConfig& config) const {
  if (index >= MAX_FANS || !_fans[index].used) return false;
  config = _fans[index];
  return true;
}

bool FanController::getFanConfigByToken(const String& token, FanConfig& config) const {
  const int index = findFanIndex(token);
  if (index < 0) return false;
  config = _fans[index];
  return true;
}

bool FanController::getDiscoveredFans(SmartMiFanInfo* fans, size_t& count) const {
  if (!fans) return false;
  count = 0;
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    SmartMiFanInfo info;
    if (SmartMiFan::info(i, info)) fans[count++] = info;
  }
  return true;
}

void FanController::markTelemetryDirty() {
#if HOMEWIND_ENABLE_WEBSOCKET
  if (_telemetry) _telemetry->markDirty(TelemetryFrameType::FANS_SNAPSHOT);
#endif
#if HW_ENABLE_DISPLAY
  if (g_displayManager) g_displayManager->markFansDirty();
#endif
}

uint8_t FanController::getFanRecoveryUiState(uint8_t index) const {
  if (index >= MAX_FANS) return 0;
  if (_autoRecoveryExhausted[index]) return 2;
  if (_autoRecoveryActive[index]) return 1;
  return 0;
}

bool FanController::isFanActiveForHr(uint8_t index) const {
  return (SmartMiFan::participating() & static_cast<FanMask>(1u << index)) != 0;
}

bool FanController::isPostConnectDone() const {
  if (_postConnectStep != PostConnectStep::IDLE) return false;
  return _postConnectCompleted || _postConnectStepStartTime > 0;
}

bool FanController::setPostConnectSpeed(uint8_t speed, bool persist) {
  if (speed < 1) speed = 1;
  if (speed > HW_FAN_MAX_SPEED_PERCENT) speed = HW_FAN_MAX_SPEED_PERCENT;
  _postConnectSpeed = speed;
  return persist ? saveFansToNVS() : true;
}

bool FanController::setHandshakeTtlS(uint32_t seconds, bool persist) {
  if (seconds > 255) seconds = 255;
  _handshakeTtlS = seconds;
  SmartMiFan::setHandshakeTtl(_handshakeTtlS);
  return persist ? saveFansToNVS() : true;
}

bool FanController::loadFansFromNVS() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, true)) return false;
  NVSFanData data;
  const size_t len = prefs.getBytesLength(NVS_KEY_DATA);
  if (len != sizeof(NVSFanData)) {
    prefs.end();
    _handshakeTtlS = HW_FAN_HANDSHAKE_TTL_S;
    return false;
  }
  prefs.getBytes(NVS_KEY_DATA, &data, sizeof(data));
  prefs.end();

  const uint16_t storedCrc = data.crc16;
  data.crc16 = 0;
  const uint16_t calcedCrc = Crc16::calcCRC16(reinterpret_cast<uint8_t*>(&data),
                                              sizeof(data) - sizeof(data.crc16));
  if (storedCrc != calcedCrc) return false;
  if (data.version != 2 && data.version != NVS_DATA_VERSION) return false;
  if (data.fanCount > MAX_FANS) return false;

  _postConnectSpeed = data.postConnectSpeed;
  if (_postConnectSpeed < 1) _postConnectSpeed = 1;
  if (_postConnectSpeed > HW_FAN_MAX_SPEED_PERCENT) _postConnectSpeed = HW_FAN_MAX_SPEED_PERCENT;
  _handshakeTtlS = (data.version >= 3) ? data.handshakeTtlS : HW_FAN_HANDSHAKE_TTL_S;

  memset(_fans, 0, sizeof(_fans));
  if (data.version == 2) {
    for (uint8_t i = 0; i < data.fanCount; ++i) {
      const NVSFanEntry& entry = data.fans[i];
      FanConfig& cfg = _fans[i];
      cfg.setToken(entry.token);
      cfg.ip = IPAddress(entry.ip[0], entry.ip[1], entry.ip[2], entry.ip[3]);
      strncpy(cfg.model, entry.model, sizeof(cfg.model) - 1);
      cfg.enabled = (entry.enabled != 0);
      cfg.used = (cfg.token[0] != '\0');
    }
    recountUsed();
    saveFansToNVS();
    return true;
  }

  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    const NVSFanEntry& entry = data.fans[i];
    FanConfig& cfg = _fans[i];
    cfg.setToken(entry.token);
    cfg.ip = IPAddress(entry.ip[0], entry.ip[1], entry.ip[2], entry.ip[3]);
    strncpy(cfg.model, entry.model, sizeof(cfg.model) - 1);
    cfg.enabled = (entry.enabled != 0);
    cfg.used = (entry.used != 0) && (cfg.token[0] != '\0');
  }
  recountUsed();
  return true;
}

bool FanController::saveFansToNVS() {
  Preferences prefs;
  if (!prefs.begin(NVS_NAMESPACE, false)) return false;
  NVSFanData data;
  memset(&data, 0, sizeof(data));
  data.version = NVS_DATA_VERSION;
  data.fanCount = _fanCount;
  data.postConnectSpeed = _postConnectSpeed;
  data.handshakeTtlS = static_cast<uint8_t>(_handshakeTtlS);
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    const FanConfig& cfg = _fans[i];
    NVSFanEntry& entry = data.fans[i];
    strncpy(entry.token, cfg.token, sizeof(entry.token) - 1);
    entry.ip[0] = cfg.ip[0];
    entry.ip[1] = cfg.ip[1];
    entry.ip[2] = cfg.ip[2];
    entry.ip[3] = cfg.ip[3];
    strncpy(entry.model, cfg.model, sizeof(entry.model) - 1);
    entry.enabled = cfg.enabled ? 1 : 0;
    entry.used = cfg.used ? 1 : 0;
  }
  data.crc16 = Crc16::calcCRC16(reinterpret_cast<uint8_t*>(&data),
                                sizeof(data) - sizeof(data.crc16));
  const size_t written = prefs.putBytes(NVS_KEY_DATA, &data, sizeof(data));
  prefs.end();
  return written == sizeof(data);
}

int FanController::findFanIndex(const String& token) const {
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if (_fans[i].used && _fans[i].tokenEquals(token)) return static_cast<int>(i);
  }
  return -1;
}

void FanController::syncEnabledToLibrary() {
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if (!_fans[i].used) continue;
    char ipBuf[16];
    ipToBuffer(_fans[i].ip, ipBuf, sizeof(ipBuf));
    SmartMiFan::add(i, ipBuf, _fans[i].token, _fans[i].model[0] ? _fans[i].model : nullptr);
    SmartMiFan::setEnabled(i, _fans[i].enabled);
    SmartMiFan::setSoftActive(i, _softActive[i]);
  }
}

void FanController::startDiscoveryForIndex(uint8_t index, bool recovery) {
  if (index >= MAX_FANS || !_fans[index].used) return;
  if (SmartMiFan::connecting()) return;
  _discoveryIsRecovery = recovery;
  _discoveryRecoveryNvsIndex = recovery ? static_cast<int>(index) : -1;
  SmartMiFan::discover(static_cast<FanMask>(1u << index), HW_FAN_DISCOVERY_TIMEOUT_MS);
}

void FanController::startSmartConnect() {
  if (_fanCount == 0) return;
  if (SmartMiFan::commandPending()) SmartMiFan::cancel();
  SmartMiFan::clear();
  syncEnabledToLibrary();
  _connectInProgress = SmartMiFan::connect(HW_FAN_SMART_CONNECT_TIMEOUT_MS);
}

void FanController::startPostConnectionControl() {
  if (_postConnectStep != PostConnectStep::IDLE) return;
  _postConnectStep = PostConnectStep::POWER_ON;
  _postConnectStepStartTime = millis();
  _postConnectSeq = 0;
}

void FanController::updatePostConnectionControl() {
  if (_postConnectStep == PostConnectStep::IDLE) return;
  const unsigned long elapsed = millis() - _postConnectStepStartTime;
  switch (_postConnectStep) {
    case PostConnectStep::POWER_ON: {
      const CmdAccept acc = SmartMiFan::setPowerAll(true, Scope::ALL);
      if (acc.result == CmdResult::REJECTED) {
        _postConnectStep = PostConnectStep::DONE;
        break;
      }
      _postConnectSeq = acc.seq;
      _postConnectStep = PostConnectStep::WAIT_STABILIZE;
      break;
    }
    case PostConnectStep::WAIT_STABILIZE:
      if (_postConnectSeq == 0 && elapsed >= 500) _postConnectStep = PostConnectStep::SET_SPEED;
      break;
    case PostConnectStep::SET_SPEED: {
      const CmdAccept acc = SmartMiFan::setSpeedAll(_postConnectSpeed, Scope::ALL);
      _postConnectSeq = (acc.result == CmdResult::REJECTED) ? 0 : acc.seq;
      _postConnectStep = PostConnectStep::WAIT_RUN;
      _postConnectStepStartTime = millis();
      break;
    }
    case PostConnectStep::WAIT_RUN:
      if (_postConnectSeq == 0 && elapsed >= 3000) _postConnectStep = PostConnectStep::POWER_OFF;
      break;
    case PostConnectStep::POWER_OFF: {
      if (_postConnectSeq != 0) break;
      const CmdAccept acc = SmartMiFan::setPowerAll(false, Scope::ALL);
      if (acc.result == CmdResult::REJECTED) {
        _postConnectStep = PostConnectStep::DONE;
        break;
      }
      _postConnectSeq = acc.seq;
      break;
    }
    case PostConnectStep::DONE:
      HeapDiagnostics::hwHeapDiagCapturePostCtrlDone();
      markTelemetryDirty();
#if HW_ENABLE_BLE
      if (g_fanHrAdapter) g_fanHrAdapter->onPostConnectControlComplete();
#endif
      _postConnectCompleted = true;
      _postConnectStep = PostConnectStep::IDLE;
      _postConnectSeq = 0;
      break;
    case PostConnectStep::IDLE:
      break;
  }
}

void FanController::clearSeqWaiters() {
  _postConnectSeq = 0;
#if HW_ENABLE_BLE
  if (g_fanHrAdapter) g_fanHrAdapter->clearCommandWaiters();
#endif
  _cleanupPending = false;
}

void FanController::handleEvent(const EventInfo& ev) {
  switch (ev.type) {
    case EventType::CONNECT_DONE:
      _connectInProgress = false;
      onConnectOrDiscoveryDone(ev);
      if (!_discoveryIsRecovery) startPostConnectionControl();
      else {
#if HW_ENABLE_BLE
        if (g_fanHrAdapter) g_fanHrAdapter->onPostConnectControlComplete();
#endif
      }
      markTelemetryDirty();
      break;
    case EventType::DISCOVERY_DONE:
      if (ev.restarted) break;
      onConnectOrDiscoveryDone(ev);
      if (_connectInProgress) break;
      if (ev.anyOk && !_discoveryIsRecovery) startPostConnectionControl();
      if (_discoveryIsRecovery && _discoveryRecoveryNvsIndex >= 0) {
        const int idx = _discoveryRecoveryNvsIndex;
        if (ev.okMask & static_cast<FanMask>(1u << idx)) {
          _autoRecoveryActive[idx] = false;
          _autoRecoveryExhausted[idx] = false;
          _autoRecoveryAttempts[idx] = 0;
          _unhealthyStreak[idx] = 0;
        } else {
          _autoRecoveryActive[idx] = true;
          if (_autoRecoveryAttempts[idx] >= HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS) {
            _autoRecoveryExhausted[idx] = true;
            _autoRecoveryActive[idx] = false;
          }
        }
      }
      _discoveryIsRecovery = false;
      _discoveryRecoveryNvsIndex = -1;
      markTelemetryDirty();
      break;
    case EventType::FAN_READY:
      if (ev.fanIndex >= 0 && ev.fanIndex < MAX_FANS) {
        _autoRecoveryActive[ev.fanIndex] = false;
        _autoRecoveryExhausted[ev.fanIndex] = false;
        _autoRecoveryAttempts[ev.fanIndex] = 0;
        _unhealthyStreak[ev.fanIndex] = 0;
        _softActive[ev.fanIndex] = false;
        SmartMiFan::setSoftActive(static_cast<uint8_t>(ev.fanIndex), false);
      }
      markTelemetryDirty();
      break;
    case EventType::FAN_ERROR:
      markTelemetryDirty();
      break;
    case EventType::FAN_INFO:
      if (ev.fanIndex >= 0 && ev.fanIndex < MAX_FANS) {
        SmartMiFanInfo info;
        if (SmartMiFan::info(static_cast<uint8_t>(ev.fanIndex), info)) {
          _fans[ev.fanIndex].ip = info.ip;
          strncpy(_fans[ev.fanIndex].model, info.model, sizeof(_fans[ev.fanIndex].model) - 1);
          saveFansToNVS();
        }
      }
      markTelemetryDirty();
      break;
    case EventType::CMD_DONE:
      if (_cleanupPending && ev.fanIndex == static_cast<int8_t>(_cleanupIndex)) {
        if (ev.kind == CmdKind::SPEED) _cleanupSpeedDone = true;
        if (ev.kind == CmdKind::POWER) _cleanupPowerDone = true;
      }
      break;
    case EventType::GROUP_DONE:
      if (_postConnectSeq != 0 && ev.seq == _postConnectSeq) {
        if (!ev.allOk && (_postConnectStep == PostConnectStep::WAIT_STABILIZE ||
                          _postConnectStep == PostConnectStep::POWER_ON)) {
          _postConnectStep = PostConnectStep::DONE;
          _postConnectSeq = 0;
          break;
        }
        _postConnectSeq = 0;
        if (_postConnectStep == PostConnectStep::WAIT_STABILIZE) {
          _postConnectStepStartTime = millis();
        } else if (_postConnectStep == PostConnectStep::WAIT_RUN) {
          /* keep waiting for 3s from start */
        } else if (_postConnectStep == PostConnectStep::POWER_OFF) {
          _postConnectStep = PostConnectStep::DONE;
        }
      }
#if HW_ENABLE_BLE
      if (g_fanHrAdapter) g_fanHrAdapter->onGroupDone(ev);
#endif
      break;
    case EventType::CANCELLED:
      clearSeqWaiters();
      _connectInProgress = false;
      break;
    default:
      break;
  }
}

void FanController::onConnectOrDiscoveryDone(const EventInfo& ev) {
  bool dirty = false;
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if ((ev.okMask & static_cast<FanMask>(1u << i)) == 0) continue;
    SmartMiFanInfo info;
    if (!SmartMiFan::info(i, info) || !_fans[i].used) continue;
    if (_fans[i].ip != info.ip || strcmp(_fans[i].model, info.model) != 0) {
      _fans[i].ip = info.ip;
      strncpy(_fans[i].model, info.model, sizeof(_fans[i].model) - 1);
      dirty = true;
    }
    SmartMiFan::setEnabled(i, _fans[i].enabled);
  }
  if (dirty) saveFansToNVS();
}

void FanController::updateAutoRecovery() {
  const unsigned long now = millis();
  for (uint8_t i = 0; i < MAX_FANS; ++i) {
    if (!_fans[i].used || !_fans[i].enabled) continue;
    SmartMiFanInfo info;
    if (!SmartMiFan::info(i, info)) continue;
    const bool unhealthy = !info.ready || info.lastError != MiioErr::OK;
    if (!unhealthy) {
      _unhealthyStreak[i] = 0;
      _softActive[i] = false;
      SmartMiFan::setSoftActive(i, false);
      continue;
    }
    if (_lastUnhealthyCheckMs[i] != 0 &&
        now - _lastUnhealthyCheckMs[i] < HW_FAN_AUTO_RECOVERY_CHECK_INTERVAL_MS) {
      continue;
    }
    _lastUnhealthyCheckMs[i] = now;
    if (_unhealthyStreak[i] < 255) _unhealthyStreak[i]++;
    if (_unhealthyStreak[i] >= 1 && _unhealthyStreak[i] < HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS) {
      _softActive[i] = true;
      SmartMiFan::setSoftActive(i, true);
    }
    if (_unhealthyStreak[i] < HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS) continue;
    if (_autoRecoveryExhausted[i]) continue;
    if (_lastAutoRecoveryAttemptMs[i] != 0 &&
        now - _lastAutoRecoveryAttemptMs[i] < FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS) {
      continue;
    }
    if (_autoRecoveryAttempts[i] >= HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS) {
      _autoRecoveryExhausted[i] = true;
      _autoRecoveryActive[i] = false;
      continue;
    }
    if (SmartMiFan::connecting()) continue;
    _lastAutoRecoveryAttemptMs[i] = now;
    _autoRecoveryAttempts[i]++;
    _autoRecoveryActive[i] = true;
    startDiscoveryForIndex(i, true);
  }
}

#endif // HW_ENABLE_FANS
