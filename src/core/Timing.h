/**
 * @file Timing.h
 * @brief Non-blocking timing utilities
 */

#ifndef HOMEWIND_TIMING_H
#define HOMEWIND_TIMING_H

#include <Arduino.h>

/**
 * @class Timer
 * @brief Non-blocking timer using millis()
 */
class Timer {
public:
  /**
   * Create a timer with specified interval
   * @param intervalMs Interval in milliseconds
   */
  Timer(uint32_t intervalMs) : interval(intervalMs), lastTick(0) {}
  
  /**
   * Check if interval has elapsed (non-blocking)
   * @return true if interval elapsed, false otherwise
   */
  bool tick() {
    uint32_t now = millis();
    if ((now - lastTick) >= interval) {
      lastTick = now;
      return true;
    }
    return false;
  }
  
  /**
   * Reset the timer
   */
  void reset() {
    lastTick = millis();
  }
  
  /**
   * Set new interval
   * @param intervalMs New interval in milliseconds
   */
  void setInterval(uint32_t intervalMs) {
    interval = intervalMs;
  }

private:
  uint32_t interval;
  uint32_t lastTick;
};

/**
 * @class Debounce
 * @brief Simple debounce utility
 */
class Debounce {
public:
  /**
   * Create a debounce with specified delay
   * @param delayMs Debounce delay in milliseconds
   */
  Debounce(uint32_t delayMs) : delay(delayMs), lastState(false), lastChange(0) {}
  
  /**
   * Update debounce state
   * @param currentState Current raw state
   * @return Debounced state
   */
  bool update(bool currentState) {
    uint32_t now = millis();
    if (currentState != lastState) {
      lastChange = now;
      lastState = currentState;
      return false; // State changed but not yet debounced
    }
    if ((now - lastChange) >= delay) {
      return currentState; // Stable state
    }
    return !currentState; // Return previous stable state during debounce
  }

private:
  uint32_t delay;
  bool lastState;
  uint32_t lastChange;
};

#endif // HOMEWIND_TIMING_H

