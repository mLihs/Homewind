/**
 * @file SessionManager.h
 * @brief Session token management for browser identification
 */

#ifndef HOMEWIND_SESSION_MANAGER_H
#define HOMEWIND_SESSION_MANAGER_H

#include <Arduino.h>
#include "../app/Config.h"

#if HOMEWIND_ENABLE_API_ACTIONS

/**
 * @class SessionManager
 * @brief Manages session tokens for browser identification
 * 
 * Generates and validates session tokens for browser sessions.
 * Tokens are opaque strings with browser lifetime.
 */
class SessionManager {
public:
  // Buffer sizes for session tokens and hints
  static constexpr size_t TOKEN_BUFFER_SIZE = 24;  // "session_xxxxxxxx" + margin
  static constexpr size_t HINT_BUFFER_SIZE = 48;   // "Browser (xxx.xxx.xxx.xxx)"
  
  /**
   * Generate a new session token into buffer (no heap allocation)
   * @param buffer Output buffer (must be at least TOKEN_BUFFER_SIZE)
   * @param bufferSize Size of output buffer
   */
  static void generateToken(char* buffer, size_t bufferSize);
  
  /**
   * Validate a session token format
   * @param token Token to validate
   * @return true if token format is valid
   */
  static bool validateToken(const char* token);
  
  /**
   * Generate owner hint from request into buffer (no heap allocation)
   * @param request HTTP request
   * @param buffer Output buffer (must be at least HINT_BUFFER_SIZE)
   * @param bufferSize Size of output buffer
   */
  static void generateOwnerHint(void* request, char* buffer, size_t bufferSize);
};

#endif // HOMEWIND_ENABLE_API_ACTIONS

#endif // HOMEWIND_SESSION_MANAGER_H

