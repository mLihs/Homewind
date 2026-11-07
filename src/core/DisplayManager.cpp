/**
 * @file DisplayManager.cpp
 * @brief Display integration - bridges Homewind data to HomeWindWSAmoled
 */

#include "../app/Config.h"

#if HW_ENABLE_DISPLAY

#include <cstring>
#include "DisplayManager.h"
#include "BikeCard.h"
#include "../core/DebugLog.h"

#if HW_ENABLE_BLE
#include "BLERelayManager.h"
extern BLERelayManager* g_bleRelayManager;
#endif

#if HW_ENABLE_FANS
#include "FanController.h"
extern FanController* g_fanController;
#include <SmartMiFanAsync.h>
#endif

#include <HomeWindWSAmoled.h>

static_assert(HOMEWIND_WS_AMOLED_VERSION_MAJOR > 1 ||
                  (HOMEWIND_WS_AMOLED_VERSION_MAJOR == 1 && HOMEWIND_WS_AMOLED_VERSION_MINOR >= 6),
              "HomeWindWSAmoled >= 1.6.0 required (homewind_set_csc_power)");

DisplayManager* g_displayManager = nullptr;

DisplayManager::DisplayManager()
  : _initialized(false)
  , _lastHR(0)
  , _lastCadence(0)
  , _lastPower(0)
  , _lastPowerValid(false)
  , _shownWatts(-1)
  , _fansDirty(false)
  , _hrValueDirty(false)
  , _cscValueDirty(false)
  , _hrStateDirty(false)
  , _cscStateDirty(false)
  , _urlDirty(false)
  , _hrStateConfigured(false)
  , _cachedHRState(HR_NOT_CONFIGURATED)
  , _cscStateConfigured(false)
  , _cachedCSCState(CSC_NOT_CONFIGURATED)
  , _cachedShowPower(false) {
  _cachedHRName[0] = '\0';
  _cachedCSCName[0] = '\0';
  _cachedUrl[0] = '\0';
}

bool DisplayManager::begin() {
  if (_initialized) {
    return true;
  }

  homewind_init();
  homewind_set_fan_toggle_callback(staticFanToggleCallback);

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

  if (!_hrValueDirty && !_cscValueDirty && !_hrStateDirty && !_cscStateDirty && !_fansDirty && !_urlDirty) {
    return;
  }

  if (!lcd_lvgl_lock(50)) {
    return;  /* Lock timeout – retry next loop iteration */
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
  /* Each csc setter redraws the card when its value changed: power first,
     then one call for state, name and cadence (or cadence only). */
  const bool cscState = _cscStateDirty.exchange(false);
  const bool cscValue = _cscValueDirty.exchange(false);
  if (cscState || cscValue) {
    const int32_t watts = hw::bikeCardWatts(_cachedShowPower, _lastPowerValid, _lastPower);
    if (watts != _shownWatts) {
      _shownWatts = watts;
      homewind_set_csc_power(watts);
    }
    if (!cscState) {
      homewind_set_csc_cadence(_lastCadence);
    } else if (_cscStateConfigured) {
      homewind_set_csc(_cachedCSCState, _cachedCSCName, _lastCadence);
    } else {
      homewind_set_csc_state(CSC_NOT_CONFIGURATED);
    }
  }
  if (_fansDirty.exchange(false)) {
    updateFans();
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
}

void DisplayManager::updateHRState() {
  if (!_initialized) return;

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
        strncpy(_cachedHRName, sensors[i].name, sizeof(_cachedHRName) - 1);
        _cachedHRName[sizeof(_cachedHRName) - 1] = '\0';
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
  _lastPower = m.powerW;
  _lastPowerValid = (m.flags & hw::kBikePowerValid) != 0;
  _cscValueDirty = true;
}

void DisplayManager::updateCSCState() {
  if (!_initialized) return;

#if HW_ENABLE_BLE
  if (!g_bleRelayManager) {
    _cscStateConfigured = false;
    _cachedShowPower = false;
  } else {
    HwSensorInfo sensors[BLERelayManager::MAX_SENSORS];
    uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, BLERelayManager::MAX_SENSORS);

    const HwSensorInfo* csc = nullptr;
    const HwSensorInfo* pwr = nullptr;
    for (uint8_t i = 0; i < count; i++) {
      if (!csc && strcmp(sensors[i].type, "CSC") == 0) csc = &sensors[i];
      if (!pwr && strcmp(sensors[i].type, "PWR") == 0) pwr = &sensors[i];
    }
    const hw::BikeCard card = hw::selectBikeCard(csc != nullptr, csc && csc->connected,
                                                 pwr != nullptr, pwr && pwr->connected);
    _cscStateConfigured = card.state != hw::BikeCardState::NotConfigured;
    _cachedCSCState = card.state == hw::BikeCardState::Active ? CSC_STATE_ACTIVE : CSC_STATE_INACTIVE;
    _cachedShowPower = card.showPower;
    const HwSensorInfo* named = card.nameFromPower ? pwr : csc;
    if (named) {
      strncpy(_cachedCSCName, named->name, sizeof(_cachedCSCName) - 1);
      _cachedCSCName[sizeof(_cachedCSCName) - 1] = '\0';
    }
  }
#else
  _cscStateConfigured = false;
  _cachedShowPower = false;
#endif
  _cscStateDirty = true;
}

void DisplayManager::markFansDirty() {
  if (!_initialized) return;
  _fansDirty = true;
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

void DisplayManager::staticFanToggleCallback(uint8_t fanIndex, bool isOn) {
  if (g_displayManager) {
    g_displayManager->onFanToggle(fanIndex, isOn);
  }
}

void DisplayManager::onFanToggle(uint8_t fanIndex, bool isOn) {
#if HW_ENABLE_FANS
  if (!g_fanController) return;

  FanConfig cfg;
  if (!g_fanController->getFanConfig(fanIndex, cfg)) return;

  g_fanController->setFanControlState(String(cfg.token), isOn);
#endif
}

#endif // HW_ENABLE_DISPLAY
