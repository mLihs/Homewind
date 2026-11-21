/**
 * @file DisplayManager.cpp
 * @brief Display integration - bridges Homewind data to HomeWindWSAmoled
 */

#include "../app/Config.h"

#if HW_ENABLE_DISPLAY

#include <cstring>
#include "DisplayManager.h"
#include "../core/DebugLog.h"

#if HW_ENABLE_BLE
#include <BikeSensor.h>
#include "BLERelayManager.h"
extern BLERelayManager* g_bleRelayManager;
#endif

#if HW_ENABLE_FANS
#include "FanController.h"
extern FanController* g_fanController;
#include <SmartMiFanAsync.h>
#endif

#if HW_ENABLE_FANS && HW_ENABLE_BLE
#include "FanHRAdapter.h"
extern FanHRAdapter* g_fanHrAdapter;
#endif

#if HW_ENABLE_NVS
#include "HeartRateSettings.h"
extern HeartRateSettings* g_heartRateSettings;
#endif

#if HW_ENABLE_FAN_POWER_MODE
#include "FanModeSettings.h"
extern FanModeSettings* g_fanModeSettings;
#endif

#include <HomeWindWSAmoled.h>

static_assert(HOMEWIND_WS_AMOLED_VERSION_MAJOR > 1 ||
                  (HOMEWIND_WS_AMOLED_VERSION_MAJOR == 1 && HOMEWIND_WS_AMOLED_VERSION_MINOR >= 7),
              "HomeWindWSAmoled >= 1.7.0 required (rows, badges, fan control)");

namespace {
constexpr uint32_t kPollMs = 500;   // badges, trainer, fan control
constexpr uint32_t kWakeMs = 1000;  // homewind_powersave_wake() at most this often
constexpr uint32_t kAppLostMs = 5UL * 60UL * 1000UL;  // orange APP badge after a dropped link

row_state_t toRowState(hw::BikeRowState s) {
  switch (s) {
    case hw::BikeRowState::Active:       return ROW_ACTIVE;
    case hw::BikeRowState::Disconnected: return ROW_DISCONNECTED;
    default:                             return ROW_HIDDEN;
  }
}

void copyName(char (&dst)[64], const char* src) {
  strncpy(dst, src ? src : "", sizeof(dst) - 1);
  dst[sizeof(dst) - 1] = '\0';
}
}  // namespace

DisplayManager* g_displayManager = nullptr;

DisplayManager::DisplayManager()
  : _initialized(false)
  , _lastHR(0)
  , _lastCadence(0)
  , _lastCadenceValid(false)
  , _lastPower(0)
  , _lastPowerValid(false)
  , _pwrCadenceSeen(false)
  , _fansDirty(false)
  , _hrValueDirty(false)
  , _bikeValueDirty(false)
  , _hrStateDirty(false)
  , _bikeStateDirty(false)
  , _urlDirty(false)
  , _badgesDirty(false)
  , _fanControlDirty(false)
  , _activity(false)
  , _shiftRequest(0)
  , _modeRequest(false)
  , _hrStateConfigured(false)
  , _cachedHRState(HR_NOT_CONFIGURATED)
  , _powerRow{hw::BikeRowState::Hidden, hw::BikeRowSource::None}
  , _cadenceRow{hw::BikeRowState::Hidden, hw::BikeRowSource::None}
  , _appBadge(BADGE_GREY)
  , _trainerBadge(BADGE_HIDDEN)
  , _appLostMs(0)
  , _trainerWasReady(false)
  , _trainerMirror(false)
  , _trainerReady(false)
  , _fanMode(FAN_MODE_PURE)
  , _fanOn(false)
  , _fanPct(0)
  , _lastPollMs(0)
  , _lastWakeMs(0) {
  _cachedHRName[0] = '\0';
  _powerName[0] = '\0';
  _cadenceName[0] = '\0';
  _cachedUrl[0] = '\0';
}

bool DisplayManager::begin() {
  if (_initialized) {
    return true;
  }

  homewind_init();
  homewind_set_fan_control_callbacks(staticHrShift, staticFanMode);

  _initialized = true;
  HW_DEBUG_PRINTF("[Display] Initialized (board %s, TE pulses %lu)\n",
                  homewind_get_board_revision_name(),
                  static_cast<unsigned long>(homewind_get_te_pulse_count()));
  return true;
}

const char* DisplayManager::boardRevision() const {
  return _initialized ? homewind_get_board_revision_name() : nullptr;
}

uint32_t DisplayManager::tePulseCount() const {
  return homewind_get_te_pulse_count();
}

void DisplayManager::loop() {
  if (!_initialized) return;

  const uint32_t now = millis();
  applyTouchRequests();
  if (now - _lastPollMs >= kPollMs) {
    _lastPollMs = now;
    pollStatus();
  }
  bool wake = false;
  if (_activity && now - _lastWakeMs >= kWakeMs) {
    _activity = false;
    _lastWakeMs = now;
    wake = true;
  }

  if (!wake && !_hrValueDirty && !_bikeValueDirty && !_hrStateDirty && !_bikeStateDirty &&
      !_fansDirty && !_urlDirty && !_badgesDirty && !_fanControlDirty) {
    return;
  }

  if (!lcd_lvgl_lock(50)) {
    if (wake) _activity = true;
    return;  /* Lock timeout – retry next loop iteration */
  }

  if (wake) {
    homewind_powersave_wake();
  }
  if (_hrValueDirty.exchange(false)) {
    homewind_set_hr_value(_lastHR);
  }
  if (_hrStateDirty.exchange(false)) {
    if (_hrStateConfigured) {
      homewind_set_hr(_cachedHRState, _cachedHRName, _lastHR);
    } else {
      homewind_set_hr_state(HR_NOT_CONFIGURATED);
    }
  }
  const bool bikeState = _bikeStateDirty.exchange(false);
  const bool bikeValue = _bikeValueDirty.exchange(false);
  if (bikeState || bikeValue) {
    homewind_set_power_row(toRowState(_powerRow.state), bikeState ? _powerName : nullptr,
                           hw::bikeRowValue(_lastPowerValid, _lastPower));
    const int16_t rpm = _lastCadence > 0x7FFF ? 0x7FFF : static_cast<int16_t>(_lastCadence);
    homewind_set_cadence_row(toRowState(_cadenceRow.state), bikeState ? _cadenceName : nullptr,
                             hw::bikeRowValue(_lastCadenceValid, rpm));
  }
  if (_badgesDirty.exchange(false)) {
    homewind_set_badges(_appBadge, _trainerBadge);
  }
  if (_fansDirty.exchange(false)) {
    updateFans();
  }
  if (_fanControlDirty.exchange(false)) {
    homewind_set_fan_control(_fanMode, _fanOn, _fanPct);
  }
  if (_urlDirty.exchange(false)) {
    homewind_set_qr_code_url(_cachedUrl);
  }

  lcd_lvgl_unlock();
}

void DisplayManager::setHeartRate(uint16_t bpm) {
  if (!_initialized) return;
  _lastHR = bpm;
  _hrValueDirty = true;
  if (bpm > 0) _activity = true;
}

void DisplayManager::updateHRState() {
  if (!_initialized) return;
  _activity = true;

#if HW_ENABLE_BLE
  if (!g_bleRelayManager) {
    _hrStateConfigured = false;
  } else {
    HwSensorInfo sensors[BLERelayManager::MAX_SENSORS];
    uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, BLERelayManager::MAX_SENSORS);

    for (uint8_t i = 0; i < count; i++) {
      if (strcmp(sensors[i].type, "HR") == 0) {
        _hrStateConfigured = true;
        _cachedHRState = sensors[i].connected ? HR_STATE_ACTIVE : HR_STATE_INACTIVE;
        if (!sensors[i].connected) _lastHR = 0;
        copyName(_cachedHRName, sensors[i].name);
        _hrStateDirty = true;
        return;
      }
    }
    _hrStateConfigured = false;
  }
#else
  _hrStateConfigured = false;
#endif
  _hrStateDirty = true;
}

void DisplayManager::setBikeMetrics(const hw::BikeMetrics& m) {
  if (!_initialized) return;
  _lastCadence = m.cadenceRpm;
  _lastCadenceValid = (m.flags & hw::kBikeCadenceValid) != 0;
  _lastPower = m.powerW;
  _lastPowerValid = (m.flags & hw::kBikePowerValid) != 0;
  _bikeValueDirty = true;
  if ((_lastPowerValid && _lastPower > 0) || (_lastCadenceValid && _lastCadence > 0)) {
    _activity = true;
  }
  if ((m.flags & hw::kBikeCadenceFromPower) && !_pwrCadenceSeen) {
    _pwrCadenceSeen = true;
    updateCSCState();
  }
}

void DisplayManager::updateCSCState() {
  if (!_initialized) return;
  _activity = true;

  hw::BikeSources s;
  const char* cscName = nullptr;
  const char* pwrName = nullptr;
  const char* trainerName = "Trainer";
#if HW_ENABLE_BLE
  HwSensorInfo sensors[BLERelayManager::MAX_SENSORS];
#if HW_ENABLE_TRAINER_MIRROR
  BLERelayManager::TrainerStatus trainer;
#endif
  if (g_bleRelayManager) {
    uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, BLERelayManager::MAX_SENSORS);
    for (uint8_t i = 0; i < count; i++) {
      if (!cscName && strcmp(sensors[i].type, "CSC") == 0) {
        cscName = sensors[i].name;
        s.cscConfigured = true;
        s.cscConnected = sensors[i].connected;
      }
      if (!pwrName && strcmp(sensors[i].type, "PWR") == 0) {
        pwrName = sensors[i].name;
        s.pwrConfigured = true;
        s.pwrConnected = sensors[i].connected;
      }
    }
#if HW_ENABLE_TRAINER_MIRROR
    g_bleRelayManager->trainerStatus(trainer);
    s.trainerConfigured = trainer.mirror;
    s.trainerReady = trainer.mirror && trainer.state == static_cast<uint8_t>(BikeSensor::TrainerState::READY);
    if (trainer.name[0]) trainerName = trainer.name;
#endif
  }
#endif
  s.pwrCadence = _pwrCadenceSeen;

  _powerRow = hw::selectPowerRow(s);
  _cadenceRow = hw::selectCadenceRow(s);
  const auto nameOf = [&](hw::BikeRowSource src) -> const char* {
    switch (src) {
      case hw::BikeRowSource::Csc:     return cscName;
      case hw::BikeRowSource::Power:   return pwrName;
      case hw::BikeRowSource::Trainer: return trainerName;
      default:                         return "";
    }
  };
  copyName(_powerName, nameOf(_powerRow.source));
  copyName(_cadenceName, nameOf(_cadenceRow.source));
  _bikeStateDirty = true;
}

void DisplayManager::markFansDirty() {
  if (!_initialized) return;
  _fansDirty = true;
}

void DisplayManager::noteUserActivity() {
  if (!_initialized) return;
  _activity = true;
}

void DisplayManager::updateFans() {
  if (!_initialized) return;

#if HW_ENABLE_FANS
  if (!g_fanController) return;

  for (uint8_t i = 0; i < FanController::MAX_FANS; i++) {
    FanConfig cfg;
    if (!g_fanController->getFanConfig(i, cfg)) {
      homewind_set_fan_state(i, FAN_STATE_NOT_CONFIGURATED);
      continue;
    }

    SmartMiFanInfo info;
    fan_state_t state = FAN_STATE_INACTIVE;
    bool isOn = false;
    if (SmartMiFan::info(i, info)) {
      const FanParticipationState p = SmartMiFan::participation(i);
      state = (p == FanParticipationState::ERROR || !info.ready) ? FAN_STATE_ERROR : FAN_STATE_ACTIVE;
      isOn = (p == FanParticipationState::ACTIVE);
    }
    homewind_set_fan(i, state, isOn);
  }
#endif
}

// Badges (concept table): APP green while an app is connected; when the last one
// leaves grey if it ended the link itself, else orange for kAppLostMs, then grey.
// Trainer green while mirrored and ready, orange after it was ready,
// grey before, hidden while paused, not set up or not mirrored this boot.
void DisplayManager::pollStatus() {
  badge_state_t app = _appBadge;
  badge_state_t trainerBadge = _trainerBadge;
#if HW_ENABLE_BLE
  if (BikeSensor::centralCount() > 0) {
    _activity = true;  // the panel stays on while an app is connected
    app = BADGE_GREEN;
  } else if (_appBadge == BADGE_GREEN) {
    // Same loop task as BLERelayManager::loop(), which ran first: the reason is current
    const bool clean = g_bleRelayManager && g_bleRelayManager->appLeftCleanly();
    app = clean ? BADGE_GREY : BADGE_ORANGE;
    _appLostMs = millis();
  } else if (_appBadge == BADGE_ORANGE && millis() - _appLostMs >= kAppLostMs) {
    app = BADGE_GREY;
  }
#if HW_ENABLE_TRAINER_MIRROR
  if (g_bleRelayManager) {
    BLERelayManager::TrainerStatus t;
    g_bleRelayManager->trainerStatus(t);
    const bool ready = t.mirror && t.state == static_cast<uint8_t>(BikeSensor::TrainerState::READY);
    if (ready) _trainerWasReady = true;
    if (!t.enabled || !t.mirror) {
      trainerBadge = BADGE_HIDDEN;
    } else {
      trainerBadge = ready ? BADGE_GREEN : (_trainerWasReady ? BADGE_ORANGE : BADGE_GREY);
    }
    if (t.mirror != _trainerMirror || ready != _trainerReady) {
      _trainerMirror = t.mirror;
      _trainerReady = ready;
      updateCSCState();  // the rows follow the trainer
    }
  }
#endif
#endif
  if (app != _appBadge || trainerBadge != _trainerBadge) {
    _appBadge = app;
    _trainerBadge = trainerBadge;
    _badgesDirty = true;
  }

  fan_mode_t mode = FAN_MODE_PURE;
  bool on = false;
  uint8_t pct = 0;
#if HW_ENABLE_FAN_POWER_MODE
  if (g_fanHrAdapter) {
    const hw::FanControlStatus st = g_fanHrAdapter->currentStatus();
    mode = static_cast<fan_mode_t>(st.effective);
    on = st.targetOn;
    pct = st.targetPct;
  }
#elif HW_ENABLE_FANS && HW_ENABLE_BLE
  if (g_fanHrAdapter) {
    const FanHRStatus st = g_fanHrAdapter->getStatus();
    on = st.globalTargetPower;
    pct = st.globalTargetSpeedPct;
  }
#endif
  if (mode != _fanMode || on != _fanOn || pct != _fanPct) {
    _fanMode = mode;
    _fanOn = on;
    _fanPct = pct;
    _fanControlDirty = true;
  }
}

// Plus/minus one HR step per tap; the mode cycles Pure, Smart, Hybrid and skips
// Hybrid while its preconditions are not met (same check as the web handler).
void DisplayManager::applyTouchRequests() {
  int8_t steps = _shiftRequest.exchange(0);
#if HW_ENABLE_NVS
  for (; steps != 0 && g_heartRateSettings; steps += steps > 0 ? -1 : 1) {
    uint16_t lo = 0;
    uint16_t hi = 0;
    g_heartRateSettings->requestShift(steps > 0 ? 1 : -1, lo, hi);
  }
#else
  (void)steps;
#endif
  if (!_modeRequest.exchange(false)) return;
#if HW_ENABLE_FAN_POWER_MODE
  if (!g_fanModeSettings) return;
  hw::FanMode next = hw::FanMode::Pure;
  switch (g_fanModeSettings->mode()) {
    case hw::FanMode::Pure:  next = hw::FanMode::Smart; break;
    case hw::FanMode::Smart: next = hw::FanMode::Hybrid; break;
    default:                 next = hw::FanMode::Pure; break;
  }
  if (next == hw::FanMode::Hybrid && FanModeSettings::hybridBlockReason()) {
    next = hw::FanMode::Pure;
  }
  g_fanModeSettings->request(true, static_cast<uint8_t>(next), false, 0, false, 0);
  _activity = true;
#endif
}

// LVGL task (lock held): only queue, the loop task applies.
void DisplayManager::staticHrShift(int8_t steps) {
  if (g_displayManager) {
    g_displayManager->_shiftRequest.fetch_add(steps);
  }
}

void DisplayManager::staticFanMode() {
  if (g_displayManager) {
    g_displayManager->_modeRequest = true;
  }
}

void DisplayManager::setUrl(const char* url) {
  if (!_initialized || !url) return;
  strncpy(_cachedUrl, url, sizeof(_cachedUrl) - 1);
  _cachedUrl[sizeof(_cachedUrl) - 1] = '\0';
  _urlDirty = true;
}

void DisplayManager::showAPScreen() {
  if (!_initialized) return;
  if (!lcd_lvgl_lock(100)) return;
  homewind_show_ap_screen();
  powersave_lock();
  lcd_lvgl_unlock();
}

void DisplayManager::showMainScreen() {
  if (!_initialized) return;
  if (!lcd_lvgl_lock(100)) return;
  homewind_show_main_screen();
  powersave_unlock();
  lcd_lvgl_unlock();
}

#endif // HW_ENABLE_DISPLAY
