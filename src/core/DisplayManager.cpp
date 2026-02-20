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
#include "BLERelayManager.h"
extern BLERelayManager* g_bleRelayManager;
#endif

#if HW_ENABLE_FANS
#include "FanController.h"
extern FanController* g_fanController;
#define SystemState SmartMiFanAsync_SystemState
#include <SmartMiFanAsync.h>
#undef SystemState
#endif

#include <HomeWindWSAmoled.h>

DisplayManager* g_displayManager = nullptr;

DisplayManager::DisplayManager()
  : _initialized(false)
  , _lastHR(0)
  , _lastCadence(0)
  , _fansDirty(false)
  , _hrValueDirty(false)
  , _cscValueDirty(false)
  , _hrStateDirty(false)
  , _cscStateDirty(false)
  , _urlDirty(false)
  , _hrStateConfigured(false)
  , _cachedHRState(HR_NOT_CONFIGURATED)
  , _cscStateConfigured(false)
  , _cachedCSCState(CSC_NOT_CONFIGURATED) {
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
  HW_DEBUG_PRINTLN("[Display] Initialized");
  return true;
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
  if (_cscValueDirty.exchange(false)) {
    homewind_set_csc_cadence(_lastCadence);
  }
  if (_hrStateDirty.exchange(false)) {
    if (_hrStateConfigured) {
      homewind_set_hr(_cachedHRState, _cachedHRName, _lastHR);
    } else {
      homewind_set_hr_state(HR_NOT_CONFIGURATED);
    }
  }
  if (_cscStateDirty.exchange(false)) {
    if (_cscStateConfigured) {
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
    SensorInfo sensors[2];
    uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, 2);

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

void DisplayManager::setCadence(uint16_t rpm) {
  if (!_initialized) return;
  _lastCadence = rpm;
  _cscValueDirty = true;
}

void DisplayManager::updateCSCState() {
  if (!_initialized) return;

#if HW_ENABLE_BLE
  if (!g_bleRelayManager) {
    _cscStateConfigured = false;
  } else {
    SensorInfo sensors[2];
    uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, 2);

    for (uint8_t i = 0; i < count; i++) {
      if (strcmp(sensors[i].type, "CSC") == 0) {
        _cscStateConfigured = true;
        _cachedCSCState = sensors[i].connected ? CSC_STATE_ACTIVE : CSC_STATE_INACTIVE;
        if (!sensors[i].connected) _lastCadence = 0;
        strncpy(_cachedCSCName, sensors[i].name, sizeof(_cachedCSCName) - 1);
        _cachedCSCName[sizeof(_cachedCSCName) - 1] = '\0';
        _cscStateDirty = true;
        return;
      }
    }
    _cscStateConfigured = false;
  }
#else
  _cscStateConfigured = false;
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

  uint8_t fanCount = g_fanController->getFanCount();

  SmartMiFanDiscoveredDevice discovered[4];
  size_t discoveredCount = 0;
  g_fanController->getDiscoveredFans(static_cast<void*>(discovered), discoveredCount);

  for (uint8_t i = 0; i < 4; i++) {
    if (i >= fanCount) {
      homewind_set_fan_state(i, FAN_STATE_NOT_CONFIGURATED);
      continue;
    }

    FanConfig cfg;
    if (!g_fanController->getFanConfig(i, cfg)) {
      homewind_set_fan_state(i, FAN_STATE_NOT_CONFIGURATED);
      continue;
    }

    fan_state_t state = FAN_STATE_INACTIVE;  /* Default: configured but not yet discovered */
    bool isOn = false;

    for (size_t j = 0; j < discoveredCount; j++) {
      if (strcasecmp(discovered[j].token, cfg.token) == 0) {
        FanParticipationState p = SmartMiFanAsync_getFanParticipationState(static_cast<uint8_t>(j));
        state = (p == FanParticipationState::ERROR || !discovered[j].ready) ? FAN_STATE_ERROR : FAN_STATE_ACTIVE;
        isOn = (p == FanParticipationState::ACTIVE);
        break;
      }
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
