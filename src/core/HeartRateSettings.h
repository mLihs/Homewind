/**
 * @file HeartRateSettings.h
 * @brief Heart rate min/max settings manager with NVS persistence
 * 
 * Uses deterministic struct-based storage (no heap allocation for NVS operations).
 * Data is stored as a binary blob with version and CRC16 for integrity.
 */

#ifndef HOMEWIND_HEARTRATE_SETTINGS_H
#define HOMEWIND_HEARTRATE_SETTINGS_H

#include "../app/Config.h"
#include <Arduino.h>

#if HW_ENABLE_NVS
#include <Preferences.h>

/**
 * @class HeartRateSettings
 * @brief Manages heart rate min/max settings with NVS persistence
 * 
 * Stores heart rate range settings in NVS using direct Preferences API.
 * Uses deterministic struct-based storage with CRC16 validation.
 * Zero heap allocation during load/save operations.
 */
class HeartRateSettings {
public:
  HeartRateSettings();
  ~HeartRateSettings();
  
  /**
   * Initialize settings manager
   * Loads values from NVS or applies defaults
   * @return true on success
   */
  bool begin();
  
  /**
   * Get minimum heart rate
   * @return Min heart rate value (default: 80)
   */
  uint16_t getMin() const { return _minHeartRate; }
  
  /**
   * Get maximum heart rate
   * @return Max heart rate value (default: 160)
   */
  uint16_t getMax() const { return _maxHeartRate; }
  
  /**
   * Set minimum heart rate
   * Validates and saves to NVS
   * @param min Min heart rate (50-200, must be < max - 10)
   * @return true on success
   */
  bool setMin(uint16_t min);
  
  /**
   * Set maximum heart rate
   * Validates and saves to NVS
   * @param max Max heart rate (50-200, must be > min + 10)
   * @return true on success
   */
  bool setMax(uint16_t max);
  
  /**
   * Set both min and max heart rate
   * Validates and saves to NVS
   * @param min Min heart rate
   * @param max Max heart rate
   * @return true on success
   */
  bool setRange(uint16_t min, uint16_t max);
  
  /**
   * Load settings from NVS
   * @return true if loaded successfully, false if using defaults
   */
  bool loadFromNVS();
  
  /**
   * Save settings to NVS
   * @return true on success
   */
  bool saveToNVS();
  
  /**
   * Check if settings are initialized and ready
   * @return true if ready
   */
  bool ready() const { return _initialized; }

private:
  static constexpr uint16_t HARD_MIN = 50;
  static constexpr uint16_t HARD_MAX = 200;
  static constexpr uint16_t DEFAULT_MIN = 80;
  static constexpr uint16_t DEFAULT_MAX = 160;
  static constexpr uint16_t MIN_GAP = 10; // Minimum gap between min and max
  
  // NVS namespace and key
  static constexpr const char* NVS_NAMESPACE = "homewind";
  static constexpr const char* NVS_KEY_DATA = "hr_data";      // New blob format
  
  /**
   * @brief NVS data structure (deterministic, no heap)
   * 
   * Layout:
   * - version: Schema version for future migrations
   * - minHR: Minimum heart rate
   * - maxHR: Maximum heart rate
   * - reserved: Future expansion
   * - crc16: CRC16-CCITT for data integrity
   */
  #pragma pack(push, 1)
  struct NVSData {
    uint8_t version;      // Schema version (current: 1)
    uint16_t minHR;       // Min heart rate
    uint16_t maxHR;       // Max heart rate
    uint8_t reserved[3];  // Reserved for future use
    uint16_t crc16;       // CRC16-CCITT checksum
  };
  #pragma pack(pop)
  
  static_assert(sizeof(NVSData) == 10, "NVSData must be 10 bytes");
  
  static constexpr uint8_t NVS_DATA_VERSION = 1;
  
  uint16_t _minHeartRate;
  uint16_t _maxHeartRate;
  bool _initialized;
  
  /**
   * Validate min/max range
   * @param min Min value
   * @param max Max value
   * @return true if valid
   */
  bool validateRange(uint16_t min, uint16_t max) const;
  
  /**
   * Clamp value to valid range
   * @param value Value to clamp
   * @param min Minimum allowed
   * @param max Maximum allowed
   * @return Clamped value
   */
  static uint16_t clamp(uint16_t value, uint16_t min, uint16_t max);
  
  
};

#endif // HW_ENABLE_NVS

#endif // HOMEWIND_HEARTRATE_SETTINGS_H

