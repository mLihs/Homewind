/**
 * @file FanModeSettings.cpp
 * @brief Fan control mode, FTP and cool-down with NVS persistence
 */

#include "FanModeSettings.h"

#if HW_ENABLE_FAN_POWER_MODE && HW_ENABLE_NVS

#include <Preferences.h>
#include "DebugLog.h"
#include "MaintenanceMode.h"

namespace {
constexpr const char* kNvsNamespace = "homewind";
constexpr const char* kNvsKey = "fan_mode";
}  // namespace

bool FanModeSettings::begin() {
  if (_initialized) {
    return true;
  }
  if (!load()) {
    _mode = hw::FanMode::Pure;
    _ftpW = hw::kFanFtpDefaultW;
    _cooldown = 0;
  }
  HW_DEBUG_PRINTF("[FanModeSettings] mode=%u ftp=%u cooldown=%d\n", static_cast<unsigned>(_mode), _ftpW,
                  static_cast<int>(_cooldown));
  _initialized = true;
  return true;
}

bool FanModeSettings::request(bool setMode, uint8_t mode, bool setFtp, uint16_t ftpW, bool setCooldown,
                              int8_t cooldown) {
  if (!_initialized || (!setMode && !setFtp && !setCooldown) || MaintenanceMode::admissionClosed()) {
    return false;
  }
  if ((setMode && !hw::fanModeValid(mode)) || (setCooldown && !hw::fanCooldownValid(cooldown))) {
    return false;
  }
  portENTER_CRITICAL(&_mux);
  if (!_pendingValid) {
    _pendingMode = _mode;
    _pendingFtpW = _ftpW;
    _pendingCooldown = _cooldown;
  }
  if (setMode) _pendingMode = static_cast<hw::FanMode>(mode);
  if (setFtp) _pendingFtpW = hw::fanFtpClamp(ftpW);
  if (setCooldown) _pendingCooldown = cooldown;
  _pendingValid = true;
  portEXIT_CRITICAL(&_mux);
  return true;
}

bool FanModeSettings::loop() {
  if (!_initialized) {
    return false;
  }
  // From maintenance REQUESTED on, NVS stays untouched (a factory reset erases it).
  // Apply inside the section: request() bases a new pending change on the current values.
  portENTER_CRITICAL(&_mux);
  const bool have = _pendingValid;
  _pendingValid = false;
  const bool apply = have && !MaintenanceMode::admissionClosed();
  const bool changed =
      apply && (_pendingMode != _mode || _pendingFtpW != _ftpW || _pendingCooldown != _cooldown);
  if (changed) {
    _mode = _pendingMode;
    _ftpW = _pendingFtpW;
    _cooldown = _pendingCooldown;
  }
  portEXIT_CRITICAL(&_mux);
  if (have && !apply) {
    HW_DEBUG_PRINTLN("[FanModeSettings] Queued change discarded (maintenance)");
  }
  if (!changed) {
    return false;
  }
  if (!save()) {
    HW_ERROR_PRINTLN("[FanModeSettings] Failed to save to NVS");
  }
  return true;
}

bool FanModeSettings::load() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, true)) {
    return false;
  }
  hw::FanModeNvsData data;
  const size_t len = prefs.getBytesLength(kNvsKey);
  const bool read = len == sizeof(data) && prefs.getBytes(kNvsKey, &data, sizeof(data)) == sizeof(data);
  prefs.end();
  if (!read) {
    return false;
  }
  const hw::FanModeLoad res =
      hw::fanModeDecode(reinterpret_cast<const uint8_t*>(&data), sizeof(data), _mode, _ftpW, _cooldown);
  if (res != hw::FanModeLoad::OK) {
    HW_DEBUG_PRINTF("[FanModeSettings] Blob rejected (%u), using defaults\n", static_cast<unsigned>(res));
    return false;
  }
  return true;
}

bool FanModeSettings::save() {
  Preferences prefs;
  if (!prefs.begin(kNvsNamespace, false)) {
    return false;
  }
  const hw::FanModeNvsData data = hw::fanModeEncode(_mode, _ftpW, _cooldown);
  const size_t written = prefs.putBytes(kNvsKey, &data, sizeof(data));
  prefs.end();
  return written == sizeof(data);
}

#endif // HW_ENABLE_FAN_POWER_MODE && HW_ENABLE_NVS
