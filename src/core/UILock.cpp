/**
 * @file UILock.cpp
 * @brief Implementation of UI Lock management
 */

#include "UILock.h"
#include "DebugLog.h"

#if HOMEWIND_ENABLE_API_ACTIONS

// Static lock state (using char arrays to avoid heap fragmentation)
bool UILock::_held = false;
char UILock::_ownerSession[SESSION_BUFFER_SIZE] = "";
char UILock::_reason[REASON_BUFFER_SIZE] = "";
uint32_t UILock::_sinceMs = 0;
char UILock::_ownerHint[HINT_BUFFER_SIZE] = "";
const uint32_t UILock::TTL_MS = 300000; // 5 minutes failsafe TTL

void UILock::begin() {
  _held = false;
  _ownerSession[0] = '\0';
  _reason[0] = '\0';
  _sinceMs = 0;
  _ownerHint[0] = '\0';
}

bool UILock::acquire(const char* sessionToken, const char* reason) {
  if (!sessionToken || sessionToken[0] == '\0') {
    return false;
  }
  
  // If already locked by same session, allow (idempotent)
  if (_held && strcmp(_ownerSession, sessionToken) == 0) {
    return true;
  }
  
  // If locked by different session, deny
  if (_held && strcmp(_ownerSession, sessionToken) != 0) {
    return false;
  }
  
  // Acquire lock
  _held = true;
  strncpy(_ownerSession, sessionToken, SESSION_BUFFER_SIZE - 1);
  _ownerSession[SESSION_BUFFER_SIZE - 1] = '\0';
  if (reason) {
    strncpy(_reason, reason, REASON_BUFFER_SIZE - 1);
    _reason[REASON_BUFFER_SIZE - 1] = '\0';
  } else {
    _reason[0] = '\0';
  }
  _sinceMs = millis();
  
  HW_DEBUG_PRINTF("[UILock] Lock acquired by session %s for flow: %s\n", 
                 sessionToken, reason ? reason : "");
  
  return true;
}

bool UILock::release(const char* sessionToken) {
  // If not locked, allow (idempotent)
  if (!_held) {
    return true;
  }
  
  // If locked by different session, deny (but don't error - just return false)
  if (!sessionToken || strcmp(_ownerSession, sessionToken) != 0) {
    return false;
  }
  
  // Release lock - save old reason for debug log
  char oldReason[REASON_BUFFER_SIZE];
  strncpy(oldReason, _reason, REASON_BUFFER_SIZE - 1);
  oldReason[REASON_BUFFER_SIZE - 1] = '\0';
  
  _held = false;
  _ownerSession[0] = '\0';
  _reason[0] = '\0';
  _sinceMs = 0;
  _ownerHint[0] = '\0';
  
  HW_DEBUG_PRINTF("[UILock] Lock released by session %s (was: %s)\n", 
                 sessionToken, oldReason);
  
  return true;
}

bool UILock::isHeld() {
  return _held;
}

const char* UILock::getOwner() {
  return _ownerSession;
}

const char* UILock::getReason() {
  return _reason;
}

uint32_t UILock::getAgeMs() {
  if (!_held) {
    return 0;
  }
  return millis() - _sinceMs;
}

bool UILock::isOwner(const char* sessionToken) {
  if (!sessionToken) return false;
  return _held && strcmp(_ownerSession, sessionToken) == 0;
}

const char* UILock::getOwnerHint() {
  return _ownerHint;
}

void UILock::setOwnerHint(const char* hint) {
  if (hint) {
    strncpy(_ownerHint, hint, HINT_BUFFER_SIZE - 1);
    _ownerHint[HINT_BUFFER_SIZE - 1] = '\0';
  } else {
    _ownerHint[0] = '\0';
  }
}

void UILock::checkTTL() {
  if (!_held) {
    return;
  }
  
  uint32_t age = getAgeMs();
  if (age > TTL_MS) {
    HW_DEBUG_PRINTF("[UILock] Lock expired (age: %lu ms, TTL: %lu ms)\n", age, TTL_MS);
    _held = false;
    _ownerSession[0] = '\0';
    _reason[0] = '\0';
    _sinceMs = 0;
    _ownerHint[0] = '\0';
  }
}

#endif // HOMEWIND_ENABLE_API_ACTIONS

