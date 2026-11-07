/**
 * @file DisplayManager.h
 * @brief Display integration - bridges Homewind data to HomeWindWSAmoled
 *
 * Integrates HR, CSC, Fans, and Web-URL display. Compile-time gated by HW_ENABLE_DISPLAY.
 * Uses static allocation (no heap). Data flows from existing callbacks.
 */

#ifndef HOMEWIND_DISPLAY_MANAGER_H
#define HOMEWIND_DISPLAY_MANAGER_H

#include "../app/Config.h"

#if HW_ENABLE_DISPLAY

#include <atomic>
#include <HomeWindWSAmoled.h>
#include "BikeMetrics.h"

class DisplayManager {
public:
  DisplayManager();

  bool begin();
  void loop();

  void setHeartRate(uint16_t bpm);
  void updateHRState();

  /** Power and cadence of the cadence card: the latched BIKE_METRICS state */
  void setBikeMetrics(const hw::BikeMetrics& m);
  /** Cadence card state and name from the configured CSC and PWR sensors */
  void updateCSCState();

  void updateFans();

  /** Mark fans dirty for deferred update (no LVGL call, safe from any task) */
  void markFansDirty();

  void setUrl(const char* url);

  void showAPScreen();    /* Switch to AP/Wifisetup screen (Captive Portal) */
  void showMainScreen();  /* Switch to main screen */

  /** Detected board revision ("V1", "V1.1"), nullptr before begin() */
  const char* boardRevision() const;
  /** LCD_TE pulses seen during board detection (diagnostics) */
  uint32_t tePulseCount() const;

private:
  bool _initialized;
  uint16_t _lastHR;
  uint16_t _lastCadence;
  int16_t _lastPower;
  bool _lastPowerValid;
  int32_t _shownWatts;  /* last value passed to homewind_set_csc_power() */

  /* Dirty-flags for deferred LVGL updates, read in loop(). Values and HR/CSC
     state are written in the Arduino loop task (BLERelayManager::loop(), App);
     only _fansDirty is set from other tasks (web handlers, LVGL fan toggle). */
  std::atomic<bool> _fansDirty;
  std::atomic<bool> _hrValueDirty;
  std::atomic<bool> _cscValueDirty;
  std::atomic<bool> _hrStateDirty;
  std::atomic<bool> _cscStateDirty;
  std::atomic<bool> _urlDirty;

  /* Cached state for HR/CSC (from updateHRState/updateCSCState) */
  bool _hrStateConfigured;
  hr_state_t _cachedHRState;
  char _cachedHRName[64];
  bool _cscStateConfigured;
  csc_state_t _cachedCSCState;
  char _cachedCSCName[64];
  bool _cachedShowPower;

  /* Cached URL for setUrl */
  char _cachedUrl[64];

  static void staticFanToggleCallback(uint8_t fanIndex, bool isOn);
  void onFanToggle(uint8_t fanIndex, bool isOn);
};

extern DisplayManager* g_displayManager;

#endif // HW_ENABLE_DISPLAY
#endif // HOMEWIND_DISPLAY_MANAGER_H
