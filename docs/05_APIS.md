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
POST /api/v1/action/sensor/set?generation=<discovery-generation>&index=<discovery-index>[&force=1]
```
Save a sensor from the discovery list. The request is queued; the main loop resolves name, type, MAC and address type from the discovery snapshot, saves NVS and adds the sensor to the BLE library.

**Parameters**:
- `generation` (since 1.6.0): `generation` from the `DISCOVERY_RESULTS` frame the index came from. If it is missing (pre-1.6.0 WebUI), the current discovery list is used.
- `index` (required): Index from `DISCOVERY_RESULTS`. Use `index=-1` to cancel.
- `force` / `forceApply` (optional): `1` = disconnect a connected sensor of the same type now; otherwise the change applies after it disconnects.

A sensor without a name is stored with its MAC as name. If the other sensor type already uses the MAC, the sensor is saved but not active; `SENSORS_SNAPSHOT` reports it as duplicate.

**Response**: `"OK"` or `"ERR:<message>"`

**Status Codes**:
- `200 OK`: Queued
- `400 Bad Request`: Missing or invalid parameters
- `409 Conflict`: Generation or index not in the current discovery snapshot, or maintenance active
- `503 Service Unavailable`: System not ready or command queue full

**Example**:
```
POST /api/v1/action/sensor/set?generation=3&index=0
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
- `type` (optional): Sensor type (`HR`, `CSC` or `PWR`); empty = all

**Response**: `"OK"` (request queued) or `"ERR:<message>"`

**Note (1.6.0)**: The start runs on the main loop. If a discovery is still open, the start is refused and `DISCOVERY_STATUS` reports `error=1` (busy); the HTTP status stays 200. Results arrive in `DISCOVERY_RESULTS` with a new `generation`.

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
Download firmware from GitHub (async). Only accepted in `ready_to_update`.

Admission enters maintenance (reason `ota`): the main loop stops fans, the HR adapter and BLE (discovery, scans, sensors) before the worker starts the download (`state=quiescing`). If that does not finish within 15 s the download does not start and the state becomes `restart_required`.

**Response**: `202` `{"ok":true,"job":"ota","state":"quiescing"}`

**Status Codes**:
- `202 Accepted`: Download admitted (also when it was already admitted)
- `409 Conflict`: `{"ok":false,"error":"not_ready_to_update"}` or `{"ok":false,"error":"maintenance_active"}` (other maintenance running)
- `500`: `{"ok":false,"error":"restart_required"}` – admitted, but the download job could not be queued; maintenance has already started, restart the device
- `503 Service Unavailable`: System not ready or OTA not available

**Progress**: Reported via WebSocket `FIRMWARE_PROGRESS` frame and the status endpoint.

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

**Note (1.6.0)**: After a download was admitted, abort never returns to `idle`: during `quiescing` the download is cancelled, during `downloading` the transfer is stopped; both end in `restart_required`. Repeated aborts return `OK`.

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

**States**: `idle`, `searching`, `ready_to_update`, `quiescing`, `downloading`, `installing`, `aborted`, `error`, `restart_required`, `unavailable`, `disabled`

- `quiescing` (1.6.0): download admitted, sensors and fans are being stopped.
- `restart_required` (1.6.0): maintenance ended without a download (abort, timeout or error). Sensors and fans stay stopped; restart the device (`system/restart`).

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
  - If provided, heavy snapshots (`SENSORS_SNAPSHOT`, `FANS_SNAPSHOT`) and `BIKE_METRICS` are sent **unicast** to that client. The handler only records the request; the main loop sends the frames.
  - This avoids broadcast bursts under WS=2 and reduces cold-load heap spikes.

**Response**: `"OK"` or `"ERR:<message>"`

**Notes**:
- Small frames (SYSTEM_STATUS, FIRMWARE_VERSION, HEART_RATE_SETTINGS) are always allowed; without `client_id`, BIKE_METRICS is broadcast too.
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
Reboot the device. No parameters. Settings are preserved. Also allowed during maintenance (it is the way out of maintenance).

**Response**: `202` `{"ok":true,"job":"restart"}`

**Note**: The main loop restarts the device about 250 ms after the response.

---

#### WiFi Reset
```
POST /api/v1/action/system/wifi_reset
```
Clear WiFi credentials (NVS) and reboot. Other settings (sensors, fans, etc.) are preserved. Available only when using the WML WiFi backend (Production mode).

**Implementation**: The handler queues the reset; the main loop calls `WiFiManagerLite::factoryReset(clearCallback, true)`, which clears the credentials and schedules the restart.

**Response**: `202` `{"ok":true,"job":"wifi_reset","rebooting":true}`

**Status Codes**:
- `202 Accepted`: Reset queued
- `409 Conflict`: Maintenance active
- `503 Service Unavailable`: System not ready, or WiFi reset not available (e.g. fixed WiFi backend)

**Note**: Device restarts shortly after. On boot, WiFi setup portal (AP) is shown until new credentials are configured.

---

#### Factory Reset
```
POST /api/v1/action/system/factory_reset
```
Clear all settings and reboot.

Enters maintenance (reason `factory_reset`). After the main loop has stopped fans and BLE, it clears the Preferences namespace `homewind` (sensors, fans, HR settings) and, with the WML backend, the WiFi credentials (`hwwifi`), then reboots. If stopping fails, nothing is erased and the status shows `restart_required`.

**Response**: `202` `{"ok":true,"job":"factory_reset"}`; with the WML backend `{"ok":true,"job":"factory_reset","ap_mode_after_reboot":true}` (the WiFi credentials are erased, the device comes back as access point).

**Status Codes**:
- `202 Accepted`: Admitted (also when already admitted)
- `409 Conflict`: Other maintenance running (e.g. OTA download)

**Result**: Only known after the reboot: `maintenance/status` reports `factory_reset.attempted` and `factory_reset.failed_mask` (bit 0 settings, bit 1 WiFi).

---

### Maintenance Mode (Debug)

Maintenance is one-way since 1.6.0: once admitted, BLE and fans stay stopped until reboot.

#### Begin Maintenance Mode
```
POST /api/v1/action/system/maintenance/begin
```
Begin manual maintenance (debug endpoint, reason `manual`). The `reason` parameter is ignored; OTA and factory reset have their own endpoints.

**Response**: `202` `{"ok":true,"job":"maintenance"}`; `409` `{"ok":false,"error":"maintenance_active"}` if another reason is active.

---

#### End Maintenance Mode
```
POST /api/v1/action/system/maintenance/end
```
No-op since 1.6.0: maintenance ends with a reboot (`system/restart`).

**Response**: `409` `{"ok":false,"error":"restart_required"}`

---

#### Maintenance Status
```
GET /api/v1/action/system/maintenance/status
```
**Response** (`application/json`):
```
{"state":"idle|requested|stopping_discovery|stopping_scans|removing|acked|failed",
 "reason":"none|ota|factory_reset|manual","result":"pending|ack|failed|aborted",
 "restart_required":false,
 "factory_reset":{"attempted":1,"failed_mask":0}}
```
`factory_reset` is present only after a reboot that followed a factory reset.

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
| `HEART_RATE_SETTINGS` | `0x0B` | Heart rate min/max settings | `[min:uint16][max:uint16]` |
| `BIKE_METRICS` | `0x0D` | Power and cadence | `[power_w:int16][cadence_rpm:uint16][flags:uint8]` |

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
[type:uint8]              // Sensor type (0=HR, 1=CSC, 2=PWR)
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
[type:uint8]             // Sensor type being discovered (0=HR, 1=CSC, 2=PWR, 0xFF=all)
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
[type:uint8]            // Sensor type (0=HR, 1=CSC, 2=PWR)
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

#### BIKE_METRICS (0x0D) ✅ **Implemented** (1.7.0)

**Status**: Latched by `BLERelayManager::loop()` from BikeSensor `lastPower()`, `lastCadence()` and `info(POWER).providesCadence`; encoded by `hw::encodeBikeMetrics()` (`src/core/BikeMetrics.h`)

**Payload** (5 bytes):
```
[power_w:int16]           // Power in watts (signed, little-endian), 0 if invalid or older than 3 s
[cadence_rpm:uint16]      // Cadence in rpm (little-endian), 0 if invalid or older than 3 s
[flags:uint8]             // bit0 power valid, bit1 cadence valid, bit2 cadence from power meter
```

**Sent**: When the encoded value changes (power/cadence sample or validity change, checked at least once per second), and once per client after connect. Cadence source: the CSC sensor while it is connected, otherwise the power meter. A value older than 3 s (power: last packet; cadence: last crank event or CSC packet) is sent as 0 with its valid bit set. Details: `src/web/WEBSOCKET_PROTOCOL.md`.

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

