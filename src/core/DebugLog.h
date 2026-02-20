/**
 * @file DebugLog.h
 * @brief Debug logging macros (gated by HW_ENABLE_DEBUG_LOGS)
 * 
 * Production builds should have HW_ENABLE_DEBUG_LOGS=0 to remove all debug output.
 * This reduces flash usage and improves performance.
 */

#ifndef HOMEWIND_DEBUG_LOG_H
#define HOMEWIND_DEBUG_LOG_H

#include "../app/Config.h"

#if HW_ENABLE_DEBUG_LOGS
  // Debug logging enabled
  #define HW_DEBUG_PRINT(x) Serial.print(x)
  #define HW_DEBUG_PRINTLN(x) Serial.println(x)
  #define HW_DEBUG_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)
#else
  // Debug logging disabled (compile-time removal)
  #define HW_DEBUG_PRINT(x) ((void)0)
  #define HW_DEBUG_PRINTLN(x) ((void)0)
  #define HW_DEBUG_PRINTF(fmt, ...) ((void)0)
#endif

// Error logging (always enabled, even in production)
// These should be used for critical errors that need to be visible
#define HW_ERROR_PRINT(x) Serial.print(x)
#define HW_ERROR_PRINTLN(x) Serial.println(x)
#define HW_ERROR_PRINTF(fmt, ...) Serial.printf(fmt, ##__VA_ARGS__)

#endif // HOMEWIND_DEBUG_LOG_H

