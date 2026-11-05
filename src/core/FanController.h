/**
 * @file FanController.h
 * @brief Fan controller on SmartMiFan 2.0 (slot index = NVS place)
 */

#ifndef HOMEWIND_FAN_CONTROLLER_H
#define HOMEWIND_FAN_CONTROLLER_H

#include <Arduino.h>
#include <strings.h>
#include "../app/Config.h"

#if HW_ENABLE_FANS

#include <WiFiUdp.h>
#include <Preferences.h>
#include <SmartMiFanAsync.h>

#if defined(ARDUINO_ARCH_ESP32)
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#endif

#if HOMEWIND_ENABLE_WEBSOCKET
class WebSocketTelemetry;
#endif

struct FanConfig {
  char token[33];
  IPAddress ip;
  char model[24];
  bool enabled;
  bool used;

  FanConfig() : ip(0, 0, 0, 0), enabled(true), used(false) {
    token[0] = '\0';
    model[0] = '\0';
  }

  void setToken(const char* t) {
    if (t) {
      strncpy(token, t, sizeof(token) - 1);
      token[sizeof(token) - 1] = '\0';
    } else {
      token[0] = '\0';
    }
  }

  void setToken(const String& t) { setToken(t.c_str()); }

  bool tokenEquals(const char* other) const {
    if (!other) return token[0] == '\0';
    return strcasecmp(token, other) == 0;
  }

  bool tokenEquals(const String& other) const { return tokenEquals(other.c_str()); }

  void clear() {
    token[0] = '\0';
    model[0] = '\0';
    ip = IPAddress(0, 0, 0, 0);
    enabled = true;
    used = false;
  }
};

class FanHRAdapter;

class FanController {
  friend class FanHRAdapter;

public:
  static const uint8_t MAX_FANS = 4;
  static_assert(MAX_FANS == SMARTMIFAN_MAX_FANS, "FanController slots must match the library");

  FanController();
  ~FanController();

#if HOMEWIND_ENABLE_WEBSOCKET
  bool begin(WebSocketTelemetry* telemetry = nullptr);
#else
  bool begin();
#endif

  void loop();
  bool ready() const { return initialized; }
  void stopForMaintenance();
  bool isStoppedForMaintenance() const { return _maintenanceStopped; }

  bool addFan(const String& token);
  bool removeFan(const String& token);
  bool removeFanFast(const String& token);
  void removeFanCleanup(const String& token);
  bool updateFanToken(const String& oldToken, const String& newToken);
  bool setFanControlState(const String& token, bool active);

  uint8_t getFanCount() const { return _fanCount; }
  uint8_t getConnectedFanCount() const;
  bool isUsed(uint8_t index) const { return index < MAX_FANS && _fans[index].used; }
  bool getFanConfig(uint8_t index, FanConfig& config) const;
  bool getFanConfigByToken(const String& token, FanConfig& config) const;
  bool getDiscoveredFans(SmartMiFanInfo* fans, size_t& count) const;

  void markTelemetryDirty();
  uint8_t getFanRecoveryUiState(uint8_t index) const;
  uint8_t getPostConnectSpeed() const { return _postConnectSpeed; }
  bool isFanActiveForHr(uint8_t index) const;
  bool setPostConnectSpeed(uint8_t speed, bool persist = true);
  bool isPostConnectDone() const;
  uint32_t getHandshakeTtlS() const { return _handshakeTtlS; }
  bool setHandshakeTtlS(uint32_t seconds, bool persist = true);

  bool enqueueSetSpeed(uint8_t index, uint8_t pct);
  bool enqueueSetPower(uint8_t index, bool on);

private:
  static constexpr unsigned long FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS = HW_FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS;
  static constexpr const char* NVS_NAMESPACE = "homewind";
  static constexpr const char* NVS_KEY_DATA = "fans_data";

  #pragma pack(push, 1)
  struct NVSFanEntry {
    char token[33];
    uint8_t ip[4];
    char model[24];
    uint8_t enabled;
    uint8_t used;
  };

  struct NVSFanData {
    uint8_t version;
    uint8_t fanCount;
    uint8_t postConnectSpeed;
    uint8_t handshakeTtlS;
    NVSFanEntry fans[MAX_FANS];
    uint16_t crc16;
  };
  #pragma pack(pop)

  static_assert(sizeof(NVSFanEntry) == 63, "NVSFanEntry size mismatch");
  static_assert(sizeof(NVSFanData) == 258, "NVSFanData size mismatch");
  static constexpr uint8_t NVS_DATA_VERSION = 3;

  enum class FanReqType : uint8_t {
    SET_ENABLED,
    SET_SPEED,
    SET_POWER,
    REMOVE_CLEANUP,
    ADD_SLOT,
    UPDATE_SLOT,
    SMART_CONNECT
  };
  struct FanReq {
    FanReqType type;
    uint8_t index;
    uint8_t value;
  };

  enum class PostConnectStep : uint8_t {
    IDLE,
    POWER_ON,
    WAIT_STABILIZE,
    SET_SPEED,
    WAIT_RUN,
    POWER_OFF,
    DONE
  };

  bool initialized;
  bool _maintenanceStopped;
  uint8_t _postConnectSpeed;
  uint32_t _handshakeTtlS;
  WiFiUDP _fanUdp;

#if HOMEWIND_ENABLE_WEBSOCKET
  WebSocketTelemetry* _telemetry;
#endif

  FanConfig _fans[MAX_FANS];
  uint8_t _fanCount;

  bool _discoveryIsRecovery;
  int _discoveryRecoveryNvsIndex;
  bool _connectInProgress;
  bool _postConnectCompleted;
  PostConnectStep _postConnectStep;
  unsigned long _postConnectStepStartTime;
  uint8_t _postConnectSeq;

  static constexpr unsigned long FAN_CONTROL_THROTTLE_MS = 500;
  unsigned long _lastControlStateChangeMs[MAX_FANS];
  static constexpr unsigned long FAN_RECOVERY_DISCOVERY_THROTTLE_MS = 5000;
  unsigned long _lastRecoveryDiscoveryAttemptMs[MAX_FANS];

  uint8_t _autoRecoveryAttempts[MAX_FANS];
  bool _autoRecoveryExhausted[MAX_FANS];
  bool _autoRecoveryActive[MAX_FANS];
  unsigned long _lastAutoRecoveryAttemptMs[MAX_FANS];
  uint8_t _unhealthyStreak[MAX_FANS];
  unsigned long _lastUnhealthyCheckMs[MAX_FANS];
  bool _softActive[MAX_FANS];

  static constexpr uint8_t REQ_DEPTH = 8;
  FanReq _req[REQ_DEPTH];
  uint8_t _reqHead;
  uint8_t _reqCount;
#if defined(ARDUINO_ARCH_ESP32)
  SemaphoreHandle_t _reqMux;
#endif

  uint8_t _cleanupIndex;
  uint32_t _cleanupStartMs;
  bool _cleanupPending;
  bool _cleanupSpeedDone;
  bool _cleanupPowerDone;

  bool loadFansFromNVS();
  bool saveFansToNVS();
  int findFanIndex(const String& token) const;
  int firstFreePlace() const;
  void recountUsed();
  void startDiscoveryForIndex(uint8_t index, bool recovery);
  void startSmartConnect();
  void startPostConnectionControl();
  void updatePostConnectionControl();
  void updateAutoRecovery();
  void handleEvent(const EventInfo& ev);
  void onConnectOrDiscoveryDone(const EventInfo& ev);
  void syncEnabledToLibrary();
  bool enqueue(FanReqType type, uint8_t index, uint8_t value);
  void drainQueue();
  void clearSeqWaiters();
  static void onLibraryEvent(const EventInfo& ev);
};

#endif // HW_ENABLE_FANS

#endif // HOMEWIND_FAN_CONTROLLER_H
