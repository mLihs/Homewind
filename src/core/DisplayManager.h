/**
 * @file DisplayManager.h
 * @brief Display integration - bridges Homewind data to HomeWindWSAmoled
 *
 * Integrates HR, power and cadence rows, status badges, fan control and the
 * web URL. Compile-time gated by HW_ENABLE_DISPLAY.
 * Uses static allocation (no heap). Data flows from existing callbacks.
 */

#ifndef HOMEWIND_DISPLAY_MANAGER_H
#define HOMEWIND_DISPLAY_MANAGER_H

#include "../app/Config.h"

#if HW_ENABLE_DISPLAY

#include <atomic>
#include <HomeWindWSAmoled.h>
#include "BikeCard.h"
#include "BikeMetrics.h"

class DisplayManager {
public:
  DisplayManager();

  bool begin();
  void loop();

  void setHeartRate(uint16_t bpm);
  void updateHRState();

  /** Power and cadence values: the latched BIKE_METRICS state */
  void setBikeMetrics(const hw::BikeMetrics& m);
  /** Power and cadence rows: state and names from CSC, PWR and the trainer */
  void updateCSCState();

  void updateFans();

  /** Mark fans dirty for deferred update (no LVGL call, safe from any task) */
  void markFansDirty();

  /** Web UI change (any task): restarts the panel-off timeout, wakes a dark panel */
  void noteUserActivity();

  void setUrl(const char* url);

  void showAPScreen();    /* Switch to AP/Wifisetup screen (Captive Portal) */
  void showMainScreen();  /* Switch to main screen */

  /** Detected board revision ("V1", "V1.1"), nullptr before begin() */
  const char* boardRevision() const;
  /** LCD_TE pulses seen during board detection (diagnostics) */
  uint32_t tePulseCount() const;

private:
  void applyTouchRequests();
  void pollStatus();

  static void staticHrShift(int8_t steps);
  static void staticFanMode();

  bool _initialized;
  uint16_t _lastHR;
  uint16_t _lastCadence;
  bool _lastCadenceValid;
  int16_t _lastPower;
  bool _lastPowerValid;
  bool _pwrCadenceSeen;  /* the power meter delivered cadence this boot */

  /* Dirty-flags for deferred LVGL updates, read in loop(). Values and HR/bike
     state are written in the Arduino loop task (BLERelayManager::loop(), App);
     _fansDirty and _activity are also set from other tasks (web handlers). */
  std::atomic<bool> _fansDirty;
  std::atomic<bool> _hrValueDirty;
  std::atomic<bool> _bikeValueDirty;
  std::atomic<bool> _hrStateDirty;
  std::atomic<bool> _bikeStateDirty;
  std::atomic<bool> _urlDirty;
  std::atomic<bool> _badgesDirty;
  std::atomic<bool> _fanControlDirty;
  std::atomic<bool> _activity;

  /* Touch requests from the LVGL task, applied in the loop task */
  std::atomic<int8_t> _shiftRequest;
  std::atomic<bool> _modeRequest;

  /* Cached state for HR (from updateHRState) */
  bool _hrStateConfigured;
  hr_state_t _cachedHRState;
  char _cachedHRName[64];

  /* Rows (from updateCSCState) */
  hw::BikeRow _powerRow;
  hw::BikeRow _cadenceRow;
  char _powerName[64];
  char _cadenceName[64];

  /* Badges and fan control (from pollStatus) */
  badge_state_t _appBadge;
  badge_state_t _trainerBadge;
  uint32_t _appLostMs;  /* when the APP badge turned orange */
  bool _trainerWasReady;
  bool _trainerMirror;
  bool _trainerReady;
  fan_mode_t _fanMode;
  bool _fanOn;
  uint8_t _fanPct;

  uint32_t _lastPollMs;
  uint32_t _lastWakeMs;

  /* Cached URL for setUrl */
  char _cachedUrl[64];
};

extern DisplayManager* g_displayManager;

#endif // HW_ENABLE_DISPLAY
#endif // HOMEWIND_DISPLAY_MANAGER_H
