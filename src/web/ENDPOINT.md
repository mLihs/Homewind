# Homewind WebServer Endpoints

Documentation of all HTTP routes and WebSocket endpoints of the Homewind WebServer.

## Overview

- **Static Assets**: Automatically registered in `WebServer.cpp`
- **API Actions**: Registered in `ApiActions.cpp` (when `HOMEWIND_ENABLE_API_ACTIONS == 1`)
- **API Settings**: Registered in `ApiSettings.cpp` (when `HOMEWIND_ENABLE_API_SETTINGS == 1`)
- **WebSocket**: Registered in `WebSocketTelemetry.cpp` (when `HOMEWIND_ENABLE_WEBSOCKET == 1`)

## Static Assets

**File**: `WebServer.cpp` → `registerAssetRoutes()`

All static assets are automatically loaded from the manifest and registered.

| Method | Route | Description |
|--------|-------|-------------|
| GET | `/` | Serves `index.html` |
| GET | `/index.html` | Serves `index.html` |
| GET | `/app.css` | Serves `app.css` (gzipped) |
| GET | `/app.js` | Serves `app.js` (gzipped) |
| GET | `/*` | Fallback: Checks manifest for asset names |

**Features**:
- Automatic GZIP compression
- ETag support (304 Not Modified)
- Cache-Control headers (configurable per asset)
- PROGMEM-based (no RAM usage)

## API v1 - Actions

**File**: `ApiActions.cpp` → `registerRoutes()`

All action endpoints use **POST** with query parameters or form-urlencoded data. No JSON.

Action POSTs (and `POST /api/v1/settings/set`) require header `X-Homewind: 1` or the device answers 403.

### Sensor Actions

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/api/v1/action/sensor/connect` | Connect sensor |
| POST | `/api/v1/action/sensor/disconnect` | Disconnect sensor |
| POST | `/api/v1/action/sensor/delete` | Delete sensor |
| POST | `/api/v1/action/sensor/reload` | Reload sensor |
| POST | `/api/v1/action/sensor/server/pause` | Pause BLE server |
| POST | `/api/v1/action/sensor/server/resume` | Resume BLE server |

### Discovery Actions

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/api/v1/action/discovery/start` | Start sensor discovery |
| POST | `/api/v1/action/discovery/stop` | Stop sensor discovery |

### Trainer Actions (`HW_ENABLE_TRAINER_MIRROR`, on in release builds since 2.0)

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/api/v1/action/trainer/setup` | Read the trainer into the mirror table: `generation` + `index` of `DISCOVERY_RESULTS` (WebUI), or `mac` + optional `name`. Runs in the background (status `setup`); active after a restart |
| POST | `/api/v1/action/trainer/enable` | `on=true\|false`: mirror the trainer after the next restart, or release it; 409 while a setup runs |
| POST | `/api/v1/action/trainer/forget` | Erase the stored trainer and its table; mirror off after the next restart; 409 while a setup runs |
| GET | `/api/v1/action/trainer/status` | JSON `{enabled, mirror, state, app_connected, setup, name, mac}` (same data as the `TRAINER_STATUS` frame) |

409 answers (text): `Besides the heart-rate strap, at most two devices`, `This device is the trainer`, `Discovery or trainer setup running`, `No trainer set up`, `Discovery result no longer valid`. Details: `docs/05_APIS.md`.

### Fan Actions

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/api/v1/action/fan/add` | Add fan by 32-hex token (`?token=`). Used by WebUI and by Token Extractor / external tools — see `docs/05_APIS.md` |
| POST | `/api/v1/action/fan/update` | Replace a fan token (`old_token`, `new_token`) |
| POST | `/api/v1/action/fan/control` | Control fan (Power, Speed, etc.) |
| POST | `/api/v1/action/fan/step` | Shift HR range ±5 bpm (`dir=up|down`; up = more wind; since 2.3) |
| POST | `/api/v1/action/fanmode/set` | Set fan mode / FTP / cooldown |
| POST | `/api/v1/action/fanmode/cycle` | Cycle Pure → Smart → Hybrid → Pure (since 2.3) |
| POST | `/api/v1/action/fan/discovery/start` | Start fan discovery |
| POST | `/api/v1/action/fan/discovery/stop` | Stop fan discovery |
| POST | `/api/v1/action/fan/remove` | Remove fan |

### Firmware Actions

| Method | Route | Description |
|--------|-------|-------------|
| POST | `/api/v1/action/firmware/search` | Search for firmware |
| POST | `/api/v1/action/firmware/download` | Download firmware |
| POST | `/api/v1/action/firmware/install` | Install firmware |
| POST | `/api/v1/action/firmware/abort` | Abort firmware update |

### System Actions

| Method | Route | Description |
|--------|-------|-------------|
| GET | `/api/v1/system/info` | System info (`device_id`, `hostname` since 2.2; key=value) |
| GET | `/api/v1/state` | Status snapshot for Karoo/HTTP clients (since 2.3; key=value) |
| POST | `/api/v1/action/system/restart` | Restart device (settings preserved) |
| POST | `/api/v1/action/system/wifi_reset` | WiFi reset (clear WiFi, reboot; WML only) |
| POST | `/api/v1/action/system/factory_reset` | Factory reset (clear all, reboot) |
| POST | `/api/v1/action/system/maintenance/begin` | Begin maintenance mode (debug) |
| POST | `/api/v1/action/system/maintenance/end` | No-op since 1.6.0 (409, maintenance ends with a reboot) |
| GET | `/api/v1/action/system/maintenance/status` | Maintenance state, `restart_required`, factory-reset report (JSON) |

**Response Format**: `text/plain` (no JSON) for most; action endpoints return `"OK"` or `"ERR:..."`

## API v1 - Settings

**File**: `ApiSettings.cpp` → `registerRoutes()`

Settings API for persistent configuration.

| Method | Route | Description |
|--------|-------|-------------|
| GET | `/api/v1/settings/get?key=<name>` | Get setting value |
| POST | `/api/v1/settings/set?key=<name>&value=<value>` | Set setting |
| GET | `/api/v1/settings/list` | List all settings |

**Response Format**: `text/plain` (no JSON)

**Parameters**:
- `key`: Setting name (String)
- `value`: Setting value (String, parsed according to setting type)

## WebSocket Telemetry

**File**: `WebSocketTelemetry.cpp` → `begin()`

| Endpoint | Protocol | Description |
|----------|----------|-------------|
| `/ws` | WebSocket | Binary telemetry (Server → Client) |

### WebSocket Protocol

**Format**: Binary (not text)

**Frame Structure**: `[type:uint8][payload:variable]`

**Frame Types**:

| ID | Type | Description |
|----|------|-------------|
| 0x01 | `SYSTEM_STATUS` | System health, uptime, memory (Snapshot) |
| 0x02 | `SENSORS_SNAPSHOT` | All configured sensors (Snapshot) |
| 0x03 | `SENSOR_CONN_STATE` | Sensor connection state (Delta) |
| 0x04 | `DISCOVERY_STATUS` | Discovery status (Delta) |
| 0x05 | `DISCOVERY_RESULTS` | Discovery results (Delta) |
| 0x06 | `HEART_RATE` | Heart rate value (Delta) |
| 0x07 | `FANS_SNAPSHOT` | All configured fans (Snapshot) |
| 0x08 | `FIRMWARE_VERSION` | Firmware version (Delta) |
| 0x09 | `FIRMWARE_PROGRESS` | Firmware update progress (Delta) |
| 0x0D | `BIKE_METRICS` | Power and cadence with validity flags (Delta) |
| 0x0E | `FAN_CONTROL` | Fan mode and acknowledged fan target (Delta; `HW_ENABLE_FAN_POWER_MODE`, on in release builds) |
| 0x0F | `TRAINER_STATUS` | Trainer mirror state, flags, setup result, name (Delta; `HW_ENABLE_TRAINER_MIRROR` only) |
| 0x10 | `APP_STATUS` | Number of apps (BLE centrals) connected to Homewind (Delta; `HW_ENABLE_BLE`) |

**Behavior**:
- **Snapshot Frames**: Full state (on connect + periodically every 30s)
- **Delta Frames**: Incremental updates (on state changes)
- **Reconnect Safety**: On client connect, all snapshots are sent

**Detailed Documentation**: See `WEBSOCKET_PROTOCOL.md`

## Registration

Routes are registered in `App.cpp` → `initWeb()`:

```cpp
// Static Assets: Automatically in WebServerManager::begin()
webServer->begin();

// API Actions: TODO - not automatically registered yet
// ApiActions* actions = ...;
// actions->registerRoutes(webServer->getServer());

// API Settings: TODO - not automatically registered yet
// ApiSettings* settings = ...;
// settings->registerRoutes(webServer->getServer(), this->settings);

// WebSocket: Automatically in WebSocketTelemetry::begin()
telemetry->begin(webServer->getServer());
```

## Feature Flags

Endpoints are controlled by the following feature flags:

- `HW_ENABLE_WEB` (default: 1) - Enable WebServer
- `HOMEWIND_ENABLE_API_ACTIONS` (default: `HW_ENABLE_WEB`) - Enable Action API
- `HOMEWIND_ENABLE_API_SETTINGS` (default: `HW_ENABLE_WEB && HW_ENABLE_NVS`) - Enable Settings API
- `HOMEWIND_ENABLE_WEBSOCKET` (default: 0) - Enable WebSocket

See `src/app/Config.h` for details.

## Ports

- **HTTP/WebServer**: Port 80 (defined in `HOMEWIND_WEBSERVER_PORT`)
- **WebSocket**: Runs on the same port as WebServer (via `/ws` endpoint)

## Example Requests

### Connect Sensor
```bash
curl -X POST "http://192.168.1.100/api/v1/action/sensor/connect?name=HR_Sensor"
```

### Get Setting
```bash
curl "http://192.168.1.100/api/v1/settings/get?key=wifi_ssid"
```

### Set Setting
```bash
curl -X POST "http://192.168.1.100/api/v1/settings/set?key=wifi_ssid&value=MyWiFi"
```

### Connect WebSocket
```javascript
const ws = new WebSocket('ws://192.168.1.100/ws');
ws.binaryType = 'arraybuffer';
ws.onmessage = (event) => {
  const data = new Uint8Array(event.data);
  const frameType = data[0];
  const payload = data.slice(1);
  // Process frame...
};
```
