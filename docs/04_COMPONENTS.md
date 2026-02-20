# Component Documentation

This document provides detailed documentation for all major components in the Homewind system.

---

## Application Layer

### App (Application Coordinator)

**File**: `src/app/App.h`, `src/app/App.cpp`

**Purpose**: Main application coordinator that manages boot orchestrator and subsystem lifecycle.

**Key Responsibilities**:
- Boot orchestrator with deterministic phases
- Subsystem initialization and lifecycle management
- Feature gate coordination
- State coordination between subsystems

**Public API**:
```cpp
class App {
public:
  App();
  ~App();
  
  bool begin();              // Initialize boot orchestrator
  void loop();               // Main loop (call from Arduino loop())
  bool isReady() const;      // Check if all critical services ready
  BootPhase getPhase() const; // Get current boot phase
};
```

**Boot Phases**: See [03_BOOT_FLOW.md](./03_BOOT_FLOW.md)

**Integration**: Main sketch (`Homewind.ino`) creates global `App` instance.

---

### Config (Configuration)

**File**: `src/app/Config.h`

**Purpose**: Compile-time configuration and feature gates.

**Key Definitions**:
- Feature gates: `HW_ENABLE_*`
- Default values: Ports, timeouts, credentials
- Build configuration checks

**Feature Gates**: See [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) → "Feature Gates"

**Usage**: Include before other headers to enable/disable features.

---

### BuildInfo (Version Information)

**File**: `src/app/BuildInfo.h`

**Purpose**: Build metadata (version, build ID).

**API**:
```cpp
namespace BuildInfo {
  const char* getName();     // "Homewind"
  const char* getVersion();  // "1.4.x" (semantic versioning)
  const char* getBuildId();  // "YYYYMMDD-HHMMSS" (timestamp or git hash)
}
```

**Version Source**: Defined via preprocessor (`FW_NAME`, `FW_VERSION`, `FW_BUILD_ID`).

---

## Core Services

### TextUtil (Bounded Text Building)

**File**: `src/core/TextUtil.h`, `src/core/TextUtil.cpp`

**Purpose**: Helper utilities for deterministic, bounded text building in HTTP responses.

**API**:
```cpp
// Append formatted text to fixed buffer
bool appendf(char* dst, size_t dstSize, size_t& used, const char* fmt, ...);

// Truncate ASCII string safely
size_t truncate_ascii(char* dst, size_t dstSize, const char* src, 
                      size_t maxChars, bool addEllipsis);
```

**Features**:
- Fixed-size buffer operations (no heap allocation)
- Automatic truncation detection
- Safe NUL termination
- UTF-8 ellipsis support

**Usage**: Used by HTTP API handlers to build responses deterministically.

---

### SystemState (Global State Management)

**File**: `src/core/SystemState.h`, `src/core/SystemState.cpp`

**Purpose**: Global state management with per-module readiness flags (singleton namespace).

**API**:
```cpp
namespace SystemState {
  void update();                    // Update state (call from loop)
  bool isReady();                   // Check if all critical services ready
  void setServicesReady(bool ready); // Set overall readiness
  
  uint32_t getUptimeMs();           // System uptime in milliseconds
  uint32_t getFreeHeap();           // Free heap in bytes
  uint32_t getLargestFreeBlock();   // Largest free block in bytes
  
  // Per-module readiness (feature-gated)
  void setNVSReady(bool ready);
  bool isNVSReady();
  
  void setWiFiReady(bool ready);
  bool isWiFiReady();
  
  void setWebReady(bool ready);
  bool isWebReady();
  
  void setWebSocketReady(bool ready);
  bool isWebSocketReady();
  
  // ... other readiness flags
}
```

**State Flags**:
- `servicesReady`: Overall system readiness (set when `PH_RUNNING` reached)
- `nvsReady`: NVS store readiness
- `wifiReady`: WiFi connection readiness
- `webReady`: Web server readiness
- `wsReady`: WebSocket telemetry readiness
- `bleReady`: BLE relay readiness
- `fansReady`: Fan controller readiness

**Usage**: Subsystems call `set*Ready()` to update state. API handlers check `isReady()` before processing requests.

---

### FirmwareUpdateManager (OTA Update Manager)

**File**: `src/core/FirmwareUpdateManager.h`, `src/core/FirmwareUpdateManager.cpp`

**Purpose**: Manages firmware updates via GitHub releases with state machine and JavaScript callback injection.

**Key Responsibilities**:
- Firmware search (check GitHub for latest release)
- Firmware download (async with progress tracking)
- Firmware installation (reboot into new firmware)
- Progress reporting via WebSocket or JavaScript callbacks

**Public API**:
```cpp
class FirmwareUpdateManager {
public:
  bool begin(const char* currentVersion, const char* githubUrl);
  void loop();  // MUST be called periodically
  
  bool isReady() const;
  bool isUpdating() const;
  
  // Async operations
  bool requestSearch();
  bool requestDownload();
  bool startInstall();
  bool abort();
  
  // Sync download (for sync endpoint)
  bool beginDownloadSync(uint32_t* outSeq);
  bool waitDownloadDone(uint32_t seq, uint32_t timeoutMs, 
                        bool* okOut, String* errOut);
  
  // State queries
  String getStateString() const;  // "idle", "searching", "downloading", etc.
  int getProgress() const;        // 0-100
  String getLastError() const;
  String getRemoteVersion() const;
  String getReleaseNotes() const;
  
  // JavaScript bridge injection
  void injectJavaScriptBridge(String& html);
  void setWebServer(AsyncWebServer* server);
};
```

**State Machine**: See [06_STATE_MACHINES.md](./06_STATE_MACHINES.md) → "Firmware Update State Machine"

**Worker Task**: Async operations run in separate FreeRTOS task.

**Thread Safety**: Uses mutexes for state access from worker tasks.

---

### FanController (Fan Controller)

**File**: `src/core/FanController.h`, `src/core/FanController.cpp`

**Status**: ✅ **Implemented** – Fully integrated with SmartMiFanAsync library

**Purpose**: Manages Xiaomi Smart Mi Fan devices with SmartConnect discovery and NVS configuration.

**Key Responsibilities**:
- Fan discovery (SmartConnect)
- Fan control state (on/off) with UDP commands
- Telemetry integration (markTelemetryDirty → WebSocket, markFansDirty → DisplayManager)
- NVS persistence for fan configurations

**Integration**: SmartMiFanAsync library. Display updates via `DisplayManager::markFansDirty()` (deferred, no direct LVGL calls).

---

### DisplayManager (Display Integration)

**File**: `src/core/DisplayManager.h`, `src/core/DisplayManager.cpp`

**Status**: ✅ **Implemented** – Integrated with HomeWindWSAmoled library (compile-time: `HW_ENABLE_DISPLAY`)

**Purpose**: Bridges Homewind data (HR, CSC, Fans, URL) to the AMOLED display. Uses deferred dirty-flag pattern for safe cross-task LVGL updates.

**Architecture**: All `homewind_set_*` (LVGL) calls are centralized in `DisplayManager::loop()` under `lcd_lvgl_lock()`. BLE/Fan callbacks only set dirty flags and cache values – no direct LVGL calls from NimBLE or Arduino tasks.

**API**:
```cpp
class DisplayManager {
public:
  bool begin();
  void loop();           // Call from App::loopSubsystems() – flushes dirty flags under LVGL mutex
  
  void setHeartRate(uint16_t bpm);
  void updateHRState();
  void setCadence(uint16_t rpm);
  void updateCSCState();
  void updateFans();
  void markFansDirty();  // Deferred update (safe from any task, incl. LVGL event callback)
  void setUrl(const char* url);
};
```

**See**: [DISPLAY_UPDATE_BUG_ANALYSIS.md](./DISPLAY_UPDATE_BUG_ANALYSIS.md), [DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md](./DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md)

---

### BLERelayManager (BLE Sensor Relay)

**File**: `src/core/BLERelayManager.h`, `src/core/BLERelayManager.cpp`

**Status**: ✅ **Implemented** - Fully integrated with BluetoothBikeSensorServer library

**Purpose**: Manages BLE sensor relay (HR, CSC sensors) with persistent configuration and WebSocket telemetry.

**API**:
```cpp
class BLERelayManager {
public:
  BLERelayManager();
  ~BLERelayManager();
  
  bool begin(WebSocketTelemetry* telemetry = nullptr);
  void loop();
  bool ready() const;
  
  // Sensor Management
  bool loadConfiguredSensors();
  bool getSensorConnectionState(const char* sensorName, String& type, 
                                String& mac, bool& connected, int& battery);
  uint8_t getConfiguredSensors(SensorInfo* sensors, uint8_t maxSensors);
  bool setConfiguredSensor(const char* name, const char* type, const char* mac = nullptr);
  bool deleteConfiguredSensor(const char* name);
  bool connectSensor(const char* name);
  bool disconnectSensor(const char* name);
  bool reloadSensors();
  
  // Discovery
  bool startDiscovery(const char* type = nullptr);
  bool stopDiscovery();
  uint8_t getDiscoveryResults(DiscoveredSensor* results, uint8_t maxResults);
  
  // Server Control
  bool pauseServer();
  bool resumeServer();
  bool isServerPaused() const;
  
  // Maintenance mode
  void stopForMaintenance();
  bool isStoppedForMaintenance() const;
};
```

**Integration**: 
- ✅ Integrated with BluetoothBikeSensorServer library
- ✅ Uses NVSConfigBus for persistent sensor configuration
- ✅ WebSocket telemetry for real-time status updates
- ✅ HTTP API endpoints in ApiActions.cpp
- ✅ Initialized in App::initBLE() after boot phase PH_RUNNING

---

### MaintenanceMode (Maintenance Mode Gate)

**File**: `src/core/MaintenanceMode.h`, `src/core/MaintenanceMode.cpp`

**Purpose**: Blocks non-whitelisted API endpoints during critical operations (OTA, factory reset).

**API**:
```cpp
enum MaintenanceReason {
  UNKNOWN = 0,
  OTA = 1,
  FACTORY_RESET = 2
};

namespace MaintenanceMode {
  void begin(MaintenanceReason reason);
  void end();
  bool isActive();
  MaintenanceReason reason();
  const char* reasonString();  // "ota", "factory_reset", "unknown"
}
```

**Behavior**:
- Blocks all non-whitelisted API endpoints with `503 Service Unavailable`
- Stops background services (fans, BLE) for maintenance
- Idempotent (safe to call multiple times)

**Usage**: Called by `FirmwareUpdateManager` before install, by `ApiActions` during factory reset.

---

### Timing (Non-Blocking Timing Utilities)

**File**: `src/core/Timing.h`

**Purpose**: Non-blocking timing utilities (header-only).

**Utilities**: Check file for available timing helpers.

---

## Web Layer

### WebServer (Web Server Manager)

**File**: `src/web/WebServer.h`, `src/web/WebServer.cpp`

**Purpose**: Manages HTTP server with static asset serving (PROGMEM-based).

**Key Responsibilities**:
- Static asset serving (HTML, CSS, JS from PROGMEM)
- ETag support for bandwidth optimization
- Cache-Control headers
- Route registration for API handlers

**API**:
```cpp
class WebServerManager {
public:
  WebServerManager();
  ~WebServerManager();
  
  bool begin();
  void loop();
  
  AsyncWebServer* getServer();  // Get server instance for route registration
};
```

**Asset Serving**:
- Assets embedded in PROGMEM via `generated/webui_*.h`
- Automatic ETag generation from content hash
- GZIP compression support
- Cache-busting via query parameter (`?v=HASH`)

**Dependencies**: ESPAsyncWebServer library (if enabled)

---

### WebSocketTelemetry (WebSocket Binary Telemetry)

**File**: `src/web/WebSocketTelemetry.h`, `src/web/WebSocketTelemetry.cpp`

**Purpose**: Binary WebSocket telemetry protocol for server→client data streaming.

**Frame Types**:
- Snapshot frames: `SYSTEM_STATUS` (0x01), `SENSORS_SNAPSHOT` (0x02), `FANS_SNAPSHOT` (0x07)
- Delta frames: `SENSOR_CONN_STATE` (0x03), `DISCOVERY_STATUS` (0x04), `DISCOVERY_RESULTS` (0x05), `HEART_RATE` (0x06), `FIRMWARE_VERSION` (0x08), `FIRMWARE_PROGRESS` (0x09), `FIRMWARE_SEARCH_RESULT` (0x0A)

**Firmware Search Results**:
- `FIRMWARE_SEARCH_RESULT` (0x0A) frame delivers firmware update search results
- Includes: update availability, version (max 32 chars), release notes (max 160 chars)
- Replaces JSON in HTTP responses for real-time updates

**Key Responsibilities**:
- Binary frame encoding/decoding
- Snapshot/delta frame model
- Dirty-flag + loop-flush architecture
- Reconnect-safe (full snapshot on connect)

**API**:
```cpp
class WebSocketTelemetry {
public:
  WebSocketTelemetry();
  ~WebSocketTelemetry();
  
  bool begin(AsyncWebServer* webServer);
  void loopFlush();  // Call from main loop
  
  void markDirty(TelemetryFrameType frameType);  // Call from async contexts
  AsyncWebSocket* getWebSocket();
};
```

**Frame Types**: See [05_APIS.md](./05_APIS.md) → "WebSocket Protocol"

**Architecture**: Dirty-flag model - mark dirty from async contexts, flush from main loop.

**Memory**: Uses static 1KB frame buffer (no heap allocation).

---

### ApiActions (HTTP Action API)

**File**: `src/web/ApiActions.h`, `src/web/ApiActions.cpp`

**Purpose**: HTTP action handlers for sensors, fans, firmware, and system operations.

**Endpoints**: See [05_APIS.md](./05_APIS.md) → "HTTP API Endpoints"

**API**:
```cpp
class ApiActions {
public:
  ApiActions();
  void registerRoutes(void* server = nullptr);
  
  static void setSettingsStore(void* store);  // For factory reset
};
```

**Request Format**: Form-urlencoded POST (query params or body)

**Response Format**: `text/plain` ("OK" or "ERR:message")

**HTTP Codes**:
- `200 OK`: Success
- `400 Bad Request`: Invalid parameters
- `409 Conflict`: Operation conflict (e.g., already connected)
- `500 Server Error`: Internal error
- `503 Service Unavailable`: System not ready or maintenance mode

**Maintenance Mode**: Checks `MaintenanceMode::isActive()` and blocks non-whitelisted endpoints.

---

### ApiSettings (HTTP Settings API)

**File**: `src/web/ApiSettings.h`, `src/web/ApiSettings.cpp`

**Purpose**: HTTP settings API for persistent configuration.

**Endpoints**:
- `GET /api/v1/settings/get?key=<name>` - Get setting value
- `POST /api/v1/settings/set?key=<name>&value=<value>` - Set setting
- `GET /api/v1/settings/list` - List all settings

**Response Format**: `text/plain`

**Dependencies**: `SettingsStore` (requires `HW_ENABLE_NVS == 1`)

---

## Platform Layer

### WiFiService (WiFi Management)

**File**: `src/wifi/WiFiService.h`, `src/wifi/WiFiService.cpp`

**Purpose**: WiFi connection management with STA mode and AP fallback.

**Key Responsibilities**:
- STA connection with compile-time credentials
- AP fallback on connection failure (if enabled)
- Non-blocking connection handling
- Connection state monitoring

**API**:
```cpp
class WiFiService {
public:
  WiFiService();
  ~WiFiService();
  
  void begin();          // Start WiFi connection attempt
  void loop();           // Update connection state (call from main loop)
  bool ready() const;    // Check if ready (STA connected OR AP active)
  
  bool isSTA() const;    // Check if STA connected
  bool isAP() const;     // Check if AP active
  
  const char* ipString() const;   // Get IP address as string
  const char* modeString() const; // Get mode as string ("STA", "AP", etc.)
};
```

**Configuration**: 
- STA credentials: `HW_WIFI_SSID`, `HW_WIFI_PASS` (compile-time)
- AP credentials: `HW_WIFI_AP_SSID`, `HW_WIFI_AP_PASS` (compile-time)
- Timeout: `HW_WIFI_TIMEOUT_MS` (default: 20 seconds)
- AP fallback: `HW_WIFI_AP_FALLBACK` (default: 1, enabled)

**Behavior**:
- Starts STA connection attempt on `begin()`
- Monitors connection in `loop()`
- Activates AP fallback on timeout (if enabled)
- Returns `ready() == true` when STA connected OR AP active

---

### SettingsStore (Settings Storage)

**File**: `src/settings/SettingsStore.h`, `src/settings/SettingsStore.cpp`

**Status**: 🚧 **Stub** - Interface complete, NVS implementation pending

**Purpose**: Persistent settings storage (key-value pairs).

**API**:
```cpp
class SettingsStore {
public:
  SettingsStore();
  ~SettingsStore();
  
  bool begin();
  
  String get(const String& key);
  bool set(const String& key, const String& value);
  bool remove(const String& key);
  String list(const String& prefix);
  bool exists(const String& key);
  bool factoryReset();  // Clear all settings
};
```

**Backend**: NVS/Preferences (pending implementation)

**Usage**: Used by `ApiSettings` and `App` for persistent configuration.

---

## Component Dependencies

### Dependency Graph

```
App
  ├─> SystemState
  ├─> WiFiService
  │   └─> ESP32 WiFi
  ├─> SettingsStore
  │   └─> NVS (pending)
  ├─> WebServer
  │   └─> ESPAsyncWebServer
  ├─> WebSocketTelemetry
  │   ├─> WebServer
  │   └─> SystemState
  ├─> ApiActions
  │   ├─> WebServer
  │   ├─> FirmwareUpdateManager
  │   ├─> SettingsStore
  │   └─> MaintenanceMode
  ├─> ApiSettings
  │   ├─> WebServer
  │   └─> SettingsStore
  ├─> FirmwareUpdateManager
  │   ├─> GitFirmwareUpdate
  │   └─> WebServer (for callback injection)
  ├─> FanController
  │   └─> SmartMiFanAsync
  ├─> DisplayManager (if HW_ENABLE_DISPLAY)
  │   └─> HomeWindWSAmoled
  └─> BLERelayManager
      └─> BluetoothBikeSensorServer
```

---

## Component Lifecycle

### Initialization Order

1. **App::begin()** - Boot orchestrator starts
2. **PH_CORE** - Core initialization
3. **PH_NVS** - SettingsStore::begin()
4. **PH_WIFI** - WiFiService::begin()
5. **PH_WEB** - WebServer::begin(), WebSocketTelemetry::begin()
6. **PH_RUNNING** - SystemState::setServicesReady(true)
7. **Background** - FirmwareUpdateManager::begin(), FanController::begin(), BLERelayManager::begin()

### Runtime Loop

All components' `loop()` methods are called from `App::loop()`:

```cpp
void App::loop() {
  SystemState::update();
  
  // Advance boot phase if needed
  advanceBootPhase();
  
  // Loop subsystems
  loopSubsystems();  // Calls all subsystem loop() methods
}
```

---

## Related Documentation

- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture
- [03_BOOT_FLOW.md](./03_BOOT_FLOW.md) - Boot sequence
- [05_APIS.md](./05_APIS.md) - API documentation
- [06_STATE_MACHINES.md](./06_STATE_MACHINES.md) - State machines

