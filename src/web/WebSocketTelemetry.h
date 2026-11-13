/**
 * @file WebSocketTelemetry.h
 * @brief WebSocket binary telemetry (server → client only)
 * 
 * Protocol v1:
 * - Frame format: [type:uint8][payload:variable]
 * - Type byte indicates frame type (see TelemetryFrameType)
 * - Payload is frame-specific binary data
 * - Snapshot frames: Full state (sent on connect, periodic refresh)
 * - Delta frames: Incremental updates (sent when state changes)
 * 
 * Architecture:
 * - Dirty-flag model: Mark frames dirty from async contexts
 * - Loop-flush: Send dirty frames from main loop (not async callbacks)
 * - Reconnect safety: Always send full snapshot on client connect
 * - Low memory: Static buffers, no heap allocation
 */

#ifndef HOMEWIND_WEBSOCKET_TELEMETRY_H
#define HOMEWIND_WEBSOCKET_TELEMETRY_H

#include <Arduino.h>
#include <atomic>
#include "../app/Config.h"

#if HOMEWIND_ENABLE_WEBSOCKET
// Feature gate: ESPAsyncWebServer is required for WebSocket
// WebSocket depends on HW_ENABLE_WEB (see Config.h), so ESPAsyncWebServer should be available
#include <ESPAsyncWebServer.h>

/**
 * WebSocket frame types (binary protocol v1)
 * 
 * Protocol format: [type:uint8][payload:variable]
 * 
 * Snapshot frames (full state):
 * - SYSTEM_STATUS (0x01): System health, uptime, memory
 * - SENSORS_SNAPSHOT (0x02): All configured sensors
 * - FANS_SNAPSHOT (0x07): All configured fans
 * 
 * Delta frames (incremental updates):
 * - SENSOR_CONN_STATE (0x03): Single sensor connection state change
 * - HEART_RATE (0x06): Heart rate value update
 * - DISCOVERY_STATUS (0x04): Discovery state change
 * - DISCOVERY_RESULTS (0x05): New discovery results
 * - FIRMWARE_VERSION (0x08): Firmware version update
 * - FIRMWARE_PROGRESS (0x09): Firmware update progress
 * - FIRMWARE_SEARCH_RESULT (0x0A): Firmware search result
 * - BIKE_METRICS (0x0D): Power and cadence (also sent once per client on connect)
 * - FAN_CONTROL (0x0E): Fan mode and acknowledged fan target (HW_ENABLE_FAN_POWER_MODE only)
 *
 * 0x0C is the WebUI control frame CLIENT_ID (not a telemetry frame).
 */
enum class TelemetryFrameType : uint8_t {
  SYSTEM_STATUS = 0x01,      // Snapshot: System health
  SENSORS_SNAPSHOT = 0x02,   // Snapshot: All sensors
  SENSOR_CONN_STATE = 0x03,  // Delta: Single sensor connection state
  DISCOVERY_STATUS = 0x04,   // Delta: Discovery active/stopped
  DISCOVERY_RESULTS = 0x05,  // Delta: Discovery results
  HEART_RATE = 0x06,         // Delta: Heart rate value
  FANS_SNAPSHOT = 0x07,      // Snapshot: All fans
  FIRMWARE_VERSION = 0x08,   // Delta: Firmware version
  FIRMWARE_PROGRESS = 0x09,  // Delta: Firmware update progress
  FIRMWARE_SEARCH_RESULT = 0x0A,  // Delta: Firmware search result
  HEART_RATE_SETTINGS = 0x0B,     // Snapshot/Delta: Heart rate min/max settings
  BIKE_METRICS = 0x0D,            // Delta: Power and cadence (hw::BikeMetrics)
  FAN_CONTROL = 0x0E,             // Delta: Fan mode and target (hw::FanControlStatus)
};

/**
 * @class WebSocketTelemetry
 * @brief WebSocket binary telemetry manager
 * 
 * Usage:
 * 1. Call begin() to initialize WebSocket server
 * 2. Call markDirty() from async contexts when state changes
 * 3. Call loopFlush() from main loop() to send dirty frames
 * 
 * Reconnect safety:
 * - On client connect, full snapshot is automatically sent
 * - Subsequent updates use delta frames
 */
class WebSocketTelemetry {
public:
  WebSocketTelemetry();
  ~WebSocketTelemetry();
  
  /**
   * Initialize WebSocket server
   * @param webServer AsyncWebServer instance to attach WebSocket to
   * @return true on success
   */
  bool begin(AsyncWebServer* webServer);
  
  /**
   * Flush dirty frames (call from App::loop())
   * Sends all frames marked dirty since last flush
   */
  void loopFlush();
  
  /**
   * Mark frame type as dirty (needs to be sent)
   * Safe to call from async contexts (BLE callbacks, etc.)
   * @param frameType Frame type to mark dirty
   */
  void markDirty(TelemetryFrameType frameType);
  
  /**
   * Get number of connected WebSocket clients
   * @return Client count (0 if not initialized)
   */
  uint8_t getClientCount() const;
  
  /**
   * Request SENSORS + FANS snapshots for one client (unicast).
   * Safe from any task: only records the client id; loopFlush() builds and sends.
   * Avoids broadcasting heavy snapshots in multi-client scenarios.
   */
  void requestSnapshotsForClient(uint32_t clientId);
  
  /**
   * Get WebSocket instance (for integration)
   * @return Pointer to AsyncWebSocket, or nullptr if not initialized
   */
  AsyncWebSocket* getWebSocket() { return wsSocket; }
  
  /**
   * Check if telemetry is initialized and ready
   * @return true if ready
   */
  bool ready() const { return initialized; }

private:
  bool initialized;
  AsyncWebSocket* wsSocket;
  
  // Dirty flags: bitfield of frame types that need to be sent (any task sets, loopFlush drains)
  std::atomic<uint16_t> dirtyFlags;

#if HW_ENABLE_TELEMETRY_DIAG && HW_ENABLE_DEBUG_LOGS
  // Telemetry diagnostics (compile-time only)
  uint32_t diagLastSummaryMs;
  uint16_t diagBuiltMask60s;
  uint16_t diagBuiltMaskLastFlush;
#endif
  
  // Last sent snapshot timestamp (for periodic refresh)
  unsigned long lastSnapshotTime;
  static const unsigned long SNAPSHOT_INTERVAL_MS = 30000; // 30 seconds

  // Heavy snapshots (SENSORS/FANS) scheduling:
  // - With 1 client: can be frequent.
  // - With 2+ clients: budgeted and staggered to reduce heap fragmentation.
  unsigned long lastHeavySnapshotTime;
  bool heavyNextIsSensors;
  
  // Staged connect: spread heavy snapshots over multiple flush cycles
  // 0 = no staged connect pending
  // 1 = first stage (SYSTEM_STATUS sent, need SENSORS next)
  // 2 = second stage (SENSORS sent, need FANS next)
  // 3+ = done
  uint8_t stagedConnectPhase;
  unsigned long stagedConnectStartMs;
  uint32_t stagedConnectTargetClientId;

  // Per-client snapshot requests (any task writes under the mux, loopFlush drains)
  static const uint8_t SNAPSHOT_REQUEST_SLOTS = 4;
  portMUX_TYPE snapshotRequestMux;
  uint32_t snapshotRequests[SNAPSHOT_REQUEST_SLOTS];  // 0 = free
  
  // Frame building buffers (static to avoid heap allocation; loop task only)
  static const size_t MAX_FRAME_SIZE = 512; // Type byte + payload (largest payload: DISCOVERY_RESULTS, 500 bytes)
  uint8_t frameBuffer[MAX_FRAME_SIZE];
  
  /**
   * WebSocket event handler
   */
  static void onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, 
                               AwsEventType type, void* arg, uint8_t* data, size_t len);
  
  /**
   * Send frame to all connected clients
   * @param frameType Frame type
   * @param payload Frame payload data
   * @param payloadLen Payload length
   */
  void sendFrame(TelemetryFrameType frameType, const uint8_t* payload, size_t payloadLen);

  /**
   * Build and send a frame to a single client (unicast). Loop task only.
   * Used for connect snapshots to avoid binaryAll() bursts when 2+ clients are connected.
   */
  void emitFrameToClient(uint32_t clientId, TelemetryFrameType frameType);
  
  /**
   * Build and send frame for given type. Loop task only.
   * @param frameType Frame type to build and send
   */
  void emitFrame(TelemetryFrameType frameType);
  
  /**
   * Build SYSTEM_STATUS frame (snapshot)
   */
  size_t buildSystemStatus(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build SENSORS_SNAPSHOT frame (snapshot)
   */
  size_t buildSensorsSnapshot(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build FANS_SNAPSHOT frame (snapshot)
   */
  size_t buildFansSnapshot(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build SENSOR_CONN_STATE frame (delta)
   */
  size_t buildSensorConnState(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build HEART_RATE frame (delta)
   */
  size_t buildHeartRate(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build DISCOVERY_STATUS frame (delta)
   */
  size_t buildDiscoveryStatus(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build DISCOVERY_RESULTS frame (delta)
   */
  size_t buildDiscoveryResults(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build FIRMWARE_VERSION frame (delta)
   */
  size_t buildFirmwareVersion(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build FIRMWARE_PROGRESS frame (delta)
   */
  size_t buildFirmwareProgress(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build FIRMWARE_SEARCH_RESULT frame (delta)
   */
  size_t buildFirmwareSearchResult(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Build HEART_RATE_SETTINGS frame (snapshot/delta)
   */
  size_t buildHeartRateSettings(uint8_t* buffer, size_t bufferSize);

  /**
   * Build BIKE_METRICS frame (delta)
   */
  size_t buildBikeMetrics(uint8_t* buffer, size_t bufferSize);

  /**
   * Build FAN_CONTROL frame (delta)
   */
  size_t buildFanControl(uint8_t* buffer, size_t bufferSize);
  
  /**
   * Send full snapshot to newly connected client
   * Called automatically on client connect
   */
  void sendFullSnapshot();
  
  /**
   * Helper: Write string to buffer (length-prefixed)
   * @return Number of bytes written
   */
  size_t writeString(uint8_t* buffer, size_t offset, const char* str, size_t maxLen);
  
  /**
   * Helper: Write uint8_t to buffer
   */
  size_t writeUint8(uint8_t* buffer, size_t offset, uint8_t value);
  
  /**
   * Helper: Write uint16_t to buffer (little-endian)
   */
  size_t writeUint16(uint8_t* buffer, size_t offset, uint16_t value);
  
  /**
   * Helper: Write uint32_t to buffer (little-endian)
   */
  size_t writeUint32(uint8_t* buffer, size_t offset, uint32_t value);
};
#endif // HOMEWIND_ENABLE_WEBSOCKET

#endif // HOMEWIND_WEBSOCKET_TELEMETRY_H

