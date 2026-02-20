/**
 * @file SettingsStore.h
 * @brief Persistent settings storage (stub - no NVS yet)
 */

#ifndef HOMEWIND_SETTINGS_STORE_H
#define HOMEWIND_SETTINGS_STORE_H

#include <Arduino.h>
#include "../app/Config.h"

#if HOMEWIND_ENABLE_SETTINGS
/**
 * @class SettingsStore
 * @brief Settings storage interface (stub implementation)
 */
class SettingsStore {
public:
  SettingsStore();
  ~SettingsStore();
  
  /**
   * Initialize settings store
   * @return true on success
   */
  bool begin();
  
  /**
   * Get setting value
   * @param key Setting key
   * @return Value or empty string if not found
   */
  String get(const String& key);
  
  /**
   * Set setting value
   * @param key Setting key
   * @param value Setting value
   * @return true on success
   */
  bool set(const String& key, const String& value);
  
  /**
   * Remove setting
   * @param key Setting key
   * @return true on success
   */
  bool remove(const String& key);
  
  /**
   * List all keys with given prefix
   * @param prefix Key prefix
   * @return Comma-separated list of keys
   */
  String list(const String& prefix);
  
  /**
   * Check if key exists
   * @param key Setting key
   * @return true if exists
   */
  bool exists(const String& key);
  
  /**
   * Factory reset - clear all persisted settings
   * Wipes all settings stored in NVS/Preferences
   * @return true on success
   */
  bool factoryReset();
  
  /**
   * Check if store is initialized and ready
   * @return true if ready
   */
  bool ready() const { return initialized; }

private:
  bool initialized;
};
#endif // HOMEWIND_ENABLE_SETTINGS

#endif // HOMEWIND_SETTINGS_STORE_H

