# Homewind Architecture

## System Architecture Overview

Homewind follows a **modular, layered architecture** with clear boundaries and optional feature gates. The design emphasizes:
- **Non-blocking operations** throughout
- **Deterministic boot sequence** with timeouts
- **Resource efficiency** (static buffers, PROGMEM)
- **Maintainability** (clear module boundaries)

---

## Architecture Layers

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application Layer                         │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  BLE Relay   │  │ Fan Control  │  │ Firmware OTA │          │
│  │   Manager    │  │   Manager    │  │   Manager    │          │
│  │  (Complete)  │  │  (Complete)  │  │  (Complete)  │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                         Core Services                            │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │                     App Coordinator                         │ │
│  │  • Boot Orchestrator (deterministic phases)                │ │
│  │  • Subsystem lifecycle management                           │ │
│  │  • State coordination                                       │ │
│  └────────────────────────────────────────────────────────────┘ │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │ System State │  │   Settings   │  │ Maintenance  │          │
│  │  Management  │  │    Store     │  │     Mode     │          │
│  │  (Singleton) │  │   (NVS stub) │  │   (Gate)     │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                          Web Layer                               │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │ Web Server   │  │ WebSocket    │  │  HTTP API    │          │
│  │              │  │ Telemetry    │  │              │          │
│  │ • Static     │  │ • Binary     │  │ • Actions    │          │
│  │   Assets     │  │   Protocol   │  │ • Settings   │          │
│  │ • ETag/      │  │ • Snapshot/  │  │ • Factory    │          │
│  │   Caching    │  │   Delta      │  │   Reset      │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────────┐
│                       Platform Layer                             │
│  ┌──────────────┐  ┌──────────────┐                            │
│  │   WiFi       │  │    NVS       │                            │
│  │   Service    │  │   Storage    │                            │
│  │ • STA mode   │  │ • Settings   │                            │
│  │ • AP fallback│  │ • Persistent │                            │
│  │ • Non-block  │  │   storage    │                            │
│  └──────────────┘  └──────────────┘                            │
└─────────────────────────────────────────────────────────────────┘
                              │
                              ▼
                    ┌─────────────────┐
                    │   ESP32 Core    │
                    │  (Arduino API)  │
                    └─────────────────┘
```

---

## Module Organization

### Application Layer (`src/app/`)

**App Coordinator** (`App.h/cpp`)
- Boot orchestrator with deterministic phases
- Subsystem lifecycle management
- Feature gate coordination

**Configuration** (`Config.h`)
- Compile-time feature gates
- Default values and timeouts
- Build configuration checks

**Build Info** (`BuildInfo.h`)
- Version information
- Build metadata

### Core Services (`src/core/`)

**SystemState** (`SystemState.h/cpp`)
- Global state management (singleton namespace)
- Per-module readiness flags
- Uptime and memory tracking

**FirmwareUpdateManager** (`FirmwareUpdateManager.h/cpp`)
- OTA update state machine
- GitHub integration
- Progress tracking and callbacks

**FanController** (`FanController.h/cpp`) ✅ **Implemented** – SmartMiFanAsync integration
- Interface for fan management
- Needs SmartMiFanAsync integration

**BLERelayManager** (`BLERelayManager.h/cpp`) ✅ **Implemented**
- BLE sensor relay management (HR, CSC sensors)
- Integrated with BluetoothBikeSensorServer library
- Persistent configuration via NVSConfigBus
- WebSocket telemetry integration
- Sensor discovery and connection management

**MaintenanceMode** (`MaintenanceMode.h/cpp`)
- API endpoint gate during critical operations
- Prevents conflicts during OTA/factory reset

**Timing** (`Timing.h`)
- Non-blocking timing utilities

### Web Layer (`src/web/`)

**WebServer** (`WebServer.h/cpp`)
- Static asset serving (PROGMEM)
- ETag support and caching headers
- Route registration

**WebSocketTelemetry** (`WebSocketTelemetry.h/cpp`)
- Binary telemetry protocol
- Snapshot/delta frame model
- Dirty-flag + loop-flush architecture

**ApiActions** (`ApiActions.h/cpp`)
- HTTP action handlers (sensors, fans, firmware, system)
- Form-urlencoded request handling
- Maintenance mode checking

**ApiSettings** (`ApiSettings.h/cpp`)
- HTTP settings API (get/set/list)
- SettingsStore integration

### Platform Layer

**WiFi Service** (`src/wifi/WiFiService.h/cpp`)
- STA connection with timeout
- AP fallback mode
- Non-blocking connection handling

**Settings Store** (`src/settings/SettingsStore.h/cpp`) 🚧 **Stub**
- Interface for persistent storage
- NVS implementation pending

---

## Feature Gates

All features are **compile-time configurable** via `src/app/Config.h`. Feature gates use preprocessor directives (`#if HW_ENABLE_*`) to exclude code entirely from builds where features are disabled.

### Feature Gate Hierarchy

```
HW_ENABLE_NVS (Settings Store)
    └─> HOMEWIND_ENABLE_SETTINGS
            └─> HOMEWIND_ENABLE_API_SETTINGS (requires HW_ENABLE_WEB)

HW_ENABLE_WIFI (WiFi Service)
    └─> HW_ENABLE_WEB (Web Server)
            ├─> HOMEWIND_ENABLE_WEBSERVER (alias)
            ├─> HOMEWIND_ENABLE_WEBSOCKET (WebSocket Telemetry)
            ├─> HOMEWIND_ENABLE_API_ACTIONS (Action API)
            └─> HOMEWIND_ENABLE_API_SETTINGS (Settings API, also requires HW_ENABLE_NVS)

HW_ENABLE_BLE (BLE Relay)
HW_ENABLE_FANS (Fan Controller)
HW_ENABLE_OTA (Firmware Updates - requires GitFirmwareUpdate library)
```

### Default Configuration

```cpp
// Core services (always enabled)
HW_ENABLE_NVS = 1          // Settings store
HW_ENABLE_WIFI = 1         // WiFi service
HW_ENABLE_WEB = 1          // Web server

// Background services (default disabled)
HW_ENABLE_BLE = 0          // BLE relay (optional)
HW_ENABLE_FANS = 0         // Fan controller (optional)
HW_ENABLE_OTA = 1          // Firmware updates

// Web features (default enabled if web enabled)
HOMEWIND_ENABLE_WEBSOCKET = 1      // WebSocket telemetry
HOMEWIND_ENABLE_API_ACTIONS = 1    // Action API
HOMEWIND_ENABLE_API_SETTINGS = 1   // Settings API (if NVS enabled)
```

### Feature Gate Benefits

1. **Zero Runtime Overhead**: Disabled features add no code or memory
2. **Minimal Dependencies**: Can compile with only ESP32 Arduino Core
3. **Flexible Builds**: Choose features based on use case
4. **Green Build**: Compiles successfully with all features disabled

---

## Module Boundaries

### App Coordinator
- **Manages**: Boot sequence, subsystem initialization, feature gate coordination
- **Depends on**: SystemState, all subsystem headers (conditionally)
- **Used by**: Main sketch (`Homewind.ino`)

### SystemState
- **Manages**: Global state, readiness flags, uptime/memory
- **Depends on**: Config.h (for feature gates)
- **Used by**: App, WebServer, API handlers

### WebServer
- **Manages**: HTTP server, static assets, route registration
- **Depends on**: ESPAsyncWebServer (if enabled)
- **Used by**: App, ApiActions, ApiSettings, WebSocketTelemetry

### WiFiService
- **Manages**: WiFi connection, STA/AP modes, connection state
- **Depends on**: ESP32 WiFi library
- **Used by**: App, SystemState (for WiFi readiness)

### SettingsStore
- **Manages**: Persistent settings storage
- **Depends on**: NVS/Preferences (pending implementation)
- **Used by**: App, ApiSettings, ApiActions (factory reset)

### FirmwareUpdateManager
- **Manages**: OTA update state machine, GitHub integration
- **Depends on**: GitFirmwareUpdate library
- **Used by**: App, ApiActions

---

## Design Patterns

### 1. Boot Orchestrator Pattern
Deterministic, non-blocking boot sequence with phases and readiness gates.

**Implementation**: `App.h/cpp` → `advanceBootPhase()`

See: [03_BOOT_FLOW.md](./03_BOOT_FLOW.md)

### 2. Dirty-Flag + Loop-Flush Pattern
Mark state as dirty from async contexts, flush from main loop.

**Implementation**: `WebSocketTelemetry.h/cpp`

```cpp
// From async context (BLE callback, etc.)
telemetry->markDirty(TelemetryFrameType::HEART_RATE);

// From main loop
telemetry->loopFlush();  // Sends all dirty frames
```

### 3. State Machine Pattern
Explicit state machines for complex operations (firmware updates).

**Implementation**: `FirmwareUpdateManager.h/cpp`

See: [06_STATE_MACHINES.md](./06_STATE_MACHINES.md)

### 4. Feature Gate Pattern
Compile-time feature selection with zero runtime overhead.

**Implementation**: `Config.h` → `#if HW_ENABLE_*`

### 5. Singleton Namespace Pattern
Global state via namespace (not class singleton).

**Implementation**: `SystemState.h/cpp`

```cpp
namespace SystemState {
  void setServicesReady(bool ready);
  bool isReady();
  // ...
}
```

---

## Memory Management

### PROGMEM for Web Assets
All web assets (HTML, CSS, JS) are stored in PROGMEM (flash), not RAM:
- `generated/webui_*.h` - Auto-generated PROGMEM headers
- Zero RAM usage for assets
- Direct flash access via `pgm_read_byte()`

### Static Buffers
- WebSocket frame buffer: 1KB static buffer (no heap allocation)
- HTTP response buffers: Fixed-size stack buffers (512 bytes) for deterministic responses
- String buffers: Static char arrays where possible
- Minimal heap allocation in hot paths

### Bounded Text Building
- **TextUtil** (`src/core/TextUtil.h/cpp`): Helper functions for safe text building
  - `appendf()`: Append formatted text to fixed buffer
  - `truncate_ascii()`: Safe string truncation with ellipsis
- HTTP endpoints use fixed-size buffers instead of `String` concatenation
- Hard limits enforced: version (32 chars), build ID (8 chars), release notes (160 chars)

### Heap Allocation
- System objects allocated once during initialization
- No allocation in main loop (telemetry, etc.)
- Feature-gated objects only allocated if enabled

---

## Thread Safety

### Single-Threaded Main Loop
All code runs in the main `loop()` function (single thread). Async operations use callbacks but should not directly modify shared state.

### Dirty-Flag Pattern for Async Updates
Async callbacks (BLE, WiFi events) mark state as dirty. Main loop flushes dirty state safely.

**Example**:
```cpp
// Async callback (interrupt context)
void onBLEHeartRate(uint16_t bpm) {
  heartRate = bpm;
  telemetry->markDirty(TelemetryFrameType::HEART_RATE);  // Safe
}

// Main loop
void loop() {
  telemetry->loopFlush();  // Sends dirty frames
}
```

### Mutex for Shared State (FirmwareUpdateManager)
`FirmwareUpdateManager` uses mutexes for state access from worker tasks.

---

## Error Handling

### Boot Phase Failures
- Phase timeouts trigger degraded mode or skip to next phase
- Non-critical failures don't block boot

### API Errors
- HTTP endpoints return `text/plain` with "OK" or "ERR:message"
- No JSON error objects (simplified parsing)

### Maintenance Mode
- Blocks all non-whitelisted API endpoints during OTA/factory reset
- Returns `503 Service Unavailable` with reason

---

## Extension Points

### Adding a New Feature Module

1. **Define feature gate** in `Config.h`:
   ```cpp
   #ifndef HW_ENABLE_MY_FEATURE
   #define HW_ENABLE_MY_FEATURE 0
   #endif
   ```

2. **Create module** in appropriate directory:
   ```
   src/myfeature/
     ├── MyFeature.h
     └── MyFeature.cpp
   ```

3. **Add to App** (`App.h/cpp`):
   ```cpp
   #if HW_ENABLE_MY_FEATURE
   class MyFeature;
   #endif
   
   #if HW_ENABLE_MY_FEATURE
     MyFeature* myFeature;
     bool myFeatureReady;
   #endif
   ```

4. **Initialize in boot phase** (`App.cpp`):
   ```cpp
   void App::initMyFeature() {
     #if HW_ENABLE_MY_FEATURE
       if (myFeatureReady) return;
       myFeature = new MyFeature();
       if (myFeature->begin()) {
         myFeatureReady = true;
       }
     #endif
   }
   ```

5. **Update SystemState** if needed (add readiness flag)

See: [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) → "Extending the Library"

---

## Related Documentation

- [03_BOOT_FLOW.md](./03_BOOT_FLOW.md) - Boot orchestrator details
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation
- [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) - Development guide

