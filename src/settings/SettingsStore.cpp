/**
 * @file SettingsStore.cpp
 * @brief Persistent settings storage implementation (stub)
 */

#include "../app/Config.h"
#include "../core/DebugLog.h"

#if HOMEWIND_ENABLE_SETTINGS

#include "SettingsStore.h"

SettingsStore::SettingsStore() : initialized(false) {
}

SettingsStore::~SettingsStore() {
}

bool SettingsStore::begin() {
  if (initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[SettingsStore] Initialized (stub - no persistence yet)");
  initialized = true;
  return true;
}

String SettingsStore::get(const String& key) {
  (void)key; // Unused parameter warning suppression
  return String(""); // Stub: always return empty
}

bool SettingsStore::set(const String& key, const String& value) {
  (void)key;   // Unused parameter warning suppression
  (void)value; // Unused parameter warning suppression
  return true; // Stub: always succeed
}

bool SettingsStore::remove(const String& key) {
  (void)key; // Unused parameter warning suppression
  return true; // Stub: always succeed
}

String SettingsStore::list(const String& prefix) {
  (void)prefix; // Unused parameter warning suppression
  return String(""); // Stub: return empty list
}

bool SettingsStore::exists(const String& key) {
  (void)key; // Unused parameter warning suppression
  return false; // Stub: always return false
}

bool SettingsStore::factoryReset() {
  if (!initialized) {
    return false;
  }
  
  HW_DEBUG_PRINTLN("[SettingsStore] Factory reset: clearing all settings");
  
  // TODO: When real NVS implementation is added, clear all namespaces:
  // - WiFi credentials namespace
  // - User settings namespace
  // - Fan configs namespace
  // - Sensor configs namespace
  // - Any cached device lists
  
  // For now (stub), just log that reset was requested
  // When NVS is implemented, use Preferences::clear() for each namespace
  
  HW_DEBUG_PRINTLN("[SettingsStore] Factory reset complete (stub - no persistence yet)");
  return true;
}

#else // HOMEWIND_ENABLE_SETTINGS not defined

// Stub implementations when settings are disabled
#include "SettingsStore.h"

SettingsStore::SettingsStore() : initialized(false) {
}

SettingsStore::~SettingsStore() {
}

bool SettingsStore::begin() {
  initialized = true;
  return true;
}

String SettingsStore::get(const String& key) {
  (void)key;
  return String("");
}

bool SettingsStore::set(const String& key, const String& value) {
  (void)key;
  (void)value;
  return false; // Disabled
}

bool SettingsStore::remove(const String& key) {
  (void)key;
  return false; // Disabled
}

String SettingsStore::list(const String& prefix) {
  (void)prefix;
  return String("");
}

bool SettingsStore::exists(const String& key) {
  (void)key;
  return false;
}

bool SettingsStore::factoryReset() {
  // Settings disabled - nothing to reset
  return true;
}

#endif // HOMEWIND_ENABLE_SETTINGS

