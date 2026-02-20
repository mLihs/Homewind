# Homewind Documentation Index

**Version**: 1.4.51  
**Platform**: ESP32 (Arduino Core)  
**Last Updated**: 2026-02-20

---

## Overview

This documentation provides comprehensive information about the Homewind ESP32 WebUI library, covering architecture, implementation details, APIs, state machines, and development guidelines. The documentation is structured for both human developers and AI agents.

---

## Documentation Structure

### 📋 Quick Start
- **[01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md)** – What is Homewind? Purpose, features, and use cases

### 🏗️ Architecture & Design
- **[02_ARCHITECTURE.md](./02_ARCHITECTURE.md)** – System architecture, module boundaries, design principles
- **[03_BOOT_FLOW.md](./03_BOOT_FLOW.md)** – Boot orchestrator, deterministic phases, initialization sequence
- **[06_STATE_MACHINES.md](./06_STATE_MACHINES.md)** – All state machines (boot, firmware update, etc.)

### 🔧 Implementation Details
- **[04_COMPONENTS.md](./04_COMPONENTS.md)** – Detailed documentation of all components and classes
- **[05_APIS.md](./05_APIS.md)** – HTTP API endpoints and WebSocket binary protocol
- **[UI_TO_BACKEND_FLOW.md](./UI_TO_BACKEND_FLOW.md)** – UI ↔ Backend data flow (sensors, heartrate, fan count, fan state)
- **[DISPLAY_INTEGRATION_CONCEPT.md](./DISPLAY_INTEGRATION_CONCEPT.md)** – HomeWindWSAmoled display integration concept
- **[DISPLAY_UPDATE_BUG_ANALYSIS.md](./DISPLAY_UPDATE_BUG_ANALYSIS.md)** – LVGL mutex race condition analysis (root cause)
- **[DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md](./DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md)** – Dirty-flag implementation review
- **[BOOT_AND_AP_SCREEN_IMPLEMENTATION_PLAN.md](./BOOT_AND_AP_SCREEN_IMPLEMENTATION_PLAN.md)** – Boot-Screen und AP/Captive-Portal-Screen (✅ implementiert v1.4.51)

### 👨‍💻 Development
- **[07_DEVELOPMENT.md](./07_DEVELOPMENT.md)** – Build instructions, configuration, extending the library

### 📝 Status & Future
- **[08_OPEN_TOPICS.md](./08_OPEN_TOPICS.md)** – TODOs, known issues, planned features, open questions

### 🔧 Troubleshooting
- **[BOOT_LOOP_8MB_FLASH.md](./BOOT_LOOP_8MB_FLASH.md)** – Boot loop on 8MB flash boards (XIAO ESP32-S3)
- **[PSRAM_IMPLEMENTATION_GUIDE.md](./PSRAM_IMPLEMENTATION_GUIDE.md)** – PSRAM usage and implementation guide

---

## Quick Navigation by Topic

### For New Developers
1. Start with [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) to understand what Homewind does
2. Read [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) for system design
3. Review [03_BOOT_FLOW.md](./03_BOOT_FLOW.md) to understand initialization
4. Check [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) for setup instructions

### For AI Agents
- **Architecture queries**: [02_ARCHITECTURE.md](./02_ARCHITECTURE.md), [04_COMPONENTS.md](./04_COMPONENTS.md)
- **State machine queries**: [06_STATE_MACHINES.md](./06_STATE_MACHINES.md), [03_BOOT_FLOW.md](./03_BOOT_FLOW.md)
- **API queries**: [05_APIS.md](./05_APIS.md)
- **Implementation status**: [08_OPEN_TOPICS.md](./08_OPEN_TOPICS.md)
- **Display/LVGL**: [DISPLAY_UPDATE_BUG_ANALYSIS.md](./DISPLAY_UPDATE_BUG_ANALYSIS.md), [DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md](./DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md), [BOOT_AND_AP_SCREEN_IMPLEMENTATION_PLAN.md](./BOOT_AND_AP_SCREEN_IMPLEMENTATION_PLAN.md)

### For Contributors
- **Adding features**: [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) → "Extending the Library"
- **Understanding existing code**: [04_COMPONENTS.md](./04_COMPONENTS.md)
- **API changes**: [05_APIS.md](./05_APIS.md)
- **Current work**: [08_OPEN_TOPICS.md](./08_OPEN_TOPICS.md)

---

## Key Concepts

### Boot Orchestrator
Deterministic, non-blocking boot sequence with phases: `PH_CORE → PH_NVS → PH_WIFI → PH_WEB → PH_RUNNING`

See: [03_BOOT_FLOW.md](./03_BOOT_FLOW.md)

### Feature Gates
Compile-time flags that enable/disable optional subsystems without runtime overhead.

See: [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) → "Feature Gates", [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) → "Configuration"

### State Management
Global state tracking via `SystemState` namespace with per-module readiness flags.

See: [04_COMPONENTS.md](./04_COMPONENTS.md) → "SystemState"

### WebSocket Telemetry
Binary protocol for server→client telemetry with snapshot/delta frames and dirty-flag model.

See: [05_APIS.md](./05_APIS.md) → "WebSocket Protocol", [../src/web/WEBSOCKET_PROTOCOL.md](../src/web/WEBSOCKET_PROTOCOL.md)

### Firmware Updates
OTA updates via GitHub with state machine: `idle → searching → downloading → installing`

See: [06_STATE_MACHINES.md](./06_STATE_MACHINES.md) → "Firmware Update State Machine", [04_COMPONENTS.md](./04_COMPONENTS.md) → "FirmwareUpdateManager"

### Display / LVGL Integration
Deferred dirty-flag pattern for safe cross-task display updates. All LVGL calls centralized in `DisplayManager::loop()` under `lcd_lvgl_lock()`.

See: [DISPLAY_UPDATE_BUG_ANALYSIS.md](./DISPLAY_UPDATE_BUG_ANALYSIS.md), [DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md](./DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md)

---

## Related Documentation

### In Repository
- `README.md` – Quick start and installation
- `tools/README.md` – Build tools (update_manifest, build_webui, watch_webui)
- `src/web/ENDPOINT.md` – API endpoint reference (see [05_APIS.md](./05_APIS.md))
- `src/web/WEBSOCKET_PROTOCOL.md` – WebSocket protocol specification (see [05_APIS.md](./05_APIS.md))

### External Libraries
- **BluetoothBikeSensorServer** – BLE sensor relay (✅ integrated via BLERelayManager)
- **SmartMiFanAsync** – Fan control (✅ integrated via FanController)
- **GitFirmwareUpdate** – OTA update engine (✅ integrated via FirmwareUpdateManager)
- **HomeWindWSAmoled** – AMOLED display (✅ integrated via DisplayManager)
- **ESPAsyncWebServer** – Web server foundation
- **ArduinoJson** – JSON parsing

---

## Documentation Conventions

### Code Examples
- C++ code uses `cpp` language identifier
- JavaScript code uses `js` language identifier
- Configuration examples show actual values

### Status Indicators
- ✅ **Implemented** – Feature is complete and functional
- 🚧 **In Progress** – Feature is partially implemented
- 📋 **Planned** – Feature is planned but not started
- ⚠️ **Stub/Placeholder** – Interface exists but implementation is missing

### File References
- Relative paths from repository root: `src/app/App.h`
- Absolute paths for clarity: `/src/app/App.h` (when needed)

---

**Last Documentation Review**: 2026-02-20
