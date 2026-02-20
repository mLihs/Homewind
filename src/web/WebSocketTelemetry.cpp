/**
 * @file WebSocketTelemetry.cpp
 * @brief WebSocket binary telemetry implementation
 * 
 * Protocol v1 Binary Format:
 * 
 * Frame structure: [type:uint8][payload:variable]
 * 
 * SYSTEM_STATUS (0x01) - Snapshot:
 *   [uptime_ms:uint32][free_heap:uint32][largest_free_block:uint32]
 *   Total: 12 bytes
 * 
 * SENSORS_SNAPSHOT (0x02) - Snapshot:
 *   [count:uint8][sensor1...sensorN]
 *   Sensor entry: [name_len:uint8][name:bytes][type:uint8][mac_len:uint8][mac:bytes][connected:uint8][battery:uint8]
 * 
 * FANS_SNAPSHOT (0x07) - Snapshot:
 *   [count:uint8][fan1...fanN]
 *   Fan entry: [token_len:uint8][token:bytes][connected:uint8][control_active:uint8]
 * 
 * SENSOR_CONN_STATE (0x03) - Delta:
 *   [name_len:uint8][name:bytes][connected:uint8]
 * 
 * HEART_RATE (0x06) - Delta:
 *   [value:uint16]
 * 
 * DISCOVERY_STATUS (0x04) - Delta:
 *   [active:uint8][type:uint8]
 * 
 * DISCOVERY_RESULTS (0x05) - Delta:
 *   [count:uint8][result1...resultN]
 *   Result entry: [name_len:uint8][name:bytes][type:uint8][mac_len:uint8][mac:bytes]
 * 
 * FIRMWARE_VERSION (0x08) - Delta:
 *   [version_len:uint8][version:bytes]
 * 
 * FIRMWARE_PROGRESS (0x09) - Delta:
 *   [percent:uint8]
 * 
 * FIRMWARE_SEARCH_RESULT (0x0A) - Delta:
 *   [has_update:uint8][version_len:uint8][version:bytes][notes_len:uint16][notes:bytes]
 *   Note: version and notes only present if has_update=1
 */

#include "WebSocketTelemetry.h"
#include "../core/SystemState.h"
#include "../core/DebugLog.h"
#include "../core/HeapDiagnostics.h"
#include "../app/BuildInfo.h"

#if HW_ENABLE_BLE
#include "../core/BLERelayManager.h"
#include <BluetoothBikeSensorServer.h>
// Forward declaration - will be set by App
extern BLERelayManager* g_bleRelayManager;
#endif

#if HW_ENABLE_FANS
// SystemState.h is already included above, so namespace SystemState is defined
// Workaround for SystemState name conflict: rename SmartMiFanAsync's enum
#define SystemState SmartMiFanAsync_SystemState
#include "../core/FanController.h"
#include <SmartMiFanAsync.h>
#undef SystemState
// Forward declaration - will be set by App
extern FanController* g_fanController;
#endif

#if HW_ENABLE_OTA
#include "../core/FirmwareUpdateManager.h"
#endif

#if HW_ENABLE_NVS
#include "../core/HeartRateSettings.h"
#endif

#include <strings.h>  // strcasecmp

#if HOMEWIND_ENABLE_WEBSOCKET

#include <new>  // For placement-new

// Static instance pointer for event handler callback
static WebSocketTelemetry* g_telemetryInstance = nullptr;

// Static buffer for AsyncWebSocket (placement-new, no heap allocation)
alignas(AsyncWebSocket) static uint8_t s_wsBuffer[sizeof(AsyncWebSocket)];

WebSocketTelemetry::WebSocketTelemetry()
  : initialized(false)
  , wsSocket(nullptr)
  , dirtyFlags(0)
  , lastSnapshotTime(0)
  , lastHeavySnapshotTime(0)
  , heavyNextIsSensors(true)
  , stagedConnectPhase(0)
  , stagedConnectStartMs(0)
  , stagedConnectTargetClientId(0)
#if HW_ENABLE_TELEMETRY_DIAG && HW_ENABLE_DEBUG_LOGS
  , diagLastSummaryMs(0)
  , diagBuiltMask60s(0)
  , diagBuiltMaskLastFlush(0)
#endif
{
}

WebSocketTelemetry::~WebSocketTelemetry() {
  if (wsSocket) {
    // Explicit destructor call for placement-new object
    wsSocket->~AsyncWebSocket();
    wsSocket = nullptr;
  }
  if (g_telemetryInstance == this) {
    g_telemetryInstance = nullptr;
  }
}

bool WebSocketTelemetry::begin(AsyncWebServer* webServer) {
  if (initialized) {
    return true;
  }
  
  if (!webServer) {
    HW_ERROR_PRINTLN("[WebSocketTelemetry] ERROR: AsyncWebServer is null");
    return false;
  }
  
  // Placement-new: construct in static buffer (no heap allocation)
  wsSocket = new (s_wsBuffer) AsyncWebSocket("/ws");
  
  // Max clients limit is enforced in WS_EVT_CONNECT event handler
  // (AsyncWebSocket doesn't have setMaxClients() method)
  
  // Set event handler (static callback)
  g_telemetryInstance = this;
  wsSocket->onEvent(onWebSocketEvent);
  
  // Register WebSocket with AsyncWebServer
  webServer->addHandler(wsSocket);
  
  initialized = true;
  lastSnapshotTime = millis();
  lastHeavySnapshotTime = lastSnapshotTime;
  
  HW_DEBUG_PRINTLN("[WebSocketTelemetry] Initialized on /ws (placement-new)");
  
  return true;
}

void WebSocketTelemetry::loopFlush() {
  if (!wsSocket || !initialized) {
    return;
  }
  
  // AsyncWebSocket doesn't need explicit loop() calls, but we can cleanup disconnected clients
  wsSocket->cleanupClients();
  
  const uint8_t clientCount = wsSocket->count();
  
  // OPTIMIZATION: If no clients connected, let heap rest - don't mark dirty, don't build frames, no staging
  if (clientCount == 0) {
    // Clear any pending dirty flags to prevent burst on next connect
    // (they will be re-marked by the connect handler anyway)
    dirtyFlags = 0;
    stagedConnectPhase = 0;  // Reset staged connect
    lastSnapshotTime = millis();
    lastHeavySnapshotTime = lastSnapshotTime;
    return;
  }
  
  unsigned long now = millis();
  
  // OPTIMIZATION: Process staged connect - spread heavy snapshots over time
  // Delay between stages: 150ms to let TCP buffers settle
  // IMPORTANT: Stop staging if clients disconnect during staged connect
  static const unsigned long STAGED_DELAY_MS = 150;
  if (stagedConnectPhase > 0 && stagedConnectPhase <= 3) {
    // Stop staging if target client is gone.
    // IMPORTANT: Don't keep/deref AsyncWebSocketClient* across calls (client list can change concurrently).
    if (!wsSocket->hasClient(stagedConnectTargetClientId)) {
      stagedConnectPhase = 0;
      return;
    }
    
    if (now - stagedConnectStartMs >= STAGED_DELAY_MS * stagedConnectPhase) {
      switch (stagedConnectPhase) {
        case 1:
          buildAndSendFrameToClient(stagedConnectTargetClientId, TelemetryFrameType::SENSORS_SNAPSHOT);
          HW_DEBUG_PRINTLN("[WebSocketTelemetry] Staged connect: SENSORS_SNAPSHOT");
          break;
        case 2:
          buildAndSendFrameToClient(stagedConnectTargetClientId, TelemetryFrameType::FANS_SNAPSHOT);
          HW_DEBUG_PRINTLN("[WebSocketTelemetry] Staged connect: FANS_SNAPSHOT");
          break;
        case 3:
          // Done with staged connect
          HW_DEBUG_PRINTLN("[WebSocketTelemetry] Staged connect complete");
          break;
      }
      stagedConnectPhase++;
    }
  }
  
  // Check if periodic SYSTEM_STATUS snapshot is due (only when clients connected)
  if (now - lastSnapshotTime >= SNAPSHOT_INTERVAL_MS) {
    markDirty(TelemetryFrameType::SYSTEM_STATUS);
    lastSnapshotTime = now;
  }

  // Budgeted periodic heavy snapshots:
  // - With 1 client: refresh both heavy snapshots on the normal interval.
  // - With 2+ clients: refresh one heavy snapshot at a slower interval (alternate SENSORS/FANS).
  // IMPORTANT: Skip periodic heavy while staged connect is running (avoid burst overlap).
  static const unsigned long MULTI_CLIENT_HEAVY_INTERVAL_MS = 120000; // 2 minutes
  if (stagedConnectPhase == 0 || stagedConnectPhase > 3) {
    const unsigned long heavyInterval = (clientCount <= 1) ? SNAPSHOT_INTERVAL_MS : MULTI_CLIENT_HEAVY_INTERVAL_MS;
    if (now - lastHeavySnapshotTime >= heavyInterval) {
      if (clientCount <= 1) {
        markDirty(TelemetryFrameType::SENSORS_SNAPSHOT);
        markDirty(TelemetryFrameType::FANS_SNAPSHOT);
      } else {
        markDirty(heavyNextIsSensors ? TelemetryFrameType::SENSORS_SNAPSHOT
                                     : TelemetryFrameType::FANS_SNAPSHOT);
        heavyNextIsSensors = !heavyNextIsSensors;
      }
      lastHeavySnapshotTime = now;
    }
  }

#if HW_ENABLE_TELEMETRY_DIAG && HW_ENABLE_DEBUG_LOGS
  // Periodic summary every HW_TELEMETRY_DIAG_SUMMARY_MS
  if (diagLastSummaryMs == 0) {
    diagLastSummaryMs = now;
  }
  if ((now - diagLastSummaryMs) >= HW_TELEMETRY_DIAG_SUMMARY_MS) {
    const uint32_t freeHeap = ESP.getFreeHeap();
    const uint32_t largestBlock = ESP.getMaxAllocHeap();
    const uint8_t clientCount = wsSocket->count();
    const uint16_t dirtyNow = dirtyFlags;
    const uint16_t builtMask = diagBuiltMask60s;
    
    HW_DEBUG_PRINTF("[TelDiag] 60s: dirty=0x%04X built=0x%04X clients=%u free=%u largest=%u\n",
                   dirtyNow, builtMask, clientCount, freeHeap, largestBlock);
    
#if HW_TELEMETRY_DIAG_PRINT_FRAME_LIST
    if (builtMask != 0) {
      static const char* kFrameNames[10] = {
        "SYSTEM_STATUS",
        "SENSORS_SNAPSHOT",
        "SENSOR_CONN_STATE",
        "DISCOVERY_STATUS",
        "DISCOVERY_RESULTS",
        "HEART_RATE",
        "FANS_SNAPSHOT",
        "FIRMWARE_VERSION",
        "FIRMWARE_PROGRESS",
        "FIRMWARE_SEARCH_RESULT",
      };
      
      HW_DEBUG_PRINT("[TelDiag] built: ");
      bool first = true;
      for (uint8_t i = 0; i < 10; i++) {
        if (builtMask & (1U << i)) {
          if (!first) HW_DEBUG_PRINT(",");
          HW_DEBUG_PRINT(kFrameNames[i]);
          first = false;
        }
      }
      HW_DEBUG_PRINTLN("");
    }
#endif
    
    diagBuiltMask60s = 0;
    diagLastSummaryMs = now;
  }
#endif
  
  // Send all dirty frames
  if (dirtyFlags != 0) {
#if HW_ENABLE_TELEMETRY_DIAG && HW_ENABLE_DEBUG_LOGS
    const uint16_t dirtyBefore = dirtyFlags;
    diagBuiltMaskLastFlush = 0;
    
    // Per-flush signal (only when we actually flush)
    HW_DEBUG_PRINTF("[TelDiag] flush: dirty=0x%04X clients=%u free=%u largest=%u\n",
                   dirtyBefore, wsSocket->count(), ESP.getFreeHeap(), ESP.getMaxAllocHeap());
#endif
    // Iterate through frame types and send dirty ones
    for (uint8_t i = 0; i < 16; i++) {
      if (dirtyFlags & (1 << i)) {
        TelemetryFrameType frameType = static_cast<TelemetryFrameType>(i + 1);
        buildAndSendFrame(frameType);
#if HW_ENABLE_TELEMETRY_DIAG && HW_ENABLE_DEBUG_LOGS
        // Track which frames were actually built/sent
        diagBuiltMaskLastFlush |= (1U << i);
#endif
      }
    }
    
    // Clear dirty flags after sending
    dirtyFlags = 0;

#if HW_ENABLE_TELEMETRY_DIAG && HW_ENABLE_DEBUG_LOGS
    diagBuiltMask60s |= diagBuiltMaskLastFlush;
#endif
  }
}

void WebSocketTelemetry::markDirty(TelemetryFrameType frameType) {
  // OPTIMIZATION: Don't mark dirty if no clients connected - reduces unnecessary state tracking
  // No exceptions: If no clients, don't track dirty state at all
  // This prevents buffer builds, AsyncWS message objects, and queue pressure
  if (wsSocket && wsSocket->count() == 0) {
    return;
  }
  
  uint8_t frameId = static_cast<uint8_t>(frameType);
  if (frameId >= 1 && frameId <= 11) {
    dirtyFlags |= (1 << (frameId - 1)); // Frame IDs are 1-11, bitfield uses 0-10
  }
}

uint8_t WebSocketTelemetry::getClientCount() const {
  if (!wsSocket || !initialized) {
    return 0;
  }
  return wsSocket->count();
}

void WebSocketTelemetry::flushFrame(TelemetryFrameType frameType) {
  if (!wsSocket || !initialized) {
    return;
  }
  
  // OPTIMIZATION: Don't build frames if no clients connected
  if (wsSocket->count() == 0) {
    return;
  }
  
  // Build and send frame immediately
  buildAndSendFrame(frameType);
  
  // Clear the dirty flag for this frame type
  uint8_t frameId = static_cast<uint8_t>(frameType);
  if (frameId >= 1 && frameId <= 11) {
    dirtyFlags &= ~(1 << (frameId - 1));
  }
}

void WebSocketTelemetry::sendFrameToClient(uint32_t clientId, TelemetryFrameType frameType) {
  // Public wrapper for safe unicast sending.
  // NOTE: This builds the frame immediately; call from main loop / HTTP handler only.
  buildAndSendFrameToClient(clientId, frameType);
}

void WebSocketTelemetry::onWebSocketEvent(AsyncWebSocket* server, AsyncWebSocketClient* client, 
                                           AwsEventType type, void* arg, uint8_t* data, size_t len) {
  if (!g_telemetryInstance || !g_telemetryInstance->wsSocket) {
    return;
  }
  
  switch (type) {
    case WS_EVT_DISCONNECT:
      // Client disconnected - cleanup is handled by AsyncWebSocket library
      // No additional cleanup needed (static buffers, no per-client state)
      HW_DEBUG_PRINTF("[WebSocketTelemetry] Client #%u disconnected\n", client->id());
      break;
      
    case WS_EVT_CONNECT: {
      IPAddress ip = client->remoteIP();
      HW_DEBUG_PRINTF("[WebSocketTelemetry] Client #%u connected from %d.%d.%d.%d\n", 
                    client->id(), ip[0], ip[1], ip[2], ip[3]);

      // Send client id to the connecting client (unicast).
      // This allows the WebUI to request unicast heavy snapshots via /telemetry/refresh?client_id=...
      // without resorting to broadcast bursts (important for cold loads / multi-client).
      {
        static const uint8_t FRAME_CLIENT_ID = 0x0C; // WebUI-only control frame: [0x0C][clientId:uint32 LE]
        uint8_t buf[1 + 4];
        buf[0] = FRAME_CLIENT_ID;
        const uint32_t cid = client->id();
        buf[1] = (uint8_t)((cid >> 0) & 0xFF);
        buf[2] = (uint8_t)((cid >> 8) & 0xFF);
        buf[3] = (uint8_t)((cid >> 16) & 0xFF);
        buf[4] = (uint8_t)((cid >> 24) & 0xFF);
        // Use AsyncWebSocket API by id to avoid AsyncWebSocketClient* lifetime issues.
        if (g_telemetryInstance->wsSocket->hasClient(cid) && g_telemetryInstance->wsSocket->availableForWrite(cid)) {
          g_telemetryInstance->wsSocket->binary(cid, buf, sizeof(buf));
        }
      }
      
      // Reject if already at max clients
      // count() includes the current connecting client, so:
      // - If 2 clients connected and 3rd connects: count() = 3, reject (3 > 2)
      // - If 1 client connected and 2nd connects: count() = 2, accept (2 > 2 = false)
      uint8_t currentCount = g_telemetryInstance->wsSocket->count();
      if (currentCount > HW_WEBSOCKET_MAX_CLIENTS) {
        HW_DEBUG_PRINTF("[WebSocketTelemetry] Client #%u rejected (max=%u, current=%u)\n",
                        client->id(), HW_WEBSOCKET_MAX_CLIENTS, currentCount);
        // Close with code 1008 (Policy Violation) so client can show error message
        client->close(1008, "MAX_CLIENTS_REACHED");
        return;  // Return early to prevent any initialization
      }
      
#if HW_ENABLE_SERIAL_COMMANDS
      // Capture first WebSocket connect heap baseline
      HeapDiagnostics::hwHeapDiagCaptureFirstWs();
#endif
      
      // FIX 2: Staged connect - spread heavy snapshots over multiple flush cycles
      // This reduces heap fragmentation by not allocating all TCP buffers at once
      // Phase 0: Send only small frames immediately (SYSTEM_STATUS, FIRMWARE_VERSION, HEART_RATE_SETTINGS)
      // Phase 1-3: Send SENSORS, FANS one at a time with ~150ms delay between
      g_telemetryInstance->markDirty(TelemetryFrameType::SYSTEM_STATUS);
      g_telemetryInstance->markDirty(TelemetryFrameType::FIRMWARE_VERSION);
      g_telemetryInstance->markDirty(TelemetryFrameType::HEART_RATE_SETTINGS);

      // IMPORTANT (heap stability): only push heavy snapshots on the FIRST client connect.
      // Repeated connect/disconnect churn while WS already has another active client
      // can erode `largest_free_block` over time (seen in HomewindBootLog13).
      //
      // The second client will still receive heavy snapshots via:
      // - periodic refresh (budgeted) and/or
      // - state-change-driven updates.
      if (currentCount == 1) {
        g_telemetryInstance->stagedConnectTargetClientId = client->id();
        g_telemetryInstance->stagedConnectPhase = 1;
        g_telemetryInstance->stagedConnectStartMs = millis();
      } else {
        g_telemetryInstance->stagedConnectPhase = 0;
      }
      break;
    }
    
    case WS_EVT_DATA:
      // Observation only - ignore incoming messages
      break;
      
    default:
      break;
  }
}

void WebSocketTelemetry::sendFrame(TelemetryFrameType frameType, const uint8_t* payload, size_t payloadLen) {
  if (!wsSocket || !initialized || !payload || payloadLen == 0) {
    return;
  }
  
  // Check if any clients are connected
  if (wsSocket->count() == 0) {
    return; // No clients, skip sending
  }
  
  // Build frame: [type:uint8][payload:variable]
  if (payloadLen + 1 > MAX_FRAME_SIZE) {
    HW_ERROR_PRINTF("[WebSocketTelemetry] ERROR: Frame too large (%zu bytes)\n", payloadLen);
    return;
  }
  
  frameBuffer[0] = static_cast<uint8_t>(frameType);
  if (payloadLen > 0) {
    memcpy(frameBuffer + 1, payload, payloadLen);
  }
  
  // Send to all connected clients (binary broadcast)
  wsSocket->binaryAll(frameBuffer, payloadLen + 1);
}

void WebSocketTelemetry::buildAndSendFrameToClient(uint32_t clientId, TelemetryFrameType frameType) {
  if (!wsSocket || !initialized) {
    return;
  }

  // IMPORTANT: Avoid wsSocket->client(id) pointer deref here.
  // The client list is owned by AsyncWebSocket and may change concurrently.
  if (!wsSocket->hasClient(clientId)) {
    return;
  }
  // If the client's queue is full, skip (prevents bursty allocations and disconnect storms).
  if (!wsSocket->availableForWrite(clientId)) {
    return;
  }

  size_t payloadLen = 0;
  switch (frameType) {
    case TelemetryFrameType::SYSTEM_STATUS:
      payloadLen = buildSystemStatus(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::SENSORS_SNAPSHOT:
      payloadLen = buildSensorsSnapshot(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FANS_SNAPSHOT:
      payloadLen = buildFansSnapshot(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::SENSOR_CONN_STATE:
      payloadLen = buildSensorConnState(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::HEART_RATE:
      payloadLen = buildHeartRate(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::DISCOVERY_STATUS:
      payloadLen = buildDiscoveryStatus(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::DISCOVERY_RESULTS:
      payloadLen = buildDiscoveryResults(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FIRMWARE_VERSION:
      payloadLen = buildFirmwareVersion(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FIRMWARE_PROGRESS:
      payloadLen = buildFirmwareProgress(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FIRMWARE_SEARCH_RESULT:
      payloadLen = buildFirmwareSearchResult(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::HEART_RATE_SETTINGS:
      payloadLen = buildHeartRateSettings(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    default:
      payloadLen = 0;
      break;
  }

  if (payloadLen == 0 || payloadLen + 1 > MAX_FRAME_SIZE) {
    return;
  }

  frameBuffer[0] = static_cast<uint8_t>(frameType);
  wsSocket->binary(clientId, frameBuffer, payloadLen + 1);
}

void WebSocketTelemetry::buildAndSendFrame(TelemetryFrameType frameType) {
  // OPTIMIZATION: Check client count BEFORE building frames
  // This prevents unnecessary buffer builds and CPU usage when no clients are connected
  if (!wsSocket || wsSocket->count() == 0) {
    return;
  }
  
  size_t payloadLen = 0;
  
  switch (frameType) {
    case TelemetryFrameType::SYSTEM_STATUS:
      payloadLen = buildSystemStatus(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::SENSORS_SNAPSHOT:
      payloadLen = buildSensorsSnapshot(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FANS_SNAPSHOT:
      payloadLen = buildFansSnapshot(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::SENSOR_CONN_STATE:
      payloadLen = buildSensorConnState(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::HEART_RATE:
      payloadLen = buildHeartRate(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::DISCOVERY_STATUS:
      payloadLen = buildDiscoveryStatus(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::DISCOVERY_RESULTS:
      payloadLen = buildDiscoveryResults(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FIRMWARE_VERSION:
      payloadLen = buildFirmwareVersion(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FIRMWARE_PROGRESS:
      payloadLen = buildFirmwareProgress(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::FIRMWARE_SEARCH_RESULT:
      payloadLen = buildFirmwareSearchResult(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    case TelemetryFrameType::HEART_RATE_SETTINGS:
      payloadLen = buildHeartRateSettings(frameBuffer + 1, MAX_FRAME_SIZE - 1);
      break;
    default:
      return; // Unknown frame type
  }
  
  if (payloadLen > 0) {
    sendFrame(frameType, frameBuffer + 1, payloadLen);
  }
}

size_t WebSocketTelemetry::buildSystemStatus(uint8_t* buffer, size_t bufferSize) {
  if (bufferSize < 14) {
    return 0;
  }
  
  size_t offset = 0;
  
  // Get system state
  uint32_t uptime = SystemState::getUptimeMs();
  uint32_t freeHeap = SystemState::getFreeHeap();
  uint32_t largestBlock = SystemState::getLargestFreeBlock();
  
  offset = writeUint32(buffer, offset, uptime);
  offset = writeUint32(buffer, offset, freeHeap);
  offset = writeUint32(buffer, offset, largestBlock);
  
  return offset;
}

size_t WebSocketTelemetry::buildSensorsSnapshot(uint8_t* buffer, size_t bufferSize) {
  // Format: [count:uint8][sensor1:84][sensor2:84][sensor3:84]
  // Sensor entry (84 bytes): [name:64][type:1][mac:17][connected:1][battery:1]
  // Fixed-size entries, but we only emit `count` entries (smaller payload).
  //
  // NOTE: In production, `count` is typically stable (configured sensors don't change),
  // so payload sizes remain stable without forcing max-padding that increases peak memory.
  
  const size_t SENSOR_ENTRY_SIZE = 84;  // Fixed size per sensor
  const size_t MIN_BUFFER_SIZE = 1 + SENSOR_ENTRY_SIZE;  // count + at least 1 sensor
  
  if (bufferSize < MIN_BUFFER_SIZE) {
    return 0;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    // Return empty snapshot if BLE not available
    return writeUint8(buffer, 0, 0);
  }
  
  // Get sensors from BLERelayManager
  SensorInfo sensors[2];  // Max 2 sensors (HR, CSC)
  uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, 2);
  
  size_t offset = 0;
  offset = writeUint8(buffer, offset, count);
  
  // Write each sensor (fixed 84 bytes per sensor)
  for (uint8_t i = 0; i < count && i < 2; i++) {
    const SensorInfo& s = sensors[i];
    
    // Check buffer space
    if (offset + SENSOR_ENTRY_SIZE > bufferSize) {
      break;
    }
    
    // Write name (fixed 64 bytes, null-padded)
    size_t nameLen = strlen(s.name);
    if (nameLen > 64) nameLen = 64;  // Truncate if too long
    memcpy(buffer + offset, s.name, nameLen);
    // Zero-pad remaining bytes
    if (nameLen < 64) {
      memset(buffer + offset + nameLen, 0, 64 - nameLen);
    }
    offset += 64;
    
    // Write type (1 byte: 0=HR, 1=CSC)
    uint8_t typeByte = 0;
    if (strcmp(s.type, "HR") == 0) typeByte = 0;
    else if (strcmp(s.type, "CSC") == 0) typeByte = 1;
    buffer[offset++] = typeByte;
    
    // Write MAC (fixed 17 bytes, null-terminated, null-padded)
    size_t macLen = strlen(s.mac);
    if (macLen > 17) macLen = 17;  // Truncate if too long
    memcpy(buffer + offset, s.mac, macLen);
    // Zero-pad remaining bytes
    if (macLen < 17) {
      memset(buffer + offset + macLen, 0, 17 - macLen);
    }
    offset += 17;
    
    // Write connected status (1 byte)
    buffer[offset++] = s.connected ? 1 : 0;
    
    // Write battery (1 byte: 0-100, or 255 for unknown)
    uint8_t batteryByte = (s.battery >= 0 && s.battery <= 100) ? static_cast<uint8_t>(s.battery) : 255;
    buffer[offset++] = batteryByte;
  }
  
  return offset;
#else
  // BLE not enabled - return empty snapshot
  return writeUint8(buffer, 0, 0);
#endif
}

size_t WebSocketTelemetry::buildFansSnapshot(uint8_t* buffer, size_t bufferSize) {
  // Format: [count:uint8][fan1:35][fan2:35][fan3:35][fan4:35]
  // Fan entry (35 bytes): [token:32][connected:1][control_active:1][recovery:1]
  // recovery: 0=normal, 1=recovering (auto), 2=exhausted (needs user action)
  // Fixed-size entries, but we only emit `count` entries (smaller payload).
  
  const size_t FAN_ENTRY_SIZE = 35;  // Fixed size per fan
  const size_t MIN_BUFFER_SIZE = 1 + FAN_ENTRY_SIZE;  // count + at least 1 fan
  
  if (bufferSize < MIN_BUFFER_SIZE) {
    return 0;
  }
  
#if HW_ENABLE_FANS
  if (!g_fanController || !g_fanController->ready()) {
    // Fan controller not available - return empty snapshot
    return writeUint8(buffer, 0, 0);
  }
  
  // Get discovered fans from SmartMiFanAsync
  SmartMiFanDiscoveredDevice discoveredFans[4];
  size_t discoveredCount = 0;
  if (!g_fanController->getDiscoveredFans(static_cast<void*>(discoveredFans), discoveredCount)) {
    return writeUint8(buffer, 0, 0);
  }
  
  // Get configured fans from FanController
  FanConfig configuredFans[4];
  uint8_t configuredCount = g_fanController->getFanCount();
  for (uint8_t i = 0; i < configuredCount && i < 4; i++) {
    g_fanController->getFanConfig(i, configuredFans[i]);
  }
  
  size_t offset = 0;
  offset = writeUint8(buffer, offset, configuredCount);
  
  // Write each fan (fixed 35 bytes per fan)
  for (uint8_t i = 0; i < configuredCount && i < 4; i++) {
    const FanConfig& cfg = configuredFans[i];
    const char* token = cfg.token;
    
    // Check buffer space
    if (offset + FAN_ENTRY_SIZE > bufferSize) {
      break;
    }
    
    // Find matching discovered fan
    bool connected = false;
    bool controlActive = false;
    uint8_t recovery = 0;
    
    // Only check discovered list if fan has a valid IP in NVS config.
    // If IP is 0.0.0.0, this is a freshly added fan - don't use stale discovered entries
    // (e.g., from before the fan was deleted and re-added).
    bool checkDiscoveredList = (cfg.ip != IPAddress(0, 0, 0, 0));
    
    if (checkDiscoveredList) {
      for (size_t j = 0; j < discoveredCount; j++) {
        if (strcasecmp(discoveredFans[j].token, token) == 0) {
          // Get participation state first to determine connected/controlActive
          FanParticipationState state = SmartMiFanAsync_getFanParticipationState(j);
          
          if (state == FanParticipationState::ERROR) {
            // ERROR state: mark as disconnected so frontend shows ERROR
            connected = false;
            controlActive = false;
          } else {
            // ACTIVE or INACTIVE: use ready state as connected
            connected = discoveredFans[j].ready;
            controlActive = (state == FanParticipationState::ACTIVE);
          }
          break;
        }
      }
    }

    // Recovery UI flag is based on configured fan index (i)
    recovery = g_fanController->getFanRecoveryUiState(i);
    
    // NOTE: Fans with IP 0.0.0.0 are freshly added - always show ERROR until discovery completes.
    // This prevents stale discovered entries from showing wrong status after delete + re-add.
    
    // Write token (fixed 32 bytes, null-padded)
    size_t tokenLen = strlen(token);
    if (tokenLen > 32) tokenLen = 32;  // Truncate if too long
    memcpy(buffer + offset, token, tokenLen);
    // Zero-pad remaining bytes
    if (tokenLen < 32) {
      memset(buffer + offset + tokenLen, 0, 32 - tokenLen);
    }
    offset += 32;
    
    // Write connected status (1 byte)
    buffer[offset++] = connected ? 1 : 0;
    
    // Write control active status (1 byte)
    buffer[offset++] = controlActive ? 1 : 0;

    // Write recovery status (1 byte)
    buffer[offset++] = recovery;
  }
  
  return offset;
#else
  // Fans not enabled - return empty snapshot
  return writeUint8(buffer, 0, 0);
#endif
}

size_t WebSocketTelemetry::buildSensorConnState(uint8_t* buffer, size_t bufferSize) {
  // Format: [name_len:uint8][name:bytes][connected:uint8]
  // Note: This is a delta frame, so we need to track which sensor changed
  // For now, we'll send the first sensor's state (this could be improved to track last changed sensor)
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    return 0;
  }
  
  // Get first sensor (or we could track the last changed sensor)
  SensorInfo sensors[2];  // MAX_SENSORS (HR, CSC)
  uint8_t count = g_bleRelayManager->getConfiguredSensors(sensors, 2);
  
  if (count == 0) {
    return 0;  // No sensors to report
  }
  
  // Send first sensor's state (in a real implementation, we'd track which sensor changed)
  const SensorInfo& s = sensors[0];
  
  size_t nameLen = strlen(s.name);
  if (nameLen > 255) nameLen = 255;
  
  // Check buffer size: 1 (name_len) + name + 1 (connected)
  if (bufferSize < 1 + nameLen + 1) {
    return 0;
  }
  
  size_t offset = 0;
  offset = writeUint8(buffer, offset, static_cast<uint8_t>(nameLen));
  if (nameLen > 0) {
    memcpy(buffer + offset, s.name, nameLen);
    offset += nameLen;
  }
  offset = writeUint8(buffer, offset, s.connected ? 1 : 0);
  
  return offset;
#else
  return 0;
#endif
}

size_t WebSocketTelemetry::buildHeartRate(uint8_t* buffer, size_t bufferSize) {
  // Format: [value:uint16]
  
  if (bufferSize < 2) {
    return 0;
  }
  
#if HW_ENABLE_BLE
  // Get heart rate from BluetoothBikeSensorServer
  if (BluetoothBikeSensorServerHasHeartRate()) {
    uint16_t heartRate = BluetoothBikeSensorServerGetLastHeartRate();
    return writeUint16(buffer, 0, heartRate);
  }
#endif
  
  // No heart rate available
  return 0;
}

size_t WebSocketTelemetry::buildDiscoveryStatus(uint8_t* buffer, size_t bufferSize) {
  // Format: [active:uint8][type:uint8]
  // type: 0=HR, 1=CSC, 255=all/unknown (2=PWR reserved, not used in Homewind)
  
  if (bufferSize < 2) {
    return 0;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    return 0;
  }
  
  uint8_t active = g_bleRelayManager->isDiscoveryActive() ? 1 : 0;
  uint8_t type = 255;  // Unknown/all types
  
  // Could track discovery type if needed, but for now use 255 (all)
  
  size_t offset = 0;
  offset = writeUint8(buffer, offset, active);
  offset = writeUint8(buffer, offset, type);
  
  return offset;
#else
  return 0;
#endif
}

size_t WebSocketTelemetry::buildDiscoveryResults(uint8_t* buffer, size_t bufferSize) {
  // Format: [count:uint8][result1...resultN]
  // Result: [index:uint8][name_len:uint8][name:bytes]
  // Optimized: Only send index + name to UI, backend resolves full data from cache
  
  if (bufferSize < 1) {
    return 0;
  }
  
#if HW_ENABLE_BLE
  if (!g_bleRelayManager || !g_bleRelayManager->ready()) {
    return 0;
  }
  
  // Get discovery count directly from library (no need to fetch all data)
  uint8_t count = BikeSensorServerGetSensorCount();
  if (count > 50) count = 50;  // Limit to max 50 results
  
  size_t offset = 0;
  offset = writeUint8(buffer, offset, count);
  
  // Write each result: index + name only
  for (uint8_t i = 0; i < count; i++) {
    SensorType type;
    char name[64];
    uint8_t mac[6];  // Not used, but required by API
    
    if (!BikeSensorServerGetSensor(i, &type, name, sizeof(name), mac)) {
      // Skip invalid entries
      continue;
    }
    
    size_t nameLen = strlen(name);
    
    // Check if we have enough space (index + name + overhead)
    size_t estimatedSize = 1 + 1 + nameLen;  // index(1) + nameLen(1) + name
    if (offset + estimatedSize > bufferSize) {
      // Truncate - we've run out of space
      break;
    }
    
    // Write index (discovery cache index)
    offset = writeUint8(buffer, offset, i);
    
    // Write name (length-prefixed)
    if (nameLen > 255) nameLen = 255;
    offset = writeUint8(buffer, offset, static_cast<uint8_t>(nameLen));
    if (nameLen > 0) {
      memcpy(buffer + offset, name, nameLen);
      offset += nameLen;
    }
  }
  
  return offset;
#else
  // BLE not enabled - return empty results
  return writeUint8(buffer, 0, 0);
#endif
}

size_t WebSocketTelemetry::buildFirmwareVersion(uint8_t* buffer, size_t bufferSize) {
  // Format: [version_len:uint8][version:bytes]
  
  const char* version = BuildInfo::getVersion();
  size_t versionLen = strlen(version);
  
  // Limit version string length to 32 chars
  if (versionLen > 32) {
    versionLen = 32;
  }
  
  // Need at least 1 byte for length + version bytes
  if (bufferSize < 1 + versionLen) {
    return 0;
  }
  
  size_t offset = 0;
  offset = writeUint8(buffer, offset, static_cast<uint8_t>(versionLen));
  memcpy(buffer + offset, version, versionLen);
  offset += versionLen;
  
  return offset;
}

size_t WebSocketTelemetry::buildFirmwareProgress(uint8_t* buffer, size_t bufferSize) {
  // Format: [percent:uint8]
  
  if (bufferSize < 1) {
    return 0;
  }
  
#if HW_ENABLE_OTA
  extern FirmwareUpdateManager* g_firmwareUpdateManager;
  
  uint8_t percent = 0;
  if (g_firmwareUpdateManager && g_firmwareUpdateManager->isReady()) {
    percent = static_cast<uint8_t>(g_firmwareUpdateManager->getProgress());
  }
  
  return writeUint8(buffer, 0, percent);
#else
  return writeUint8(buffer, 0, 0);
#endif
}

size_t WebSocketTelemetry::buildFirmwareSearchResult(uint8_t* buffer, size_t bufferSize) {
  // Format: [has_update:uint8][version_len:uint8][version:bytes][notes_len:uint16][notes:bytes]
  // Note: version and notes only present if has_update=1
  
#if HW_ENABLE_OTA
  extern FirmwareUpdateManager* g_firmwareUpdateManager;
  
  if (!g_firmwareUpdateManager || !g_firmwareUpdateManager->isReady()) {
    return 0;
  }
  
  // OPTIMIZATION: getStateString() now returns const char* (no String allocation)
  const char* state = g_firmwareUpdateManager->getStateString();
  
  // Only send result if search is complete (ready_to_update or idle with no error)
  if (strcmp(state, "ready_to_update") != 0 && strcmp(state, "idle") != 0) {
    return 0;
  }
  
  // FIX: hasUpdate should be based on state, not just remoteVersion presence
  // "ready_to_update" means a NEWER version is available
  // "idle" after search means no newer version (even if remoteVersion is set to older version)
  bool hasUpdate = (strcmp(state, "ready_to_update") == 0);
  
  if (bufferSize < 1) {
    return 0;
  }
  
  size_t offset = 0;
  
  // Write has_update flag
  offset = writeUint8(buffer, offset, hasUpdate ? 1 : 0);
  
  if (hasUpdate) {
    // Enforce hard limits: version max 32 chars, notes max 160 chars
    const size_t MAX_VERSION_LEN = 32;
    const size_t MAX_NOTES_LEN = 160;
    
    // OPTIMIZATION: Direct const char* access - no heap allocation
    // FirmwareUpdateManager returns pointers to internal cached buffers
    const char* remoteVersion = g_firmwareUpdateManager->getRemoteVersion();
    const char* releaseNotes = g_firmwareUpdateManager->getReleaseNotes();
    
    size_t versionLen = strlen(remoteVersion);
    size_t notesLen = strlen(releaseNotes);
    
    // Truncate to limits
    if (versionLen > MAX_VERSION_LEN) {
      versionLen = MAX_VERSION_LEN;
    }
    if (notesLen > MAX_NOTES_LEN) {
      notesLen = MAX_NOTES_LEN;
    }
    
    // Calculate required size: 1 (has_update) + 1 (version_len) + version + 2 (notes_len) + notes
    size_t requiredSize = 1 + 1 + versionLen + 2 + notesLen;
    
    if (bufferSize < requiredSize) {
      HW_ERROR_PRINTF("[WebSocketTelemetry] FIRMWARE_SEARCH_RESULT frame too large (%zu bytes, max %zu)\n", 
                     requiredSize, bufferSize);
      return 0;
    }
    
    // Write version (length-prefixed string, max 32 chars)
    offset = writeUint8(buffer, offset, static_cast<uint8_t>(versionLen));
    if (versionLen > 0) {
      memcpy(buffer + offset, remoteVersion, versionLen);
      offset += versionLen;
    }
    
    // Write release notes (length-prefixed string, uint16 for length, max 160 chars)
    offset = writeUint16(buffer, offset, static_cast<uint16_t>(notesLen));
    if (notesLen > 0) {
      memcpy(buffer + offset, releaseNotes, notesLen);
      offset += notesLen;
    }
    
    // No String objects created - direct const char* access eliminates heap allocations
  }
  
  return offset;
#else
  return 0;
#endif
}

size_t WebSocketTelemetry::buildHeartRateSettings(uint8_t* buffer, size_t bufferSize) {
  // Format: [min:uint16][max:uint16]
  // Total: 4 bytes
  
  if (bufferSize < 4) {
    return 0;
  }
  
#if HW_ENABLE_NVS
  extern HeartRateSettings* g_heartRateSettings;
  
  if (!g_heartRateSettings) {
    return 0;
  }
  
  uint16_t minHR = g_heartRateSettings->getMin();
  uint16_t maxHR = g_heartRateSettings->getMax();
  
  size_t offset = 0;
  offset = writeUint16(buffer, offset, minHR);
  offset = writeUint16(buffer, offset, maxHR);
  
  return offset;
#else
  return 0;
#endif
}

void WebSocketTelemetry::sendFullSnapshot() {
  // Send all snapshot frames on client connect (reconnect safety)
  buildAndSendFrame(TelemetryFrameType::SYSTEM_STATUS);
  buildAndSendFrame(TelemetryFrameType::SENSORS_SNAPSHOT);
  buildAndSendFrame(TelemetryFrameType::FANS_SNAPSHOT);
}

// Helper functions

size_t WebSocketTelemetry::writeString(uint8_t* buffer, size_t offset, const char* str, size_t maxLen) {
  if (!str || !buffer) {
    return offset;
  }
  
  size_t strLen = strlen(str);
  if (strLen > maxLen) {
    strLen = maxLen;
  }
  
  if (strLen > 255) {
    strLen = 255; // Max length for uint8_t
  }
  
  buffer[offset++] = static_cast<uint8_t>(strLen);
  memcpy(buffer + offset, str, strLen);
  return offset + strLen;
}

size_t WebSocketTelemetry::writeUint8(uint8_t* buffer, size_t offset, uint8_t value) {
  if (buffer) {
    buffer[offset] = value;
  }
  return offset + 1;
}

size_t WebSocketTelemetry::writeUint16(uint8_t* buffer, size_t offset, uint16_t value) {
  if (buffer) {
    buffer[offset] = value & 0xFF;
    buffer[offset + 1] = (value >> 8) & 0xFF;
  }
  return offset + 2;
}

size_t WebSocketTelemetry::writeUint32(uint8_t* buffer, size_t offset, uint32_t value) {
  if (buffer) {
    buffer[offset] = value & 0xFF;
    buffer[offset + 1] = (value >> 8) & 0xFF;
    buffer[offset + 2] = (value >> 16) & 0xFF;
    buffer[offset + 3] = (value >> 24) & 0xFF;
  }
  return offset + 4;
}

#endif // HOMEWIND_ENABLE_WEBSOCKET

