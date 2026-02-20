# State Machines

This document describes all state machines used in Homewind.

---

## Boot Orchestrator State Machine

**File**: `src/app/App.h`, `src/app/App.cpp`

**Purpose**: Deterministic boot sequence with phases and timeouts.

**States** (BootPhase enum):
- `PH_CORE` (0): Core initialization
- `PH_NVS`: NVS/Settings initialization
- `PH_WIFI`: WiFi connection
- `PH_WEB`: Web server initialization
- `PH_BLE`: BLE initialization (background, not used as phase)
- `PH_FANS`: Fan controller initialization (background, not used as phase)
- `PH_RUNNING`: All critical services ready
- `PH_DEGRADED`: Degraded mode (some services failed)

### State Transition Diagram

```
         ┌──────────────┐
         │   begin()    │
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │   PH_CORE    │ ◄─── Always first (< 10ms)
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │   PH_NVS     │ ◄─── < 100ms (or timeout)
         │  (if enabled)│
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │   PH_WIFI    │ ◄─── 0-20s (or timeout)
         │  (if enabled)│
         └──────┬───────┘
                │
                ▼
         ┌──────────────┐
         │   PH_WEB     │ ◄─── < 500ms (or timeout)
         │  (if enabled)│
         └──────┬───────┘
                │
                ▼
    ┌───────────────────────────┐
    │      PH_RUNNING           │
    │  • Services ready         │
    │  • Background init        │
    └───────────────────────────┘
                │
                │ (background services)
                ▼
    ┌───────────────────────────┐
    │  BLE / Fans / OTA         │
    │  (if enabled)             │
    └───────────────────────────┘
```

**Error Path**:
```
PH_* → [timeout/failure] → PH_DEGRADED (or skip to next phase)
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `PH_CORE` | `PH_NVS` | Always (immediate) |
| `PH_NVS` | `PH_WIFI` | NVS ready OR timeout OR disabled |
| `PH_WIFI` | `PH_WEB` | WiFi ready (STA connected OR AP active) OR timeout |
| `PH_WEB` | `PH_RUNNING` | Web ready OR disabled |
| `PH_WEB` | `PH_DEGRADED` | Web timeout (if enabled and failed) |
| `PH_RUNNING` | - | None (steady state) |

**Detailed Documentation**: See [03_BOOT_FLOW.md](./03_BOOT_FLOW.md)

---

## Firmware Update State Machine

**File**: `src/core/FirmwareUpdateManager.h`, `src/core/FirmwareUpdateManager.cpp`

**Purpose**: Manages OTA firmware updates via GitHub with async operations.

**States** (UpdatePhase enum):
- `PHASE_IDLE`: No update operation in progress
- `PHASE_SEARCHING`: Checking GitHub for latest release
- `PHASE_READY_TO_UPDATE`: Update available, ready to download
- `PHASE_DOWNLOADING`: Downloading firmware from GitHub
- `PHASE_INSTALLING`: Installing firmware and rebooting
- `PHASE_ERROR`: Error occurred

### State Transition Diagram

```
         ┌──────────────┐
         │    IDLE      │
         └──────┬───────┘
                │
                │ requestSearch()
                ▼
         ┌──────────────┐
         │  SEARCHING   │
         └──────┬───────┘
                │
                │ (search complete)
                ▼
         ┌──────────────┐
         │ READY_TO_    │
         │   UPDATE     │
         └──────┬───────┘
                │
                │ requestDownload()
                ▼
         ┌──────────────┐
         │ DOWNLOADING  │
         │ (progress)   │
         └──────┬───────┘
                │
                │ (download complete)
                ▼
         ┌──────────────┐
         │  INSTALLING  │
         │  (reboot)    │
         └──────────────┘
```

**Error Path**:
```
PHASE_* → [error] → PHASE_ERROR
PHASE_ERROR → [abort/reset] → PHASE_IDLE
```

### State Transition Table

| From | To | Trigger | Conditions |
|------|----|---------|-----------|
| `IDLE` | `SEARCHING` | `requestSearch()` | Not busy, job queue has space |
| `SEARCHING` | `READY_TO_UPDATE` | Search complete | Update available |
| `SEARCHING` | `IDLE` | Search complete | No update available (same version) |
| `SEARCHING` | `ERROR` | Search failed | Network error, parse error, etc. |
| `READY_TO_UPDATE` | `DOWNLOADING` | `requestDownload()` | Not busy, job queue has space |
| `DOWNLOADING` | `READY_TO_UPDATE` | Download complete | Download successful |
| `DOWNLOADING` | `ERROR` | Download failed | Network error, insufficient space, etc. |
| `READY_TO_UPDATE` | `INSTALLING` | `startInstall()` | Firmware downloaded |
| `INSTALLING` | - | Install complete | System reboots (no return) |
| `*` | `ERROR` | Operation failed | Any error condition |
| `*` | `IDLE` | `abort()` | Abort current operation |

### Async Operations

**Worker Task**: Async operations (search, download) run in separate FreeRTOS task:
- Task name: `"fw_upd"`
- Stack size: 12KB
- Priority: `tskIDLE_PRIORITY + 3`
- Core: 1 (pinned)

**Job Queue**: Jobs enqueued from main thread, processed in worker task:
- Queue size: 2 (one pending + one in progress)
- Job types: `JOB_SEARCH`, `JOB_DOWNLOAD`

**Thread Safety**: State access protected by mutex (`_stateMutex`).

### Progress Tracking

**Progress Callback**: `GitFirmwareUpdate` calls `onProgress(percent, bytesRead, totalBytes)`:
- Updates `_lastProgress` (0-100)
- Marks `FIRMWARE_PROGRESS` frame dirty for WebSocket

**Download Completion**: Sync download endpoint uses semaphore (`_dlDoneSem`):
- Main thread waits on semaphore
- Worker task signals semaphore on completion

### JavaScript Bridge

**Callback Injection**: `injectJavaScriptBridge()` injects JavaScript functions into HTML:
- `window.firmwareSearchResult(json)` - Called when search completes
- `window.firmwareProgress(percent)` - Called during download
- `window.firmwareError(message)` - Called on error

**Integration**: Callbacks called from worker task, update Web UI directly.

---

## WiFi Connection State Machine

**File**: `src/wifi/WiFiService.h`, `src/wifi/WiFiService.cpp`

**Purpose**: Manages WiFi connection with STA mode and AP fallback.

**States** (implicit, via WiFi library):
- `DISCONNECTED`: Not connected
- `CONNECTING`: Connection attempt in progress
- `CONNECTED`: STA connected
- `AP_ACTIVE`: AP mode active (fallback)

### State Transition Diagram

```
         ┌──────────────┐
         │  DISCONNECTED│
         └──────┬───────┘
                │
                │ begin() (STA attempt)
                ▼
         ┌──────────────┐
         │  CONNECTING  │
         └──────┬───────┘
                │
                │ (connection success)
                ▼
         ┌──────────────┐
         │  CONNECTED   │
         └──────────────┘
                │
                │ (timeout OR connection failure)
                │ (if AP fallback enabled)
                ▼
         ┌──────────────┐
         │  AP_ACTIVE   │
         └──────────────┘
```

### Transition Conditions

| From | To | Condition |
|------|----|-----------|
| `DISCONNECTED` | `CONNECTING` | `begin()` called (STA attempt) |
| `CONNECTING` | `CONNECTED` | `WiFi.status() == WL_CONNECTED` |
| `CONNECTING` | `AP_ACTIVE` | Timeout (20s) OR connection failed AND `HW_WIFI_AP_FALLBACK == 1` |
| `CONNECTED` | `DISCONNECTED` | `WiFi.status() != WL_CONNECTED` (reconnection) |
| `AP_ACTIVE` | `CONNECTED` | STA connection restored (if reconnect succeeds) |

**Timeout**: 20 seconds (`HW_WIFI_TIMEOUT_MS`)

**AP Fallback**: Enabled by default (`HW_WIFI_AP_FALLBACK == 1`)

---

## WebSocket Telemetry Dirty-Flag Model

**File**: `src/web/WebSocketTelemetry.h`, `src/web/WebSocketTelemetry.cpp`

**Purpose**: Efficient telemetry updates with dirty-flag tracking.

**State**: Dirty flags (bitfield of frame types that need to be sent)

### Dirty-Flag State

**Dirty Flags**: `uint16_t dirtyFlags` (bitfield)

Each frame type has a bit:
- `SYSTEM_STATUS` (0x01) → bit 0
- `SENSORS_SNAPSHOT` (0x02) → bit 1
- `SENSOR_CONN_STATE` (0x03) → bit 2
- etc.

### State Transition

```
         ┌──────────────┐
         │   CLEAN      │ (no dirty flags)
         └──────┬───────┘
                │
                │ markDirty(frameType)
                ▼
         ┌──────────────┐
         │    DIRTY     │ (flag set)
         └──────┬───────┘
                │
                │ loopFlush()
                ▼
         ┌──────────────┐
         │   SENT       │ (flag cleared)
         └──────┬───────┘
                │
                │ markDirty() again
                ▼
         ┌──────────────┐
         │    DIRTY     │
         └──────────────┘
```

**Operation**:
1. **Mark Dirty**: Async contexts call `markDirty(frameType)` → sets bit
2. **Loop Flush**: Main loop calls `loopFlush()` → sends dirty frames → clears bits
3. **Periodic Refresh**: Snapshot frames marked dirty every 30 seconds

**Thread Safety**: Dirty flags set from async contexts (BLE callbacks, etc.), cleared from main loop. No mutex needed (single-writer from main loop).

---

## Maintenance Mode State Machine

**File**: `src/core/MaintenanceMode.h`, `src/core/MaintenanceMode.cpp`

**Purpose**: Blocks non-whitelisted API endpoints during critical operations.

**States**:
- `INACTIVE`: Maintenance mode not active
- `ACTIVE`: Maintenance mode active

**Reasons**:
- `OTA`: Firmware update in progress
- `FACTORY_RESET`: Factory reset in progress
- `UNKNOWN`: Unknown reason

### State Transition Diagram

```
         ┌──────────────┐
         │   INACTIVE   │
         └──────┬───────┘
                │
                │ begin(reason)
                ▼
         ┌──────────────┐
         │    ACTIVE    │
         │  (blocks API)│
         └──────┬───────┘
                │
                │ end()
                ▼
         ┌──────────────┐
         │   INACTIVE   │
         └──────────────┘
```

### Transition Conditions

| From | To | Trigger |
|------|----|---------|
| `INACTIVE` | `ACTIVE` | `begin(OTA)` or `begin(FACTORY_RESET)` |
| `ACTIVE` | `INACTIVE` | `end()` (or reboot) |

**Behavior**:
- `isActive() == true` → API endpoints return `503 Service Unavailable`
- Whitelisted endpoints (status, firmware status) may still work
- Stops background services (fans, BLE) for maintenance

**Idempotent**: Safe to call `begin()` multiple times (early return if already active).

---

## State Machine Best Practices

### 1. Deterministic Transitions
- All transitions based on explicit events or conditions
- No random behavior or undefined states

### 2. Timeout Handling
- All async operations have timeouts
- Timeout triggers error state or fallback

### 3. Thread Safety
- State access protected by mutexes when needed
- Dirty-flag pattern for async updates (no mutex needed)

### 4. Error Recovery
- All state machines have error states
- `abort()` or reset transitions to idle/clean state

### 5. State Visibility
- State queries available via public API
- State changes logged for debugging

---

## Related Documentation

- [03_BOOT_FLOW.md](./03_BOOT_FLOW.md) - Boot orchestrator details
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation
- [05_APIS.md](./05_APIS.md) - API documentation

