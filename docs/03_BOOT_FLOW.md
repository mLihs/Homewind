# Boot Flow & Initialization

## Overview

Homewind uses a **deterministic boot orchestrator** with fixed phases, timeouts, and readiness gates. The boot sequence is **non-blocking** (no `delay()` calls) and handles failures gracefully by entering degraded mode or skipping non-critical phases.

---

## Boot Phases

The boot orchestrator (`App.h/cpp`) executes phases in a **fixed order**:

```
PH_CORE → PH_NVS → PH_WIFI → PH_WEB → PH_RUNNING
                     │
                     └─> [timeout] → PH_DEGRADED (optional)
```

### Phase Enumeration

```cpp
enum class BootPhase {
  PH_CORE = 0,      // Core initialization (always first)
  PH_NVS,           // NVS/Settings initialization (if enabled)
  PH_WIFI,          // WiFi connection (if enabled, with timeout)
  PH_WEB,           // Web server initialization (if enabled)
  PH_BLE,           // BLE initialization (background, if enabled)
  PH_FANS,          // Fan controller initialization (background, if enabled)
  PH_RUNNING,       // All critical services ready
  PH_DEGRADED       // Degraded mode (some services failed)
};
```

**Note**: `PH_BLE` and `PH_FANS` are not used as separate phases. They initialize in background after `PH_RUNNING`.

---

## Detailed Phase Flow

### PH_CORE: Core Initialization

**Purpose**: Minimal core initialization (Serial already initialized in `setup()`)

**Duration**: < 10ms

**Operations**:
- Boot time tracking (`bootStartTime = millis()`)
- Minimal core state initialization
- Phase transition preparation

**Transition**: Always succeeds → `PH_NVS`

**Code Location**: `App.cpp` → `initCore()`

---

### PH_NVS: Settings Store Initialization

**Purpose**: Initialize NVS and load persistent settings

**Duration**: < 100ms (typical)

**Timeout**: 5 seconds (`HW_BOOT_PHASE_TIMEOUT_MS`)

**Operations**:
- Initialize NVS (if `HW_ENABLE_NVS == 1`)
- Load persistent settings
- Set `nvsReady` flag

**Transition**:
- **Success** → `PH_WIFI`
- **Timeout** → `PH_WIFI` (degraded, continue without NVS)
- **Disabled** → `PH_WIFI` (skip NVS)

**Code Location**: `App.cpp` → `initNVS()`

**Dependencies**: `SettingsStore` (stub implementation)

---

### PH_WIFI: WiFi Connection

**Purpose**: Connect to WiFi network or activate AP fallback

**Duration**: Variable (0-20 seconds)

**Timeout**: 20 seconds (`HW_WIFI_TIMEOUT_MS`)

**Operations**:
- Load WiFi credentials from SettingsStore (or compile-time defaults)
- Start STA connection attempt (non-blocking)
- Monitor connection status in `loop()`
- Activate AP fallback on timeout (if `HW_WIFI_AP_FALLBACK == 1`)

**Transition**:
- **Connected** → `PH_WEB`
- **Timeout + AP active** → `PH_WEB` (degraded mode, AP serves clients)
- **Timeout + no AP** → `PH_WEB` (degraded mode, web server may not work)
- **Disabled** → `PH_WEB` (skip WiFi)

**Code Location**: `App.cpp` → `initWiFi()`, `loopSubsystems()`

**Dependencies**: `WiFiService`, ESP32 WiFi library

---

### PH_WEB: Web Server Initialization

**Purpose**: Initialize web server and register routes

**Duration**: < 500ms

**Timeout**: 5 seconds (`HW_BOOT_PHASE_TIMEOUT_MS`)

**Operations**:
- Initialize `WebServerManager`
- Register static asset routes (HTML, CSS, JS)
- Initialize WebSocket telemetry (if enabled)
- Register API routes (Actions, Settings)
- Start HTTP server on port 80

**Transition**:
- **Success** → `PH_RUNNING`
- **Timeout** → `PH_DEGRADED`
- **Disabled** → `PH_RUNNING` (skip web, go straight to running)

**Code Location**: `App.cpp` → `initWeb()`

**Dependencies**: `WebServerManager`, `ESPAsyncWebServer` (if enabled)

---

### PH_RUNNING: All Critical Services Ready

**Purpose**: Mark system ready and start background services

**Duration**: Indefinite (normal operation)

**Operations**:
- Set `SystemState::setServicesReady(true)`
- APIs return 200 OK (no longer 503)
- Start background services:
  - BLE Relay Manager (if `HW_ENABLE_BLE == 1`)
  - Fan Controller (if `HW_ENABLE_FANS == 1`)
  - Firmware Update Manager (if `HW_ENABLE_OTA == 1`)

**Transition**: None (steady state)

**Code Location**: `App.cpp` → `advanceBootPhase()`, `initBLE()`, `initFans()`, `initOTA()`

---

### PH_DEGRADED: Degraded Mode

**Purpose**: System operational but some services failed

**Duration**: Indefinite (until recovery or restart)

**Operations**:
- Some critical services failed during boot
- Partial functionality available
- APIs may return `503 Service Unavailable`
- Boot orchestrator does not attempt recovery (future enhancement)

**Transition**: Can transition to `PH_RUNNING` if services recover (not currently implemented)

**Code Location**: `App.cpp` → `enterDegradedMode()`

---

## Boot Orchestrator State Machine

```
                    ┌──────────────┐
                    │   begin()    │
                    └──────┬───────┘
                           │
                           ▼
                    ┌──────────────┐
                    │   PH_CORE    │ ◄─── Always first
                    └──────┬───────┘
                           │ < 10ms
                           ▼
              ┌────────────────────────┐
              │       PH_NVS           │
              │  (if enabled)          │
              └──────┬─────────────────┘
                     │ < 100ms (or timeout)
                     ▼
              ┌────────────────────────┐
              │      PH_WIFI           │
              │  (if enabled)          │
              │  • STA connection      │
              │  • AP fallback         │
              └──────┬─────────────────┘
                     │ 0-20s (or timeout)
                     ▼
              ┌────────────────────────┐
              │       PH_WEB           │
              │  (if enabled)          │
              │  • Server init         │
              │  • Route registration  │
              └──────┬─────────────────┘
                     │ < 500ms (or timeout)
                     ▼
         ┌───────────────────────────┐
         │      PH_RUNNING           │
         │  • Services ready         │
         │  • Background init        │
         └───────────────────────────┘
                     │
                     │ (background)
                     ▼
         ┌───────────────────────────┐
         │  BLE / Fans / OTA         │
         │  (if enabled)             │
         └───────────────────────────┘
```

**Error Path** (timeout or failure):
```
PH_* → [timeout] → PH_DEGRADED (or skip to next phase)
```

---

## Readiness Gates

### Critical Services (must be ready for PH_RUNNING)

**NVS** (if `HW_ENABLE_NVS == 1`):
- Must be ready OR disabled
- Failure → Degraded mode (settings not available)

**WiFi** (if `HW_ENABLE_WIFI == 1`):
- Must be ready (STA connected OR AP active) OR disabled
- Failure → Degraded mode (web server may not work)

**Web** (if `HW_ENABLE_WEB == 1`):
- Must be ready OR disabled
- Failure → `PH_DEGRADED` (web UI unavailable)

### Background Services (start after PH_RUNNING)

These services start after `PH_RUNNING` is reached and do not block boot:

- **BLE**: `initBLE()` - BLE relay manager
- **Fans**: `initFans()` - Fan controller (SmartMiFanAsync)
- **OTA**: `initOTA()` - Firmware update manager

### Readiness Check Logic

```cpp
bool SystemState::isReady() {
  if (!servicesReady) {
    return false;  // Boot orchestrator hasn't reached PH_RUNNING
  }
  
  #if HW_ENABLE_NVS
    if (!nvsReady) {
      return false;  // NVS required but not ready
    }
  #endif

  #if HW_ENABLE_WIFI
    if (!wifiReady) {
      return false;  // WiFi required but not ready
    }
  #endif

  #if HW_ENABLE_WEB
    if (!webReady) {
      return false;  // Web server required but not ready
    }
  #endif
  
  return true;  // All critical services ready
}
```

---

## API Behavior During Boot

### Before PH_RUNNING

- All API endpoints return `503 Service Unavailable`
- Response body: `"System not ready"`
- WebSocket may accept connections but won't send snapshots
- Static assets may be served (if web server initialized)

### After PH_RUNNING

- All API endpoints return `200 OK` (or appropriate error codes)
- WebSocket sends full snapshot on client connect
- Background services become available

---

## Timeout Behavior

### WiFi Timeout (20 seconds)

If WiFi fails to connect within 20 seconds:

1. Check if AP fallback is enabled (`HW_WIFI_AP_FALLBACK == 1`)
2. If enabled, start AP mode
3. Mark WiFi as ready (AP active)
4. Continue to `PH_WEB`

If AP fallback disabled:
- Mark WiFi as not ready
- Continue to `PH_WEB` (degraded mode)
- Web server may not be accessible

### Phase Timeout (5 seconds)

If a phase doesn't complete within 5 seconds (`HW_BOOT_PHASE_TIMEOUT_MS`):

- Phase is marked as failed
- Boot continues to next phase (or degraded mode)
- Failed services are skipped but don't block boot

**Exception**: WiFi timeout uses its own 20-second timeout (`HW_WIFI_TIMEOUT_MS`).

---

## Non-Blocking Design

### No `delay()` Calls

The boot process uses **no blocking delays**. All operations are:

- **Immediate**: Core initialization, object creation
- **Non-blocking**: WiFi connection attempts (`WiFi.begin()` returns immediately)
- **Polling**: Connection status checked in `loop()`

**Exception**: Only acceptable `delay()` is in error loop (`while(1) delay(1000)`).

### Loop-Based Polling

Connection status and phase readiness are checked in `App::loop()`:

```cpp
void App::loop() {
  // Update system state
  SystemState::update();
  
  // Advance boot phase if not yet running
  if (currentPhase != BootPhase::PH_RUNNING && 
      currentPhase != BootPhase::PH_DEGRADED) {
    advanceBootPhase();
  }
  
  // Loop subsystems (safe to call even if not fully initialized)
  loopSubsystems();
}
```

### Subsystem Loop

Subsystems update state in their `loop()` methods:

```cpp
void App::loopSubsystems() {
  #if HW_ENABLE_WIFI
    if (wifi) {
      wifi->loop();  // Check connection status
      bool wasReady = wifiReady;
      wifiReady = wifi->ready();
      // Update SystemState if changed
    }
  #endif
  
  // ... other subsystems
}
```

---

## Boot Time Estimates

| Phase | Duration | Notes |
|-------|----------|-------|
| PH_CORE | < 10ms | Immediate |
| PH_NVS | < 100ms | Typical, up to 5s timeout |
| PH_WIFI | 0-20s | Variable (connection time), 20s timeout |
| PH_WEB | < 500ms | Typical, up to 5s timeout |
| PH_RUNNING | - | Steady state |
| **Total (best case)** | **~600ms** | All services connect immediately |
| **Total (WiFi timeout)** | **~25s** | WiFi timeout with AP fallback |

---

## Recovery

### Current Behavior

The boot orchestrator **does not implement runtime recovery** from degraded mode. If a service fails during boot, it remains failed until system restart.

### Future Enhancements

Possible recovery mechanisms:
- WiFi reconnection attempts in background
- Web server restart on failure
- Background service restart on failure
- Automatic retry with exponential backoff

---

## Boot Sequence Example

### Successful Boot (All Features Enabled)

```
[App] Boot orchestrator started (Homewind v0.1.0)
[App] → PH_NVS
[App] Initializing NVS...
[App] NVS ready
[App] → PH_WIFI
[App] Initializing WiFi...
[App] WiFi ready (STA): 192.168.1.100
[App] → PH_WEB
[App] Initializing Web server...
[App] Web server ready
[App] → PH_RUNNING
[App] Initializing BLE (background)...
[App] Initializing Fan controller (background)...
[App] Initializing FirmwareUpdateManager...
[App] FirmwareUpdateManager initialized
[INFO] Homewind initialized successfully
```

**Total Time**: ~600ms (if WiFi connects immediately)

### Degraded Boot (WiFi Timeout)

```
[App] Boot orchestrator started (Homewind v0.1.0)
[App] → PH_NVS
[App] NVS ready
[App] → PH_WIFI
[App] Initializing WiFi...
[App] WiFi timeout, entering degraded mode
[App] → PH_WEB
[App] Initializing Web server...
[App] Web server ready
[App] → PH_RUNNING
[INFO] Homewind initialized successfully
```

**Total Time**: ~25s (WiFi timeout)

---

## Related Documentation

- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - System architecture
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component details
- `BOOT_FLOW.md` (legacy) - Original boot flow diagram

