# Open Topics & Future Work

This document tracks TODOs, known issues, planned features, and open questions.

---

## Implementation Status

### ✅ Fully Implemented

- **Boot Orchestrator** - Deterministic, non-blocking boot sequence with phases
- **Web Server** - Static asset serving with ETag support and caching
- **WebSocket Telemetry** - Binary protocol with snapshot/delta frames
- **Firmware Updates** - OTA updates via GitHub with state machine
- **WiFi Management** - STA connection with AP fallback
- **Maintenance Mode** - API endpoint gating during critical operations
- **BLE Relay Manager** - BLE sensor relay with BluetoothBikeSensorServer integration (HR, CSC)
- **Fan Controller** - SmartMiFanAsync integration, discovery, NVS config, WebSocket telemetry
- **Display Manager** - HomeWindWSAmoled integration with dirty-flag deferred LVGL updates
- **Sensor Management** - Sensor configuration, discovery, connection management
- **Sensor Web UI** - Complete sensor management UI with REST API and WebSocket integration

### 🚧 Partially Implemented

#### Settings Store (Stub)
- **Status**: Interface complete, NVS implementation pending
- **File**: `src/settings/SettingsStore.h/cpp`
- **Work Needed**: Implement NVS/Preferences backend (TODO in SettingsStore.cpp:62)
- **Priority**: Medium


### 🚧 Partially Implemented (Web UI Complete, Backend Pending)

#### Sensor Management UI
- **Status**: ✅ Fully implemented and integrated
- **File**: `webui_src/app.js` → `BLESensorManager` (lines 3855-4182)
- **Implemented Features**:
  - ✅ Sensor discovery UI with modal flow
  - ✅ Sensor configuration UI (add/remove sensors)
  - ✅ Real-time sensor status display (connection state)
  - ✅ Battery level monitoring and display
  - ✅ Sensor selection UI (discovery results)
  - ✅ REST API integration for actions (setSensor, deleteSensor, connect, disconnect, discoveryStart/Stop)
  - ✅ WebSocket integration for status updates (sensor snapshots, connection state, discovery results)
  - ✅ BLE relay server pause/resume integration

#### Fan Management UI
- **Status**: ✅ Fully implemented and integrated with FanController
- **File**: `webui_src/app.js` → `FanManager`

### 📋 Planned Features

#### Advanced Telemetry
- **Status**: Partial - UI support exists, needs backend data
- **Implemented**: 
  - ✅ Sensor battery level display (UI ready in `BLESensorManager`)
  - ✅ Connection status display (connected/disconnected states)
- **Work Needed**:
  - Connection quality metrics (RSSI, signal strength)
  - Network statistics (WiFi signal quality)
- **Priority**: Low (basic telemetry already works)

#### Configuration Backup/Restore
- **Status**: Partial - Factory reset implemented, backup/restore pending
- **Implemented**:
  - ✅ Factory reset with confirmation modal (UI ready in `SystemManager`, lines 4203-4375)
  - ✅ Reset API endpoint integration (`/api/reset`)
- **Work Needed**:
  - Export settings to JSON
  - Import settings from JSON
- **Priority**: Medium (factory reset works, backup/restore is nice-to-have)

#### Multi-Language Support
- Internationalization (i18n) framework
- Language selection in settings
- Translated web UI

---

## Known Issues

### Boot Sequence

#### No Runtime Recovery from Degraded Mode
- **Issue**: If a service fails during boot, it remains failed until restart
- **Impact**: System must be rebooted to recover from degraded mode
- **Priority**: Low
- **Workaround**: Reboot device

**Future Enhancement**: Implement background recovery attempts (WiFi reconnection, web server restart)

### WiFi Management

#### Compile-Time Credentials Only
- **Issue**: WiFi credentials are compile-time only (no runtime configuration)
- **Impact**: Cannot change WiFi credentials without recompiling
- **Priority**: Medium
- **Workaround**: Use AP fallback mode for configuration

**Future Enhancement**: Runtime WiFi configuration via web UI

#### AP Fallback Always Uses Default Credentials
- **Issue**: AP mode uses compile-time credentials only
- **Impact**: Cannot customize AP credentials without recompiling
- **Priority**: Low
- **Workaround**: Recompile with desired AP credentials

### Settings Store

#### No NVS Implementation
- **Issue**: SettingsStore is a stub with no persistence
- **Impact**: Settings are not saved across reboots
- **Priority**: High
- **Workaround**: None (must implement NVS backend)

### WebSocket Telemetry

#### Frame Builders Are Stubs
- **Issue**: Sensor and fan frame builders return empty data
- **Impact**: WebSocket telemetry incomplete until sensor/fan integration
- **Priority**: Medium
- **Workaround**: System status frames work

**Status**: Waiting for sensor/fan manager integration

**Note**: `FIRMWARE_SEARCH_RESULT` (0x0A) frame is fully implemented with hard limits (version: 32 chars, notes: 160 chars).

### Firmware Updates

#### No Rollback Mechanism
- **Issue**: Failed firmware install may leave device unbootable
- **Impact**: Device may require physical access to recover
- **Priority**: Medium
- **Workaround**: Use stable firmware releases

**Future Enhancement**: Implement dual-bank firmware with rollback

---

## Open Questions

### Architecture

#### Should SettingsStore Support JSON?
- **Question**: Should settings use JSON format for complex values?
- **Current**: String-based key-value pairs only
- **Consideration**: Simpler parsing vs. richer data structures
- **Status**: TBD

#### Should API Endpoints Use JSON?
- **Question**: Should HTTP APIs use JSON instead of text/plain?
- **Current**: Form-urlencoded requests, text/plain responses
- **Consideration**: Simpler parsing vs. standard REST API format
- **Status**: TBD (current approach is intentional for simplicity)

### Feature Scope

#### Should BLE Relay Support Multiple Clients?
- **Question**: Should BLE relay support multiple connected clients simultaneously?
- **Current**: [Unknown - needs BluetoothBikeSensorServer investigation]
- **Consideration**: Performance vs. functionality
- **Status**: TBD

#### Should Fan Control Support Multiple Fans?
- **Question**: How many fans should be supported simultaneously?
- **Current**: [Unknown - needs SmartMiFanAsync investigation]
- **Consideration**: Memory vs. use case
- **Status**: TBD

### Performance

#### Memory Usage Optimization
- **Question**: Can WebSocket frame buffer be reduced?
- **Current**: 1KB static buffer
- **Consideration**: Memory vs. frame size limits
- **Status**: Monitor and optimize as needed

#### SmartMiFanAsync Read-Only Status / Presence Check (Smart Home Friendly)
- **Goal**: Add a **read-only** `getStatus` / presence API in `SmartMiFanAsync` that does **not** perform handshake/commands or change device state.
- **Why**: When Homewind runs without HR (Smart Home mode), we intentionally avoid periodic fan commands to not interfere. A read-only status would enable periodic “fan still there?” checks without side effects.
- **Follow-up**: Once available, Homewind can optionally publish this via WebSocket/UI (e.g., `connected`, `lastSeen`, `online`).

#### Boot Time Optimization
- **Question**: Can boot time be reduced further?
- **Current**: ~600ms (best case), ~25s (WiFi timeout)
- **Consideration**: Feature trade-offs
- **Status**: Monitor and optimize as needed

---

## Technical Debt

### Code Quality

#### ✅ Fixed: Unbounded String Concatenation
- **Status**: ✅ **Resolved** - HTTP endpoints now use fixed-size buffers
- **Solution**: Implemented `TextUtil` helpers (`appendf`, `truncate_ascii`)
- **Impact**: Deterministic memory usage, no heap fragmentation
- **Files**: `src/core/TextUtil.h/cpp`, `src/web/ApiActions.cpp`
- **Limits**: Version (32 chars), build ID (8 chars), release notes (160 chars)

#### Global Pointers for Maintenance Mode
- **Issue**: `g_fanController` and `g_bleRelayManager` global pointers used by MaintenanceMode
- **Impact**: Tight coupling, harder to test
- **Priority**: Low
- **Future**: Refactor to use dependency injection or callback pattern

#### Incomplete Error Handling
- **Issue**: Some error paths don't log or report errors
- **Impact**: Harder to debug issues
- **Priority**: Medium
- **Future**: Add comprehensive error logging

### Documentation

#### Missing API Examples
- **Issue**: API documentation lacks complete examples
- **Impact**: Harder for developers to use APIs
- **Priority**: Low
- **Future**: Add comprehensive API examples

#### Missing Architecture Diagrams
- **Issue**: Architecture documentation uses ASCII art, could use formal diagrams
- **Impact**: Less visual clarity
- **Priority**: Low
- **Future**: Add formal architecture diagrams (PlantUML, Mermaid, etc.)

---

## Planned Enhancements

### Short Term (Next Release)

1. **NVS Implementation** - Complete SettingsStore with NVS backend
2. **Fan Integration** - Integrate SmartMiFanAsync library
3. **Fan Frame Builders** - Implement fan telemetry frame builders
4. **Fan API Handlers** - Implement fan action handlers

### Medium Term (Future Releases)

1. **Runtime WiFi Configuration** - Web UI for WiFi credentials
2. **Configuration Backup/Restore** - Export/import settings
3. **Advanced Telemetry** - Connection quality, battery levels
4. **Multi-Language Support** - i18n framework
5. **Firmware Rollback** - Dual-bank firmware with rollback

### Long Term (Future Releases)

1. **OTA Rollback** - Automatic rollback on boot failure
2. **Advanced Recovery** - Runtime recovery from degraded mode
3. **Performance Monitoring** - Built-in performance metrics
4. **Plugin System** - Support for custom plugins
5. **MQTT Support** - Optional MQTT telemetry backend

---

## Testing Needs

### Unit Testing

- [ ] Boot orchestrator state machine
- [ ] Firmware update state machine
- [ ] WiFi connection state machine
- [ ] Settings store persistence
- [ ] API endpoint handlers
- [ ] WebSocket frame encoding/decoding

### Integration Testing

- [ ] Full boot sequence (all features enabled)
- [ ] WiFi timeout with AP fallback
- [ ] Firmware update flow (search → download → install)
- [ ] WebSocket reconnection
- [ ] Maintenance mode blocking
- [ ] Factory reset flow

### Hardware Testing

- [ ] BLE sensor connection/disconnection
- [ ] Fan discovery and control
- [ ] Firmware update on actual device
- [ ] Long-term stability (24+ hours)
- [ ] Memory leak detection
- [ ] WiFi reconnection after disconnection

---

## Contribution Opportunities

### High Priority

1. **NVS Settings Store Implementation**
   - File: `src/settings/SettingsStore.cpp`
   - Skills: ESP32 NVS/Preferences API
   - Estimated: 1-2 days


3. **Fan Integration**
   - Files: `src/core/FanController.cpp`, integration with App
   - Skills: BLE, SmartMiFanAsync library
   - Estimated: 3-5 days

### Medium Priority

1. **WebSocket Frame Builders**
   - Files: `src/web/WebSocketTelemetry.cpp`
   - Skills: Binary protocol, sensor/fan state access
   - Estimated: 2-3 days

2. **Runtime WiFi Configuration**
   - Files: `src/wifi/WiFiService.cpp`, web UI
   - Skills: WiFi management, web UI development
   - Estimated: 3-4 days

3. **Configuration Backup/Restore**
   - Files: `src/web/ApiActions.cpp`, web UI
   - Skills: JSON parsing, file download/upload
   - Estimated: 2-3 days

### Low Priority

1. **Unit Tests**
   - New files: `tests/` directory
   - Skills: Unit testing framework (ArduinoUnit, etc.)
   - Estimated: Ongoing

2. **Documentation Improvements**
   - Files: `docs/` directory
   - Skills: Technical writing
   - Estimated: Ongoing

---

## Questions for Maintainers

1. **Versioning Strategy**: Should version follow semantic versioning strictly?
2. **Release Cadence**: How often should releases be made?
3. **Backward Compatibility**: How many versions should be supported?
4. **Feature Requests**: What is the process for feature requests?
5. **Pull Requests**: What is the review process?

---

## Related Documentation

- [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) - Project status
- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - Architecture details
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component status
- [07_DEVELOPMENT.md](./07_DEVELOPMENT.md) - Development guide

---

**Last Updated**: 2025-01-12

**Recent Updates**:
- ✅ **2025-01-12**: BLE Relay Manager fully implemented with BluetoothBikeSensorServer integration
- ✅ **2025-01-12**: Sensor management API endpoints implemented (set, delete, connect, disconnect, reload, discovery)
- ✅ **2025-01-12**: WebSocket telemetry for sensors implemented (snapshots, connection state, discovery results)
- ✅ **2025-01-12**: Sensor Web UI updated to use REST API and WebSocket (no JSON parsing)
- ✅ **2025-01-12**: NVSConfigBus integration for persistent sensor configuration
- ✅ **2025-01-11**: Fixed unbounded String concatenation in HTTP handlers (TextUtil implementation)
- ✅ **2025-01-11**: Added FIRMWARE_SEARCH_RESULT WebSocket frame (0x0A) with hard limits
- ✅ **2025-01-11**: Removed JSON from HTTP firmware status endpoint (now uses WebSocket binary protocol)

