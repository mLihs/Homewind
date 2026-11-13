/**
 * @file FanModeSettings.h
 * @brief Fan control mode, FTP and cool-down with NVS persistence
 *
 * Blob layout and validation: FanModeBlob.h. Same request/apply pattern as
 * HeartRateSettings: any task queues, loop() applies and saves.
 * Missing or invalid blob -> Pure, 200 W, cool-down 0.
 */

#ifndef HOMEWIND_FAN_MODE_SETTINGS_H
#define HOMEWIND_FAN_MODE_SETTINGS_H

#include "../app/Config.h"

#if HW_ENABLE_FAN_POWER_MODE && HW_ENABLE_NVS

#include <Arduino.h>
#include "FanModeBlob.h"

class FanModeSettings {
public:
  bool begin();
  bool ready() const { return _initialized; }

  /** Loop task (the only writer is loop()). */
  hw::FanMode mode() const { return _mode; }
  uint16_t ftpW() const { return _ftpW; }
  int8_t cooldown() const { return _cooldown; }

  /**
   * Queue a change (any task). Mode and cool-down must be valid; FTP is clamped to
   * kFanFtpMinW..kFanFtpMaxW. Hybrid preconditions are checked by the caller.
   * @param setMode, setFtp, setCooldown which fields to change (the others keep their
   *        queued or current value)
   * @return false if not initialized, no field is set, maintenance admission is closed
   *         or the mode or cool-down is invalid
   */
  bool request(bool setMode, uint8_t mode, bool setFtp, uint16_t ftpW, bool setCooldown, int8_t cooldown);

  /** Loop task: apply the queued change and save it. Returns true if a change was applied. */
  bool loop();

private:
  bool load();
  bool save();

  hw::FanMode _mode = hw::FanMode::Pure;
  uint16_t _ftpW = hw::kFanFtpDefaultW;
  int8_t _cooldown = 0;
  bool _initialized = false;

  portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
  bool _pendingValid = false;
  hw::FanMode _pendingMode = hw::FanMode::Pure;
  uint16_t _pendingFtpW = hw::kFanFtpDefaultW;
  int8_t _pendingCooldown = 0;
};

#endif // HW_ENABLE_FAN_POWER_MODE && HW_ENABLE_NVS

#endif // HOMEWIND_FAN_MODE_SETTINGS_H
