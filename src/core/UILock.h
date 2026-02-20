/**
 * @file UILock.h
 * @brief UI Lock management for write-one, read-many pattern
 * 
 * State-Manager-driven UI lock system. Only one session can hold the lock
 * at a time, and the lock is tied to UI flows (modals).
 */

#ifndef HOMEWIND_UI_LOCK_H
#define HOMEWIND_UI_LOCK_H

#include <Arduino.h>
#include "../app/Config.h"

#if HOMEWIND_ENABLE_API_ACTIONS

/**
 * @class UILock
 * @brief Manages UI lock for exclusive operations
 * 
 * Single global lock that can be held by one session at a time.
 * Lock lifecycle is driven by UI State Manager flows.
 */
class UILock {
public:
  /**
   * Initialize UI lock system
   */
  static void begin();
  
  /**
   * Acquire lock for a session
   * @param sessionToken Session token of the requesting client
   * @param reason Flow name (e.g., "addFan", "firmwareUpdate")
   * @return true if lock acquired, false if already locked by another session
   */
  static bool acquire(const char* sessionToken, const char* reason);
  
  /**
   * Release lock for a session
   * @param sessionToken Session token of the releasing client
   * @return true if released (or not locked), false if locked by another session
   */
  static bool release(const char* sessionToken);
  
  /**
   * Check if lock is held
   * @return true if lock is currently held
   */
  static bool isHeld();
  
  /**
   * Get lock owner session token
   * @return Owner session token or empty string if not locked (no heap allocation)
   */
  static const char* getOwner();
  
  /**
   * Get lock reason (flow name)
   * @return Flow name or empty string if not locked (no heap allocation)
   */
  static const char* getReason();
  
  /**
   * Get lock age in milliseconds
   * @return Milliseconds since lock was acquired, or 0 if not locked
   */
  static uint32_t getAgeMs();
  
  /**
   * Check if session owns the lock
   * @param sessionToken Session token to check
   * @return true if session owns the lock
   */
  static bool isOwner(const char* sessionToken);
  
  /**
   * Get owner hint (for debugging/info)
   * @return Owner hint string (e.g., "Chrome (192.168.1.100)") (no heap allocation)
   */
  static const char* getOwnerHint();
  
  /**
   * Set owner hint (called when lock is acquired)
   * @param hint Hint string (e.g., "Chrome (192.168.1.100)")
   */
  static void setOwnerHint(const char* hint);
  
  /**
   * Check and expire lock if TTL exceeded (failsafe only)
   * Should be called periodically from main loop
   */
  static void checkTTL();

private:
  static bool _held;
  static constexpr size_t SESSION_BUFFER_SIZE = 24;  // "session_xxxxxxxx" + margin
  static constexpr size_t REASON_BUFFER_SIZE = 32;   // Flow names
  static constexpr size_t HINT_BUFFER_SIZE = 48;     // "Browser (xxx.xxx.xxx.xxx)"
  static char _ownerSession[SESSION_BUFFER_SIZE];
  static char _reason[REASON_BUFFER_SIZE];
  static uint32_t _sinceMs;
  static char _ownerHint[HINT_BUFFER_SIZE];
  static const uint32_t TTL_MS; // Time-to-live in milliseconds (failsafe)
};

#endif // HOMEWIND_ENABLE_API_ACTIONS

#endif // HOMEWIND_UI_LOCK_H

