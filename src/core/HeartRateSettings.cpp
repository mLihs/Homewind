/**
 * @file HeartRateSettings.cpp
 * @brief Heart rate min/max settings manager implementation
 * 
 * Uses deterministic struct-based NVS storage with CRC16 validation.
 * Zero heap allocation during load/save operations.
 */

#include "HeartRateSettings.h"
#include "../core/DebugLog.h"
#include "../core/Crc16.h"

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
#endif

#if HW_ENABLE_NVS

HeartRateSettings::HeartRateSettings()
  : _minHeartRate(DEFAULT_MIN)
  , _maxHeartRate(DEFAULT_MAX)
  , _initialized(false)
{
}

HeartRateSettings::~HeartRateSettings() {
  // No heap allocations to clean up
}

bool HeartRateSettings::begin() {
  if (_initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[HeartRateSettings] Initializing (deterministic storage)...");
  
  // Load settings from NVS (or use defaults)
  if (loadFromNVS()) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Loaded from NVS: min=%u, max=%u\n", 
                    _minHeartRate, _maxHeartRate);
  } else {
    HW_DEBUG_PRINTF("[HeartRateSettings] Using defaults: min=%u, max=%u\n", 
                    _minHeartRate, _maxHeartRate);
    // Save defaults to NVS
    saveToNVS();
  }
  
  _initialized = true;
  HW_DEBUG_PRINTLN("[HeartRateSettings] Initialized");
  
  return true;
}

bool HeartRateSettings::setMin(uint16_t min) {
  if (!_initialized) {
    HW_DEBUG_PRINTF("[HeartRateSettings] setMin(%u) failed: not initialized\n", min);
    return false;
  }
  
  HW_DEBUG_PRINTF("[HeartRateSettings] setMin(%u) called, current min=%u, max=%u\n", 
                 min, _minHeartRate, _maxHeartRate);
  
  // Validate: min must be >= HARD_MIN and < max - MIN_GAP
  uint16_t maxAllowed = _maxHeartRate - MIN_GAP;
  HW_DEBUG_PRINTF("[HeartRateSettings] Validating: min must be in [%u, %u) (maxAllowed=%u)\n",
                 HARD_MIN, maxAllowed, maxAllowed);
  uint16_t clamped = clamp(min, HARD_MIN, maxAllowed);
  
  if (clamped != min) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Clamped min from %u to %u\n", min, clamped);
  }
  
  _minHeartRate = clamped;
  
  if (saveToNVS()) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Min heart rate set to %u (saved to NVS)\n", _minHeartRate);
    
#if HOMEWIND_ENABLE_WEBSOCKET
    // Notify WebSocket telemetry of change
    extern WebSocketTelemetry* g_webSocketTelemetry;
    if (g_webSocketTelemetry) {
      g_webSocketTelemetry->markDirty(TelemetryFrameType::HEART_RATE_SETTINGS);
    }
#endif
    
    return true;
  }
  
  HW_ERROR_PRINTLN("[HeartRateSettings] Failed to save min heart rate to NVS");
  return false;
}

bool HeartRateSettings::setMax(uint16_t max) {
  if (!_initialized) {
    HW_DEBUG_PRINTF("[HeartRateSettings] setMax(%u) failed: not initialized\n", max);
    return false;
  }
  
  HW_DEBUG_PRINTF("[HeartRateSettings] setMax(%u) called, current min=%u, max=%u\n", 
                 max, _minHeartRate, _maxHeartRate);
  
  // Validate: max must be <= HARD_MAX and > min + MIN_GAP
  uint16_t minAllowed = _minHeartRate + MIN_GAP;
  HW_DEBUG_PRINTF("[HeartRateSettings] Validating: max must be in (%u, %u] (minAllowed=%u, must be > %u)\n",
                 minAllowed, HARD_MAX, minAllowed, minAllowed);
  
  // Check if max can be valid (must be > min + MIN_GAP, so at least min + MIN_GAP + 1)
  if (max <= minAllowed) {
    HW_DEBUG_PRINTF("[HeartRateSettings] setMax(%u) rejected: must be > %u (min %u + MIN_GAP %u)\n",
                   max, minAllowed, _minHeartRate, MIN_GAP);
  }
  
  uint16_t clamped = clamp(max, minAllowed + 1, HARD_MAX);  // +1 because must be > minAllowed
  
  if (clamped != max) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Clamped max from %u to %u\n", max, clamped);
  }
  
  _maxHeartRate = clamped;
  
  if (saveToNVS()) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Max heart rate set to %u (saved to NVS)\n", _maxHeartRate);
    
#if HOMEWIND_ENABLE_WEBSOCKET
    // Notify WebSocket telemetry of change
    extern WebSocketTelemetry* g_webSocketTelemetry;
    if (g_webSocketTelemetry) {
      g_webSocketTelemetry->markDirty(TelemetryFrameType::HEART_RATE_SETTINGS);
    }
#endif
    
    return true;
  }
  
  HW_ERROR_PRINTLN("[HeartRateSettings] Failed to save max heart rate to NVS");
  return false;
}

bool HeartRateSettings::setRange(uint16_t min, uint16_t max) {
  if (!_initialized) {
    return false;
  }
  
  if (!validateRange(min, max)) {
    HW_ERROR_PRINTLN("[HeartRateSettings] Invalid range");
    return false;
  }
  
  _minHeartRate = min;
  _maxHeartRate = max;
  
  if (saveToNVS()) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Range set: min=%u, max=%u\n", 
                    _minHeartRate, _maxHeartRate);
    return true;
  }
  
  HW_ERROR_PRINTLN("[HeartRateSettings] Failed to save range to NVS");
  return false;
}

bool HeartRateSettings::loadFromNVS() {
  Preferences prefs;
  
  if (!prefs.begin(NVS_NAMESPACE, true)) {  // Read-only mode
    HW_DEBUG_PRINTLN("[HeartRateSettings] Failed to open NVS namespace");
    return false;
  }
  
  // Try to load new blob format first
  NVSData data;
  size_t len = prefs.getBytesLength(NVS_KEY_DATA);
  
  if (len == sizeof(NVSData)) {
    // New format exists
    prefs.getBytes(NVS_KEY_DATA, &data, sizeof(data));
    prefs.end();
    
    // Verify CRC
    uint16_t storedCrc = data.crc16;
    data.crc16 = 0;
    uint16_t calcedCrc = Crc16::calcCRC16((uint8_t*)&data, sizeof(data) - sizeof(data.crc16));
    
    if (storedCrc != calcedCrc) {
      HW_ERROR_PRINTF("[HeartRateSettings] CRC mismatch: stored=%04X, calculated=%04X\n", 
                      storedCrc, calcedCrc);
      return false;
    }
    
    // Check version
    if (data.version != NVS_DATA_VERSION) {
      HW_DEBUG_PRINTF("[HeartRateSettings] Version mismatch: stored=%u, expected=%u\n",
                      data.version, NVS_DATA_VERSION);
      // Future: handle migrations between versions here
      return false;
    }
    
    // Validate loaded values
    if (!validateRange(data.minHR, data.maxHR)) {
      HW_DEBUG_PRINTLN("[HeartRateSettings] Invalid values in NVS, using defaults");
      return false;
    }
    
    _minHeartRate = data.minHR;
    _maxHeartRate = data.maxHR;
    
    HW_DEBUG_PRINTLN("[HeartRateSettings] Loaded from new blob format");
    return true;
  }
  
  prefs.end();
  
  HW_DEBUG_PRINTLN("[HeartRateSettings] No config found in NVS, using defaults");
  return false;
}

bool HeartRateSettings::saveToNVS() {
  Preferences prefs;
  
  if (!prefs.begin(NVS_NAMESPACE, false)) {  // Read-write mode
    HW_ERROR_PRINTLN("[HeartRateSettings] Failed to open NVS namespace for writing");
    return false;
  }
  
  // Prepare data structure
  NVSData data;
  memset(&data, 0, sizeof(data));
  data.version = NVS_DATA_VERSION;
  data.minHR = _minHeartRate;
  data.maxHR = _maxHeartRate;
  // reserved[] is already zeroed by memset
  
  // Calculate CRC (excluding crc16 field itself)
  data.crc16 = 0;
  data.crc16 = Crc16::calcCRC16((uint8_t*)&data, sizeof(data) - sizeof(data.crc16));
  
  // Write blob
  size_t written = prefs.putBytes(NVS_KEY_DATA, &data, sizeof(data));
  prefs.end();
  
  if (written != sizeof(data)) {
    HW_ERROR_PRINTF("[HeartRateSettings] Failed to write NVS: wrote %u, expected %u\n",
                    written, sizeof(data));
    return false;
  }
  
  HW_DEBUG_PRINTF("[HeartRateSettings] Saved to NVS (blob): min=%u, max=%u, crc=%04X\n", 
                  _minHeartRate, _maxHeartRate, data.crc16);
  return true;
}

bool HeartRateSettings::validateRange(uint16_t min, uint16_t max) const {
  // Check hard limits
  if (min < HARD_MIN || min > HARD_MAX) {
    return false;
  }
  if (max < HARD_MIN || max > HARD_MAX) {
    return false;
  }
  
  // Check gap requirement
  if (max <= min + MIN_GAP) {
    return false;
  }
  
  return true;
}

uint16_t HeartRateSettings::clamp(uint16_t value, uint16_t min, uint16_t max) {
  if (value < min) {
    return min;
  }
  if (value > max) {
    return max;
  }
  return value;
}


#endif // HW_ENABLE_NVS
