# Homewind Project Overview

## What is Homewind?

**Homewind** is an ESP32-based WebUI library designed for fitness/cycling applications. It provides a modular, feature-gated architecture for managing BLE sensors (heart rate, cadence), smart fans, and firmware updates through a web-based interface.

---

## Purpose

Homewind serves as the control and monitoring system for:
- **BLE Sensor Relay**: Acts as a Bluetooth Low Energy relay for fitness sensors (HR monitors, cadence sensors)
- **Smart Fan Control**: Manages Xiaomi Smart Mi Fan devices via BLE
- **Telemetry**: Real-time sensor data streaming to web clients via WebSocket
- **Firmware Updates**: Over-the-air (OTA) updates from GitHub releases
- **Web Interface**: Modern, responsive web UI for configuration and monitoring

---

## Target Use Cases

### Primary Use Case: PulseFanSync
Homewind powers the **PulseFanSync** system, which:
1. Connects to BLE fitness sensors (heart rate, cadence)
2. Relays sensor data via BLE to fitness apps/devices (as a Bluetooth relay)
3. Controls smart fans based on sensor data (heart rate-based fan speed)
4. Provides web UI for configuration and monitoring
5. Supports firmware updates without physical access

### Secondary Use Cases
- Standalone BLE sensor relay for fitness applications
- Smart fan controller with web UI
- Generic ESP32 WebUI framework (modular architecture)

---

## Key Features

### ✅ Implemented Features

1. **Modular Architecture**
   - Feature gates for optional subsystems
   - Compile-time configuration
   - No runtime overhead for disabled features

2. **Boot Orchestrator**
   - Deterministic, non-blocking boot sequence
   - Phase-based initialization with timeouts
   - Degraded mode support

3. **Web Server**
   - Embedded HTML/CSS/JS assets (PROGMEM)
   - Automatic cache-busting via build hash
   - ETag support for bandwidth optimization
   - GZIP compression

4. **WebSocket Telemetry**
   - Binary protocol (low overhead)
   - Snapshot/delta frame model
   - Reconnect-safe (full snapshot on connect)
   - Dirty-flag + loop-flush architecture

5. **Firmware Updates (OTA)**
   - GitHub-based releases
   - Async download with progress tracking
   - JavaScript callback injection for UI updates
   - Maintenance mode during updates

6. **WiFi Management** (Dual Backend)
   - **Development Mode** (`HW_DEV_MODE=1`): Fixed credentials via `WiFiService`
   - **Production Mode** (`HW_DEV_MODE=0`): Captive Portal via `wifiMangerLite`
   - Two-level connectivity: Setup-ready (AP/STA) vs Operational (STA only)
   - Smart root dispatch: Portal in AP mode, Homewind UI in STA mode
   - Non-blocking connection handling

7. **Settings Storage**
   - NVS-based persistence (stub implementation)
   - HTTP API for settings management

8. **Serial Debug Commands** (via `HW_ENABLE_SERIAL_COMMANDS`)
   - `h` - Heap info (Initial, Free, Used since boot, Largest block, Fragmentation%)
   - `s` - System status (Version, Uptime, Boot phase, WiFi state)
   - `?` - Help
   - Boot heap tracking for memory leak detection

### ✅ Implemented (BLE Sensor Relay)

1. **BLE Relay Manager** (fully implemented)
   - Integrated with BluetoothBikeSensorServer library
   - Sensor management (add, remove, connect, disconnect)
   - Sensor discovery (HR, CSC sensors)
   - Persistent configuration via NVSConfigBus
   - WebSocket telemetry integration
   - HTTP API endpoints for sensor control

2. **Sensor Web UI** (fully implemented)
   - Discovery UI with modal flow
   - Sensor configuration (add/remove sensors)
   - Real-time status display (connection state, battery level)
   - REST API integration for actions
   - WebSocket integration for status updates

9. **AMOLED Display** (optional, `HW_ENABLE_DISPLAY`)
   - HomeWindWSAmoled integration for HR, CSC, Fans, QR URL
   - Dirty-flag deferred updates (LVGL mutex safe)
   - Touch support, powersave modes

### 🚧 Partially Implemented

1. **Settings Store** (stub implementation)
   - Interface complete
   - NVS implementation pending

### 📋 Planned Features

- Advanced telemetry (connection quality, RSSI)
- Configuration backup/restore
- Multi-language support

---

## Hardware Requirements

### Minimum Requirements
- **Microcontroller**: ESP32 (any variant: ESP32, ESP32-S2, ESP32-S3, ESP32-C3)
- **Flash**: 4MB minimum (for firmware + web assets)
- **RAM**: 200KB free heap (typical)
- **WiFi**: Built-in (STA/AP mode)

### Optional Hardware
- **BLE**: For sensor relay (ESP32 native)
- **Serial**: For debugging (USB-to-Serial)
- **AMOLED + Touch**: Waveshare ESP32-S3-Touch-AMOLED-1.64 for display (HR, CSC, Fans, QR)

---

## Software Dependencies

### ESP32/Arduino Core (Built-in)
| Library | Purpose |
|---------|---------|
| `Arduino.h` | Base framework |
| `WiFi.h` | WiFi STA/AP mode |
| `WiFiUdp.h` | UDP communication (fan control) |
| `Preferences.h` | NVS persistent storage |
| `ESP.h` | System info (heap, restart) |

### External Libraries
| Library | Version | Purpose | Feature Gate |
|---------|---------|---------|--------------|
| **ESPAsyncWebServer** | ≥1.2.3 | Async HTTP server | `HW_ENABLE_WEB` |
| **AsyncTCP** | ≥1.1.1 | TCP for ESPAsyncWebServer | `HW_ENABLE_WEB` |

### Project Libraries (included in workspace)
| Library | Purpose | Feature Gate |
|---------|---------|--------------|
| **BluetoothBikeSensorServer** | BLE sensor relay (HR, CSC) | `HW_ENABLE_BLE` |
| **SmartMiFanAsync** | Xiaomi Smart Mi Fan control | `HW_ENABLE_FANS` |
| **HomeWindWSAmoled** | AMOLED display (HR, CSC, Fans, QR) | `HW_ENABLE_DISPLAY` |
| **GitFirmwareUpdate** | GitHub-based OTA updates | `HW_ENABLE_OTA` |
| **wifiMangerLite** | WiFi Captive Portal (Production mode) | `HW_WIFI_BACKEND_WML` |

### WiFi Backend Selection (compile-time)

Homewind supports two WiFi backends, selected via `HW_DEV_MODE` in `Config.h`:

| Mode | Flag | Backend | Use Case |
|------|------|---------|----------|
| **Development** | `HW_DEV_MODE=1` | `WiFiService` (fixed credentials) | Fast iteration, known network |
| **Production** | `HW_DEV_MODE=0` | `wifiMangerLite` (Captive Portal) | End-user deployment, no hardcoded creds |

**Development Mode (Default):**
- WiFi credentials defined in `Config.h` (`HW_WIFI_SSID`, `HW_WIFI_PASS`)
- Instant boot to configured network
- AP fallback on connection failure

**Production Mode:**
- First boot: Opens Captive Portal (`/wml/setup`)
- User configures WiFi via smartphone/browser
- Credentials stored in NVS, persisted across reboots
- No secrets in source code

**Two-Level Connectivity:**
- **Level A (Setup ready)**: AP *or* STA connected → WebUI accessible
- **Level B (Operational)**: STA connected only → BLE/Fans/OTA enabled

**Important Technical Note (WML Backend):**
- `WiFi.mode(WIFI_AP_STA)` **must** be called before `ESPAsyncWebServer` starts
- Without this, FreeRTOS semaphore assertions fail at runtime
- Boot order: WiFi hardware init → WebServer creation → WiFiManagerLite/Portal init

### Installation (Arduino IDE)
```
Tools → Manage Libraries → Search:
- ESPAsyncWebServer (by me-no-dev)
- AsyncTCP (by me-no-dev)
```

### Installation (PlatformIO)
```ini
lib_deps =
    me-no-dev/ESP Async WebServer@^1.2.3
    me-no-dev/AsyncTCP@^1.1.1
```

---

## Project Status

### Current State: **Beta**

- **Boot System**: ✅ Fully functional
- **Web Server**: ✅ Fully functional
- **WebSocket Telemetry**: ✅ Fully functional
- **Firmware Updates**: ✅ Fully functional (OTA via GitHub)
- **WiFi Management**: ✅ Dual backend (Dev: fixed creds, Prod: Captive Portal)
- **Settings Storage**: ✅ NVS-based persistence
- **BLE Sensor Relay**: ✅ Fully functional (HR, CSC sensors)
- **Fan Control**: ✅ Fully functional (Xiaomi Smart Mi Fans)

### Compilation Status
- ✅ Compiles successfully (green build)
- ✅ All features disabled by default
- ✅ Minimal dependencies (only ESP32 Arduino Core)

---

## Design Principles

### 1. **Modularity**
- Features are optional and feature-gated
- No dependencies between optional modules
- Each module can be enabled/disabled independently

### 2. **Non-Blocking Design**
- No `delay()` calls in main code paths
- Async operations where possible
- Deterministic boot sequence with timeouts

### 3. **Deterministic Behavior**
- Fixed boot phase order
- Predictable state transitions
- Timeout-based failure handling

### 4. **Resource Efficiency**
- Static buffers where possible
- PROGMEM for web assets (no RAM usage)
- Minimal heap allocation

### 5. **Maintainability**
- Clear module boundaries
- Consistent naming conventions
- Comprehensive documentation

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                    Application Layer                     │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │  BLE Relay   │  │ Fan Control  │  │ Firmware OTA │  │
│  │   Manager    │  │   Manager    │  │   Manager    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                      Core Services                       │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │     App      │  │ System State │  │   Settings   │  │
│  │ Coordinator  │  │  Management  │  │    Store     │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                      Web Layer                           │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ Web Server   │  │ WebSocket    │  │  HTTP API    │  │
│  │              │  │ Telemetry    │  │  (Actions/   │  │
│  │              │  │              │  │  Settings)   │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
└─────────────────────────────────────────────────────────┘
┌─────────────────────────────────────────────────────────┐
│                    Platform Layer                        │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐  │
│  │ WiFiService  │  │WiFiServiceWML│  │    NVS       │  │
│  │ (Dev Mode)   │  │ (Prod Mode)  │  │   Storage    │  │
│  └──────────────┘  └──────────────┘  └──────────────┘  │
│         ↑                 ↑                             │
│    HW_DEV_MODE=1    HW_DEV_MODE=0                       │
└─────────────────────────────────────────────────────────┘
```

See [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) for detailed architecture documentation.

---

## File Structure

```
Homewind/
├── Homewind.ino              # Main sketch (minimal wiring)
├── library.properties        # Arduino library metadata
├── README.md                 # Quick start guide
├── CHANGELOG.md              # Version history
├── docs/                     # Comprehensive documentation (this directory)
├── webui_src/                # Web UI source files (HTML, CSS, JS)
├── tools/                    # Build scripts (web UI builder)
└── src/
    ├── generated/            # Auto-generated files (web assets as headers)
    ├── app/                  # Application coordinator
    │   ├── App.h/cpp         # Boot orchestrator
    │   ├── Config.h          # Feature gates and configuration
    │   └── BuildInfo.h       # Version/build metadata
    ├── core/                 # Core services
    │   ├── SystemState.h/cpp # Global state management
    │   ├── FirmwareUpdateManager.h/cpp # OTA update manager
    │   ├── FanController.h/cpp         # Fan controller (SmartMiFanAsync)
    │   ├── BLERelayManager.h/cpp       # BLE relay (HR, CSC)
    │   ├── MaintenanceMode.h/cpp       # Maintenance mode gate
    │   └── Timing.h          # Non-blocking timing utilities
    ├── web/                  # Web layer
    │   ├── WebServer.h/cpp   # Web server manager
    │   ├── WebSocketTelemetry.h/cpp    # WebSocket telemetry
    │   ├── ApiActions.h/cpp  # HTTP action handlers
    │   └── ApiSettings.h/cpp # HTTP settings handlers
    ├── wifi/                 # WiFi management (dual backend)
    │   ├── WiFiService.h/cpp     # Dev mode: fixed credentials
    │   └── WiFiServiceWML.h/cpp  # Prod mode: wifiMangerLite Captive Portal
    └── settings/             # Settings storage
        └── SettingsStore.h/cpp # Settings store (stub)
```

---

## Version Information

- **Current Version**: 1.4.18
- **Version Source**: `src/app/BuildInfo.h`
- **Build ID**: 20260124-011052
- **Release Status**: Beta

---

## License

[License information to be added]

---

## Contributing

[Contributing guidelines to be added]

---

## Next Steps

1. Read [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) for system design details
2. Review [03_BOOT_FLOW.md](./03_BOOT_FLOW.md) to understand initialization
3. Check [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) for setup instructions
4. Explore [04_COMPONENTS.md](./04_COMPONENTS.md) for implementation details

