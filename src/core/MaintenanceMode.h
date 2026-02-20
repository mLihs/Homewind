/**
 * @file MaintenanceMode.h
 * @brief Maintenance mode gate for API endpoints
 * 
 * Maintenance mode blocks all non-whitelisted API endpoints with 503 responses.
 * Used during OTA updates, factory resets, and other critical operations.
 */

#ifndef HOMEWIND_MAINTENANCE_MODE_H
#define HOMEWIND_MAINTENANCE_MODE_H

#include <Arduino.h>

/**
 * @enum MaintenanceReason
 * @brief Reason for entering maintenance mode
 */
enum MaintenanceReason {
  UNKNOWN = 0,
  OTA = 1,
  FACTORY_RESET = 2
};

/**
 * @namespace MaintenanceMode
 * @brief Global maintenance mode state management
 */
namespace MaintenanceMode {
  /**
   * Begin maintenance mode
   * @param reason Reason for maintenance
   */
  void begin(MaintenanceReason reason);
  
  /**
   * End maintenance mode
   */
  void end();
  
  /**
   * Check if maintenance mode is active
   * @return true if maintenance mode is active
   */
  bool isActive();
  
  /**
   * Get current maintenance reason
   * @return MaintenanceReason enum value
   */
  MaintenanceReason reason();
  
  /**
   * Get current maintenance reason as string
   * @return "ota", "factory_reset", or "unknown"
   */
  const char* reasonString();
}

#endif // HOMEWIND_MAINTENANCE_MODE_H

