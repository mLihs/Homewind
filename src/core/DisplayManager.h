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

class DisplayManager {
public:
  DisplayManager();

  bool begin();
  void loop();

  void setHeartRate(uint16_t bpm);
  void updateHRState();

  void setCadence(uint16_t rpm);
  void updateCSCState();

  void updateFans();

  /** Mark fans dirty for deferred update (no LVGL call, safe from any task) */
  void markFansDirty();

  void setUrl(const char* url);

  void showAPScreen();    /* Switch to AP/Wifisetup screen (Captive Portal) */
  void showMainScreen();  /* Switch to main screen */

private:
  bool _initialized;
  uint16_t _lastHR;
  uint16_t _lastCadence;

  /* Dirty-flags for deferred LVGL updates (written from BLE/Fan/Arduino tasks, read in loop()) */
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

  /* Cached URL for setUrl */
  char _cachedUrl[64];

  static void staticFanToggleCallback(uint8_t fanIndex, bool isOn);
  void onFanToggle(uint8_t fanIndex, bool isOn);
};

extern DisplayManager* g_displayManager;

#endif // HW_ENABLE_DISPLAY
#endif // HOMEWIND_DISPLAY_MANAGER_H
