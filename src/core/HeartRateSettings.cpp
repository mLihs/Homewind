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
#include "../core/MaintenanceMode.h"

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
#endif

#if HW_ENABLE_NVS

HeartRateSettings::HeartRateSettings()
  : _minHeartRate(DEFAULT_MIN)
  , _maxHeartRate(DEFAULT_MAX)
  , _initialized(false)
  , _mux(portMUX_INITIALIZER_UNLOCKED)
  , _pendingValid(false)
  , _pendingMin(DEFAULT_MIN)
  , _pendingMax(DEFAULT_MAX)
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

void HeartRateSettings::requestBaseLocked(uint16_t& min, uint16_t& max) const {
  min = _pendingValid ? _pendingMin : _minHeartRate;
  max = _pendingValid ? _pendingMax : _maxHeartRate;
}

bool HeartRateSettings::requestMin(uint16_t min, uint16_t& outMin, uint16_t& outMax) {
  if (!_initialized || MaintenanceMode::admissionClosed()) {
    return false;
  }
  
  portENTER_CRITICAL(&_mux);
  uint16_t baseMin;
  uint16_t baseMax;
  requestBaseLocked(baseMin, baseMax);
  // min must be >= HARD_MIN and <= max - MIN_GAP
  _pendingMin = clamp(min, HARD_MIN, baseMax - MIN_GAP);
  _pendingMax = baseMax;
  _pendingValid = true;
  outMin = _pendingMin;
  outMax = _pendingMax;
  portEXIT_CRITICAL(&_mux);
  
  HW_DEBUG_PRINTF("[HeartRateSettings] Min %u queued: min=%u, max=%u\n", min, outMin, outMax);
  return true;
}

bool HeartRateSettings::requestMax(uint16_t max, uint16_t& outMin, uint16_t& outMax) {
  if (!_initialized || MaintenanceMode::admissionClosed()) {
    return false;
  }
  
  portENTER_CRITICAL(&_mux);
  uint16_t baseMin;
  uint16_t baseMax;
  requestBaseLocked(baseMin, baseMax);
  // max must be >= min + MIN_GAP and <= HARD_MAX
  _pendingMin = baseMin;
  _pendingMax = clamp(max, baseMin + MIN_GAP, HARD_MAX);
  _pendingValid = true;
  outMin = _pendingMin;
  outMax = _pendingMax;
  portEXIT_CRITICAL(&_mux);
  
  HW_DEBUG_PRINTF("[HeartRateSettings] Max %u queued: min=%u, max=%u\n", max, outMin, outMax);
  return true;
}

void HeartRateSettings::loop() {
  if (!_initialized) {
    return;
  }
  
  // Take and apply in one section: a request in between would otherwise base
  // on the old range and overwrite this change.
  // From maintenance REQUESTED on, NVS stays untouched (a factory reset erases it).
  portENTER_CRITICAL(&_mux);
  const bool have = _pendingValid;
  const uint16_t min = _pendingMin;
  const uint16_t max = _pendingMax;
  _pendingValid = false;
  const bool apply = have && !MaintenanceMode::admissionClosed();
  if (apply) {
    _minHeartRate = min;
    _maxHeartRate = max;
  }
  portEXIT_CRITICAL(&_mux);
  if (!have) {
    return;
  }
  if (!apply) {
    HW_DEBUG_PRINTLN("[HeartRateSettings] Queued change discarded (maintenance)");
    return;
  }
  
  if (saveToNVS()) {
    HW_DEBUG_PRINTF("[HeartRateSettings] Range applied: min=%u, max=%u (saved to NVS)\n", min, max);
  } else {
    HW_ERROR_PRINTLN("[HeartRateSettings] Failed to save heart rate range to NVS");
  }
  
#if HOMEWIND_ENABLE_WEBSOCKET
  extern WebSocketTelemetry* g_webSocketTelemetry;
  if (g_webSocketTelemetry) {
    g_webSocketTelemetry->markDirty(TelemetryFrameType::HEART_RATE_SETTINGS);
  }
#endif
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
  if (max < min + MIN_GAP) {
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
