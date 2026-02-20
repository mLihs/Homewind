# API Documentation

This document covers all HTTP API endpoints and the WebSocket binary telemetry protocol.

---

## HTTP API Overview

All HTTP APIs use **form-urlencoded** requests (query parameters or body) and return **text/plain** responses (no JSON). This simplifies parsing and reduces memory usage.

**Base URL**: `http://<device-ip>/` (default port 80)

**Response Format**:
- Success: `"OK"` (HTTP 200)
- Error: `"ERR:<message>"` (HTTP 4xx/5xx)

**HTTP Status Codes**:
- `200 OK`: Success
- `400 Bad Request`: Invalid parameters
- `409 Conflict`: Operation conflict (e.g., already connected)
- `500 Server Error`: Internal error
- `503 Service Unavailable`: System not ready or maintenance mode

**Readiness**: All endpoints check `SystemState::isReady()` and return `503` if system not ready.

---

## Static Assets

**File**: `src/web/WebServer.cpp`

All static assets are served automatically from PROGMEM.

| Route | Asset | Description |
|-------|-------|-------------|
| `GET /` | `index.html` | Main web UI |
| `GET /index.html` | `index.html` | Main web UI |
| `GET /app.css` | `app.css` | Stylesheet |
| `GET /app.js` | `app.js` | JavaScript application |

**Features**:
- Automatic GZIP compression
- ETag support (304 Not Modified)
- Cache-Control headers
- Cache-busting via query parameter (`?v=HASH`)

---

## Action API (`/api/v1/action/*`)

**File**: `src/web/ApiActions.cpp`

All action endpoints use **POST** method with form-urlencoded parameters.

### Sensor Actions

#### Connect Sensor
```
POST /api/v1/action/sensor/connect?name=<sensor-name>
```
Connect to a configured sensor by name. The sensor must be configured first using `/api/v1/action/sensor/set`.

**Parameters**:
- `name` (required): Sensor name/identifier

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `400 Bad Request`: Missing or invalid name
- `404 Not Found`: Sensor not configured
- `409 Conflict`: Sensor already connected
- `503 Service Unavailable`: System not ready

---

#### Disconnect Sensor
```
POST /api/v1/action/sensor/disconnect?name=<sensor-name>
```
Disconnect from a sensor. The sensor configuration is preserved and can be reconnected later.

**Parameters**:
- `name` (required): Sensor name/identifier

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `400 Bad Request`: Missing or invalid name
- `404 Not Found`: Sensor not configured
- `503 Service Unavailable`: System not ready

---

#### Delete Sensor
```
POST /api/v1/action/sensor/delete?name=<sensor-name>
```
Remove a sensor from configuration and disconnect it.

**Parameters**:
- `name` (required): Sensor name/identifier

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `400 Bad Request`: Missing or invalid name
- `404 Not Found`: Sensor not found
- `503 Service Unavailable`: System not ready

---

#### Set/Add Sensor
```
POST /api/v1/action/sensor/set?name=<sensor-name>&type=<sensor-type>&mac=<mac-address>&addrType=<addr-type>
POST /api/v1/action/sensor/set?index=<discovery-cache-index>
```
Add or update a sensor configuration. The sensor will be automatically connected when found.

**Parameters (manual)**:
- `name` (required if no index): Sensor name/identifier
- `type` (required if no index): Sensor type (`HR` or `CSC`)
- `mac` (optional): MAC address for faster reconnection (format: `XX:XX:XX:XX:XX:XX`)
- `addrType` (optional): BLE address type (`0` = Public, `1` = Random, `-1` = Unknown/auto-detect)

**Parameters (from discovery list)**:
- `index` (optional): Discovery cache index from `DISCOVERY_RESULTS`; backend resolves name, type, MAC via `BikeSensorServerGetSensor(index, ...)`. Use `index=-1` to cancel.

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `400 Bad Request`: Missing or invalid parameters
- `503 Service Unavailable`: System not ready

**Example**:
```
POST /api/v1/action/sensor/set?name=TICKR%20FIT&type=HR&mac=00:1A:2B:3C:4D:5E&addrType=0
```

---

#### Reload Sensors
```
POST /api/v1/action/sensor/reload
```
Reload all sensors from NVS configuration and reconnect them.

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `503 Service Unavailable`: System not ready

---

#### Pause BLE Server
```
POST /api/v1/action/sensor/server/pause
```
Pause BLE server (stop advertising, disconnect clients).

**Response**: `"OK"` or `"ERR:<message>"`

---

#### Resume BLE Server
```
POST /api/v1/action/sensor/server/resume
```
Resume BLE server (start advertising, accept clients).

**Response**: `"OK"` or `"ERR:<message>"`

---

### Discovery Actions

#### Start Discovery
```
POST /api/v1/action/discovery/start?type=<sensor-type>
```
Start BLE sensor discovery.

**Parameters**:
- `type` (required): Sensor type (`HR` or `CSC`)

**Response**: `"OK"` or `"ERR:<message>"`

---

#### Stop Discovery
```
POST /api/v1/action/discovery/stop
```
Stop BLE sensor discovery.

**Response**: `"OK"` or `"ERR:<message>"`

---

### Fan Actions

#### Add Fan
```
POST /api/v1/action/fan/add?token=<fan-token>
```
Add a fan to configuration. The fan will be automatically discovered and connected.

**Parameters**:
- `token` (required): 32-character hexadecimal fan token

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Fan added successfully
- `400 Bad Request`: Invalid token format (must be 32 hex characters)
- `409 Conflict`: Maximum number of fans reached (4)
- `503 Service Unavailable`: System not ready

**Initial State**: Newly added fans start in **ERROR** state until discovery and handshake complete. The WebSocket `FANS_SNAPSHOT` will show `connected=0` until the fan is successfully connected.

---

#### Control Fan
```
POST /api/v1/action/fan/control?id=<fan-id>&active=<true|false>
```
Enable/disable fan control (participation in HR-driven speed control).

**Parameters**:
- `id` (required): Fan identifier (token)
- `active` (required): Control state (`true` or `false`)

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Fan control state changed successfully
- `400 Bad Request`: Missing or invalid parameters
- `404 Not Found`: Fan not configured in NVS
- `500 Server Error`: Failed to set fan control state (fan not yet discovered/connected)
- `503 Service Unavailable`: System not ready

**Throttling**: Rapid toggle requests (< 500ms apart) are rejected to prevent fan damage.

**Note**: If a fan is configured but not yet discovered/connected, the control state is saved to NVS and will be applied when the fan connects.

---

#### Update Fan Token
```
POST /api/v1/action/fan/update?old_token=<old-token>&new_token=<new-token>
```
Update a fan's token (e.g., after token regeneration).

**Parameters**:
- `old_token` (required): Current 32-character hexadecimal fan token
- `new_token` (required): New 32-character hexadecimal fan token

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Token updated successfully
- `400 Bad Request`: Invalid token format
- `404 Not Found`: Fan with old token not found
- `503 Service Unavailable`: System not ready

---

#### Start Fan Discovery
```
POST /api/v1/action/fan/discovery/start
```
Start fan discovery.

**Response**: `"OK"` or `"ERR:<message>"`

---

#### Stop Fan Discovery
```
POST /api/v1/action/fan/discovery/stop
```
Stop fan discovery.

**Response**: `"OK"` or `"ERR:<message>"`

---

#### Remove Fan
```
POST /api/v1/action/fan/remove?token=<fan-token>
```
Remove a fan from configuration.

**Parameters**:
- `token` (required): Fan token

**Response**: `"OK"` or `"ERR:<message>"`

---

### Firmware Actions

#### Search for Firmware
```
POST /api/v1/action/firmware/search
```
Check GitHub for latest firmware release.

**Response**: `"OK"` or `"ERR:<message>"`

**Note**: Results available via WebSocket `FIRMWARE_VERSION` frame or status endpoint.

---

#### Download Firmware
```
POST /api/v1/action/firmware/download
```
Download firmware from GitHub (async).

**Response**: `"OK"` (download started) or `"ERR:<message>"`

**Progress**: Reported via WebSocket `FIRMWARE_PROGRESS` frame.

---

#### Install Firmware
```
POST /api/v1/action/firmware/install
```
Install downloaded firmware and reboot.

**Response**: `"OK"` (installation started) or `"ERR:<message>"`

**Note**: Enters maintenance mode and reboots after installation.

---

#### Abort Firmware Update
```
POST /api/v1/action/firmware/abort
```
Abort current firmware update operation.

**Response**: `"OK"` or `"ERR:<message>"`

---

#### Firmware Status
```
GET /api/v1/action/firmware/status
```
Get current firmware update state.

**Response**: `text/plain` with key=value pairs:
```
state=<state>
progress=<0-100>
error=<error-message>          (if error occurred)
remote_version=<version>       (if available, max 32 chars)
truncated=1                    (if response was truncated)
```

**States**: `idle`, `searching`, `ready_to_update`, `downloading`, `installing`, `error`, `unavailable`, `disabled`

**Note**: 
- Release notes are delivered via WebSocket `FIRMWARE_SEARCH_RESULT` frame (not in HTTP response)
- Response uses fixed-size buffer (512 bytes) for deterministic memory usage
- Version strings are truncated to max 32 characters

---

### Telemetry Actions

#### Refresh Telemetry Snapshots
```
POST /api/v1/action/telemetry/refresh
POST /api/v1/action/telemetry/refresh?client_id=<ws-client-id>
```

Triggers an immediate refresh of key WebUI snapshots via WebSocket.

**Parameters**:
- `client_id` (optional): WebSocket client id (sent by server via WS control frame `CLIENT_ID (0x0C)`).
  - If provided, heavy snapshots (`SENSORS_SNAPSHOT`, `FANS_SNAPSHOT`) are sent **unicast** to that client.
  - This avoids broadcast bursts under WS=2 and reduces cold-load heap spikes.

**Response**: `"OK"` or `"ERR:<message>"`

**Notes**:
- Small frames (SYSTEM_STATUS, FIRMWARE_VERSION, HEART_RATE_SETTINGS) are always allowed.
- Without `client_id`, heavy snapshots are only broadcast when <=1 WS client is connected.

---

### System Actions

#### System Info
```
GET /api/v1/system/info
```
Get system information.

**Response**: `text/plain` with key=value pairs:
```
name=<firmware-name>
version=<version>              (max 32 chars)
build_id=<build-id>            (max 8 chars)
uptime_ms=<milliseconds>
free_heap=<bytes>
largest_free_block=<bytes>
ready=<0|1>
truncated=1                    (if response was truncated)
```

**Note**: Response uses fixed-size buffer (512 bytes) for deterministic memory usage

---

#### Device Restart
```
POST /api/v1/action/system/restart
```
Reboot the device. No parameters. Settings are preserved.

**Response**: `"OK"` (restart initiated) or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `503 Service Unavailable`: System not ready

**Note**: Response is sent before reboot. The device restarts shortly after.

---

#### WiFi Reset
```
POST /api/v1/action/system/wifi_reset
```
Clear WiFi credentials (NVS) and reboot. Other settings (sensors, fans, etc.) are preserved. Available only when using the WML WiFi backend (Production mode).

**Implementation**: Uses `WiFiManagerLite::factoryReset(clearCallback, true)`; WML clears credentials via callback and schedules the restart.

**Response**: `application/json` `{"ok":true,"rebooting":true}` (reset initiated) or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Success
- `503 Service Unavailable`: System not ready, or WiFi reset not available (e.g. fixed WiFi backend)

**Note**: Device restarts shortly after. On boot, WiFi setup portal (AP) is shown until new credentials are configured.

---

#### Factory Reset
```
POST /api/v1/action/system/factory_reset
```
Clear all settings and reboot.

**Response**: `application/json` `{"ok":true,"rebooting":true}` (reset initiated) or `"ERR:<message>"`

**Note**: Enters maintenance mode, clears settings, and reboots.

---

### Maintenance Mode (Debug)

#### Begin Maintenance Mode
```
POST /api/v1/action/system/maintenance/begin?reason=<ota|factory_reset>
```
Begin maintenance mode (debug endpoint).

**Parameters**:
- `reason` (optional): Maintenance reason (`ota`, `factory_reset`)

**Response**: `"OK"` or `"ERR:<message>"`

---

#### End Maintenance Mode
```
POST /api/v1/action/system/maintenance/end
```
End maintenance mode (debug endpoint).

**Response**: `"OK"` or `"ERR:<message>"`

---

## Settings API (`/api/v1/settings/*`)

**File**: `src/web/ApiSettings.cpp`

Settings API for persistent configuration.

### Get Setting
```
GET /api/v1/settings/get?key=<setting-name>
```
Get a setting value.

**Parameters**:
- `key` (required): Setting name

**Response**: Setting value or empty string if not found

---

### Set Setting
```
POST /api/v1/settings/set?key=<setting-name>&value=<value>
```
Set a setting value.

**Parameters**:
- `key` (required): Setting name
- `value` (required): Setting value

**Response**: `"OK"` or `"ERR:<message>"`

---

### List Settings
```
GET /api/v1/settings/list
```
List all settings.

**Response**: Comma-separated list of setting keys

---

## WebSocket Protocol

**File**: `src/web/WebSocketTelemetry.h`, `src/web/WebSocketTelemetry.cpp`

**Endpoint**: `ws://<device-ip>/ws`

**Protocol**: Binary (not text)

**Direction**: Server → Client only (observation)

**Full Documentation**: See `src/web/WEBSOCKET_PROTOCOL.md`

---

### Frame Format

All frames follow: `[type:uint8][payload:variable]`

- **type**: Frame type identifier (1 byte)
- **payload**: Frame-specific binary data (variable length)

---

### Frame Types

#### Snapshot Frames (Full State)

Sent on client connect and periodically (every 30 seconds).

| Type | ID | Description | Payload |
|------|-----|-------------|---------|
| `SYSTEM_STATUS` | `0x01` | System health | `[uptime_ms:uint32][free_heap:uint32][largest_free_block:uint32][wifi_rssi:int8][wifi_connected:uint8]` |
| `SENSORS_SNAPSHOT` | `0x02` | All configured sensors | `[count:uint8][sensor1...sensorN]` |
| `FANS_SNAPSHOT` | `0x07` | All configured fans | `[count:uint8][fan1...fanN]` |

#### Delta Frames (Incremental Updates)

Sent only when state changes (marked dirty).

| Type | ID | Description | Payload |
|------|-----|-------------|---------|
| `SENSOR_CONN_STATE` | `0x03` | Sensor connection state change | `[name_len:uint8][name:bytes][connected:uint8]` |
| `DISCOVERY_STATUS` | `0x04` | Discovery state change | `[active:uint8][type:uint8]` |
| `DISCOVERY_RESULTS` | `0x05` | New discovery results | `[count:uint8][result1...resultN]` |
| `HEART_RATE` | `0x06` | Heart rate value update | `[value:uint16]` |
| `FIRMWARE_VERSION` | `0x08` | Firmware version update | `[version_len:uint8][version:bytes]` |
| `FIRMWARE_PROGRESS` | `0x09` | Firmware update progress | `[percent:uint8]` |
| `FIRMWARE_SEARCH_RESULT` | `0x0A` | Firmware search result | `[has_update:uint8][version_len:uint8][version:bytes][notes_len:uint16][notes:bytes]` |

---

### Frame Details

#### SYSTEM_STATUS (0x01)

**Payload** (14 bytes):
```
[uptime_ms:uint32]        // System uptime in milliseconds (little-endian)
[free_heap:uint32]        // Free heap in bytes (little-endian)
[largest_free_block:uint32] // Largest free block in bytes (little-endian)
[wifi_rssi:int8]          // WiFi signal strength in dBm (signed)
[wifi_connected:uint8]    // WiFi connection status (0=disconnected, 1=connected)
```

**Example**:
```javascript
const frame = new Uint8Array([0x01, ...]);  // First byte is type
const view = new DataView(frame.buffer, 1);
const uptime = view.getUint32(0, true);     // little-endian
const freeHeap = view.getUint32(4, true);
const largestBlock = view.getUint32(8, true);
const rssi = view.getInt8(12);
const connected = frame[13];
```

---

#### SENSORS_SNAPSHOT (0x02) ✅ **Implemented**

**Status**: Fully implemented via `BLERelayManager::getConfiguredSensors()`

**Payload**:
```
[count:uint8]             // Number of sensors
[sensor1...sensorN]       // Sensor entries
```

**Sensor Entry**:
```
[name_len:uint8]          // Sensor name length
[name:bytes]              // Sensor name (UTF-8, not null-terminated)
[type:uint8]              // Sensor type (0=HR, 1=CSC)
[mac_len:uint8]           // MAC address length
[mac:bytes]               // MAC address string (not null-terminated)
[connected:uint8]         // Connection status (0=disconnected, 1=connected)
[battery:uint8]           // Battery level (0-100, 255=unknown)
```

**Sent**: On WebSocket connect and when sensor configuration changes.

---

#### FANS_SNAPSHOT (0x07)

**Payload**:
```
[count:uint8]             // Number of fans
[fan1...fanN]             // Fan entries
```

**Fan Entry**:
```
[token_len:uint8]         // Fan token length
[token:bytes]             // Fan token string (hex, not null-terminated)
[connected:uint8]         // Connection status (0=disconnected, 1=connected)
[control_active:uint8]    // Control active status (0=inactive, 1=active)
```

**Fan State Interpretation**:

| connected | control_active | UI State | Description |
|-----------|----------------|----------|-------------|
| `0` | `0` | **ERROR** | Fan not discovered or handshake failed |
| `1` | `0` | **INACTIVE** | Fan connected but not participating in HR control |
| `1` | `1` | **ACTIVE** | Fan connected and participating in HR control |

**State Transitions**:
1. **Add Fan**: `connected=0` → ERROR (until discovery + handshake)
2. **Restart**: `connected=0` → ERROR (until Smart Connect + handshake)
3. **Handshake Success**: `connected=1`, `control_active=1` → ACTIVE
4. **User Disables**: `connected=1`, `control_active=0` → INACTIVE
5. **Connection Lost**: `connected=0` → ERROR

**Note**: Freshly added fans (IP = 0.0.0.0) always show ERROR state until discovery completes. This prevents stale state from deleted fans affecting re-added fans.

---

#### SENSOR_CONN_STATE (0x03) ✅ **Implemented**

**Status**: Fully implemented via `BLERelayManager` connection callbacks

**Payload**:
```
[name_len:uint8]          // Sensor name length
[name:bytes]              // Sensor name (UTF-8, not null-terminated)
[connected:uint8]         // Connection status (0=disconnected, 1=connected)
```

**Sent**: When a sensor connects or disconnects.

---

#### DISCOVERY_STATUS (0x04) ✅ **Implemented**

**Status**: Fully implemented via `BLERelayManager::startDiscovery()` / `stopDiscovery()`

**Payload** (2 bytes):
```
[active:uint8]            // Discovery active (0=inactive, 1=active)
[type:uint8]             // Sensor type being discovered (0=HR, 1=CSC, 0xFF=all)
```

**Sent**: When discovery starts or stops.

---

#### DISCOVERY_RESULTS (0x05) ✅ **Implemented**

**Status**: Fully implemented via `BLERelayManager::getDiscoveryResults()`

**Payload**:
```
[count:uint8]            // Number of discovered sensors
[result1...resultN]     // Discovery result entries
```

**Discovery Result Entry**:
```
[name_len:uint8]        // Sensor name length
[name:bytes]            // Sensor name (UTF-8, not null-terminated)
[type:uint8]            // Sensor type (0=HR, 1=CSC)
[mac_len:uint8]         // MAC address length
[mac:bytes]             // MAC address string (not null-terminated)
```

**Sent**: When discovery completes and results are available.

---

#### HEART_RATE (0x06) ✅ **Implemented**

**Status**: Fully implemented via `BluetoothBikeSensorServerSetHeartRateCallback()`

**Payload** (2 bytes):
```
[value:uint16]            // Heart rate in BPM (little-endian)
```

**Sent**: When heart rate value changes (from connected HR sensor).

---

#### FIRMWARE_PROGRESS (0x09)

**Payload** (1 byte):
```
[percent:uint8]           // Update progress (0-100)
```

---

#### FIRMWARE_SEARCH_RESULT (0x0A)

**Payload**:
```
[has_update:uint8]        // Whether update is available (0=false, 1=true)
[version_len:uint8]       // Version string length (0-32, only if has_update=1)
[version:bytes]           // Version string (UTF-8, max 32 chars, only if has_update=1)
[notes_len:uint16]        // Release notes length (little-endian, 0-160, only if has_update=1)
[notes:bytes]             // Release notes text (UTF-8, max 160 chars, only if has_update=1)
```

**Limits**:
- Version: Maximum 32 characters
- Release notes: Maximum 160 characters

**Note**: If `has_update=0`, only the first byte is sent. Version and notes are only present when `has_update=1`.

---

### Architecture: Dirty-Flag + Loop-Flush

1. **Mark Dirty**: Call `markDirty(frameType)` from async contexts (BLE callbacks, etc.)
2. **Loop Flush**: Call `loopFlush()` from main `loop()` to send all dirty frames
3. **No Async Sending**: Never send frames directly from async callbacks

**Example**:
```cpp
// From async context (BLE callback)
telemetry->markDirty(TelemetryFrameType::HEART_RATE);

// From main loop
telemetry->loopFlush();  // Sends all dirty frames
```

---

### Reconnect Safety

- On client connect, all snapshot frames are automatically marked dirty
- `loopFlush()` sends full snapshot on next loop iteration
- Ensures clients always receive complete state after reconnection

---

### Endianness

All multi-byte integers are **little-endian** (LSB first).

---

### String Encoding

All strings are **UTF-8** encoded, **not null-terminated**. Length prefix indicates exact byte count.

---

## Example Requests

### Set/Add Sensor
```bash
curl -X POST "http://192.168.1.100/api/v1/action/sensor/set?name=TICKR%20FIT&type=HR&mac=00:1A:2B:3C:4D:5E&addrType=0"
```

### Connect Sensor
```bash
curl -X POST "http://192.168.1.100/api/v1/action/sensor/connect?name=TICKR%20FIT"
```

### Start Discovery
```bash
curl -X POST "http://192.168.1.100/api/v1/action/discovery/start?type=HR"
```

### Delete Sensor
```bash
curl -X POST "http://192.168.1.100/api/v1/action/sensor/delete?name=TICKR%20FIT"
```

### Device Restart
```bash
curl -X POST "http://192.168.1.100/api/v1/action/system/restart"
```

### WiFi Reset
```bash
curl -X POST "http://192.168.1.100/api/v1/action/system/wifi_reset"
```

### Set Setting
```bash
curl -X POST "http://192.168.1.100/api/v1/settings/set?key=hr_min&value=60"
```

### Get Setting
```bash
curl "http://192.168.1.100/api/v1/settings/get?key=hr_min"
```

### WebSocket Connection
```javascript
const ws = new WebSocket('ws://192.168.1.100/ws');
ws.binaryType = 'arraybuffer';

ws.onmessage = (event) => {
  const data = new Uint8Array(event.data);
  const frameType = data[0];
  const payload = data.slice(1);
  
  switch (frameType) {
    case 0x01:  // SYSTEM_STATUS
      const view = new DataView(payload.buffer);
      const uptime = view.getUint32(0, true);
      const freeHeap = view.getUint32(4, true);
      // ... process system status
      break;
    case 0x06:  // HEART_RATE
      const hr = new DataView(payload.buffer).getUint16(0, true);
      updateHeartRate(hr);
      break;
    // ... handle other frame types
  }
};
```

---

## Related Documentation

- `src/web/ENDPOINT.md` - Endpoint reference (legacy)
- `src/web/WEBSOCKET_PROTOCOL.md` - Detailed WebSocket protocol specification
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation

