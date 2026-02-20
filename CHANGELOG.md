# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [1.4.51] - 2026-02-20

### Added – Boot and AP Screen Integration

- **DisplayManager:** Boot screen ("Warming Up") shown from display init until PH_RUNNING; AP screen ("Wifisetup Mode") when WiFi captive portal active. `showAPScreen()` / `showMainScreen()` switch between screens.
- **App:** PH_RUNNING display logic: AP mode → AP screen, STA mode → main screen. AP↔STA transitions in `loopSubsystems()` update display via DisplayManager.
- **HomeWindWSAmoled 1.5.9:** Boot/AP screens, `powersave_lock`/`powersave_unlock`, `homewind_show_ap_screen`/`homewind_show_main_screen`.

### Changed

- **00_INDEX.md:** Version 1.4.51, Last Updated 2026-02-20; BOOT_AND_AP_SCREEN_IMPLEMENTATION_PLAN marked as implemented.

- **Version:** 1.4.51, **Build ID:** `20260220-120000`
- **Zip release:** `Homewind-1.4.51.zip`

---

## [1.4.50] - 2026-02-13

### Fixed – Powersave screen after sensor disconnect

- **DisplayManager:** Clear `_lastHR` and `_lastCadence` when HR/CSC sensor disconnects. Fixes Powersave screen retaining last value and active state after disconnect; now correctly shows `"--"` and disconnected icon.
- **docs/POWERSAVE_SENSOR_DISCONNECT_ANALYSIS.md:** Root-cause analysis and solution documentation.

- **Version:** 1.4.50, **Build ID:** `20260213-120000`
- **Zip release:** `Homewind-1.4.50.zip`

---

## [1.4.49] - 2026-02-19

### Fixed – Display update race condition (LVGL mutex)

- **DisplayManager:** Implemented dirty-flag deferred pattern for safe cross-task LVGL updates. All `homewind_set_*` calls centralized in `loop()` under `lcd_lvgl_lock()`. BLE/Fan callbacks only set flags and cache values. Fixes intermittent stuck HR/CSC/Fan values.
- **DisplayManager:** Dirty flags changed from `volatile bool` to `std::atomic<bool>` with `exchange(false)` to eliminate theoretical lost-update race (per [DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md](docs/DISPLAY_UPDATE_IMPLEMENTATION_REVIEW.md)).
- **FanController:** `markTelemetryDirty()` now calls `markFansDirty()` instead of `updateFans()` to avoid deadlock on Fan-Touch path.
- **App:** `g_displayManager->loop()` added to `loopSubsystems()` for centralized display flush.

### Changed – Documentation cleanup and updates

- **docs:** Removed 27 obsolete analysis/debug docs (BLOCKERMIX*, BLE_FRAGMENTATION*, BOOT_*, HEAP_*, WEBSOCKET_*, PSRAM_KONZEPTE*, etc.).
- **00_INDEX.md:** Updated to v1.4.49, added Display/LVGL section, fixed external libs (SmartMiFanAsync ✅), added tools/README, DISPLAY_UPDATE_* docs.
- **04_COMPONENTS.md:** Added DisplayManager, updated FanController (✅ implemented), BLERelayManager (HR/CSC only).
- **DISPLAY_INTEGRATION_CONCEPT.md:** Added implementation update note (dirty-flag pattern).

- **Version:** 1.4.49, **Build ID:** `20260219-120000`
- **Zip release:** `Homewind-1.4.49.zip`

---

## [1.4.48] - 2026-02-18

### Added – Display update bug analysis

- **docs/DISPLAY_UPDATE_BUG_ANALYSIS.md:** Root-cause analysis of display values not updating (HR, CSC, Fans). Documents LVGL mutex race condition when BLE/Fan callbacks call LVGL APIs from NimBLE/Arduino tasks without holding `lvgl_mux`. Includes callback chains, task model, and mitigation options.

- **Version:** 1.4.48, **Build ID:** `20260218-223600`
- **Zip release:** `Homewind-1.4.48.zip`

---

## [1.4.47] - 2026-02-13

### Changed – OTA firmware URL based on display variant

- **Config.h:** `HW_FIRMWARE_GITHUB_URL` now selected at compile time via `HW_ENABLE_DISPLAY`: Touch (display) builds use `homewind-touch/latest.json`, headless (Basic) builds use `homewind-basic/latest.json`. Ensures OTA updates fetch the correct firmware variant.

- **Version:** 1.4.47, **Build ID:** `20260213-230000`
- **Zip release:** `Homewind-1.4.47.zip`

---

## [1.4.46] - 2026-02-13

### Added – ESP Web Tools manifest build script

- **tools/update_manifest.py:** New Python script to generate ESP Web Tools `manifest.json` from Arduino build output. Parses `flash_args` for filenames/offsets, reads version from `library.properties`. Supports `--board touch|basic|all` and `--deploy` to copy firmware files to the website directory.
- **tools/README.md:** Documentation for all build tools (update_manifest, build_webui, watch_webui) with usage, board mapping table, and directory structure.

- **Version:** 1.4.46, **Build ID:** `20260213-220000`
- **Zip release:** `Homewind-1.4.46.zip`

---

## [1.4.45] - 2026-02-03

### Removed – Power Meter (PWR) sensor support

- **BLERelayManager:** Removed PWR sensor type from discovery, callbacks, and NVS storage. `MAX_SENSORS` reduced from 3 to 2 (HR, CSC only).
- **ApiActions:** Removed "PWR" validation from `validateSensorType()`.
- **WebSocketTelemetry:** Updated sensor arrays from 3 to 2 slots.
- **DisplayManager:** Updated sensor arrays from 3 to 2 slots.
- **WebUI (app.js):** Removed PWR entries from `SENSOR_TYPE_REV`, `getSensorTypeLabel`, `iconMap`, and `DEFAULT_SENSOR_TYPES`.

### Changed – NVS data migration

- **BLERelayManager:** NVS sensor data structure size changed from 270 to 182 bytes. Existing NVS configurations will be reset; users need to reconfigure HR/CSC sensors after upgrade.

- **Version:** 1.4.45, **Build ID:** `20260203-030000`
- **Zip release:** `Homewind-1.4.45.zip`

---

## [1.4.44] - 2026-02-03

### Fixed – Fan HR control code quality

- **FanHRAdapter:** Tracking after power/speed commands now uses `isFanActiveForHr()` consistently (same as send decision). Fixes repeated commands to fans in soft-error phase whose tracking was not updated.
- **FanHRAdapter:** Renamed duplicate static flags for clarity: `noFansLogged` / `fansReadyLogged` (was two different `wasZero` in different scopes).
- **Reverted:** Heap optimization (`String _discoveryToken` → `char[33]`) to avoid potential heap corruption during handshake; kept `String` for discovery token.

- **Version:** 1.4.44, **Build ID:** `20260203-020000`
- **Zip release:** `Homewind-1.4.44.zip`

---

## [1.4.43] - 2026-02-03

### Added – Fan max speed define + stress/diagnostics

- **Config.h:** New `HW_FAN_MAX_SPEED_PERCENT` to cap fan speed globally (default 100%).
- **FanHRAdapter:** Max‑speed debug counters + timestamps for SEND/SKIP, plus post‑connect HR reassert logging.
- **FanController:** Recovery logs now include timestamps for unhealthy detection/threshold.
- **SmartMiFanAsync:** Debug timeout logs include timestamps (`[DBG_FAN_TIMEOUT]`).
- **WebUI:** Suppress WS reconnect during firmware download to prevent reconnect storms.
- **Example:** `MultipleFansSmartConnect` sends random speed around 90% every 3s and logs health‑checks.

- **Version:** 1.4.43, **Build ID:** `20260203-010000`
- **Zip release:** `Homewind-1.4.43.zip`

---

## [1.4.42] - 2026-02-03

### Fixed – Fan at 100% (HR > maxHR) causing timeout/ERROR

- **FanHRAdapter:** Max speed latch: 100% is sent only once when HR > maxHR and not re-sent until HR drops below maxHR. Prevents repeated UDP commands that could trigger timeouts and fan ERROR state.
- **FanHRAdapter.h/cpp:** New `_maxSpeedLatch`; reset on HR disconnect and when speed &lt; 100%.

### Fixed – Display showing "Not Configured" for configured-but-disconnected HR/CSC

- **App.cpp:** After BLE init, call `updateHRState()` and `updateCSCState()` once so the display shows "Not Connected" (with sensor name) instead of "Not Configured" when a sensor is configured but not yet connected.
- **DisplayManager.cpp:** Comment clarified: when state is INACTIVE the sensor is configured, so name is always set (no fallback needed).

- **Version:** 1.4.42, **Build ID:** `20260203-000000`
- **Zip release:** `Homewind-1.4.42.zip`

---

## [1.4.41] - 2026-02-01

### Added – Flash Size Optimization Report

- **Version:** 1.4.41, **Build ID:** `20260201-000000`
- **Docs:** New `docs/FLASH_SIZE_OPTIMIZATION_REPORT.md` with detailed analysis of program storage usage:
  - WebUI assets breakdown (~27 KB PROGMEM)
  - Production config recommendations (HW_ENABLE_DEBUG_LOGS=0, HW_ENABLE_SERIAL_COMMANDS=0 for ~18–33 KB savings)
  - Feature gate deactivation options (BLE, OTA, Web, Fans)
  - GZIP compression level and CSS optimization references
- **Zip release:** `Homewind-1.4.41.zip`

---

## [1.4.40] - 2026-01-29

### Fixed – mDNS Hostname

- **Version:** 1.4.40, **Build ID:** `20260129-040000`
- **WiFi/mDNS:** mDNS-Hostname wird nach Verbindung explizit mit ESP-APIs gesetzt (`MDNS.end()` + `MDNS.begin(hostname)`), sodass `homewind-XXXXXXXX.local` statt des ESP-Defaults `esp32s3-XXXXXX` angezeigt wird.
- **WiFiServiceWML.cpp:** Hostname-Format `homewind-XXXXXXXX` (8 Hex-Zeichen aus `ESP.getEfuseMac()`), HTTP-Service auf Port 80 registriert.
- **Zip release:** `Homewind-1.4.40.zip`

---

## [1.4.39] - 2026-01-29

### Added – UI ↔ Backend Flow Documentation

- **Version:** 1.4.39, **Build ID:** `20260129-030000`
- **Docs:** New `docs/UI_TO_BACKEND_FLOW.md` describing data flow UI ↔ backend for:
  - Verbundene Sensoren (sensor list, SENSORS_SNAPSHOT, SENSOR_CONN_STATE, DISCOVERY_RESULTS)
  - Heartrate senden (heartrate/setmin|setmax, HEART_RATE, HEART_RATE_SETTINGS)
  - Fan Count (FANS_SNAPSHOT count, fan/add, fan/remove, telemetry/refresh)
  - Fan State (fan/control, FANS_SNAPSHOT connected/control_active/recovery)
- **05_APIS.md:** Documented `sensor/set?index=<discovery-cache-index>` (UI selects from discovery list; backend resolves via `BikeSensorServerGetSensor(index, ...)`).
- **00_INDEX.md:** Linked `UI_TO_BACKEND_FLOW.md` under Implementation Details.

- **Zip release:** `Homewind-1.4.39.zip`

---

## [1.4.38] - 2026-01-29

### Optimized – UILock/SessionManager Static String Elimination

- **Version:** 1.4.38, **Build ID:** `20260129-020000`

#### UILock Optimierung
- **Static String Members entfernt:** `_ownerSession`, `_reason`, `_ownerHint` sind jetzt `char[]` Arrays
- **Getter zu `const char*`:** `getOwner()`, `getReason()`, `getOwnerHint()` geben direkte Pointer zurück
- **Parameter zu `const char*`:** `acquire()`, `release()`, `isOwner()`, `setOwnerHint()`

#### SessionManager Optimierung
- **`generateToken()`:** Jetzt `void generateToken(char* buffer, size_t size)` - schreibt direkt in Buffer
- **`validateToken()`:** Jetzt `const char*` statt `const String&`
- **`generateOwnerHint()`:** Jetzt `void generateOwnerHint(request, buffer, size)` - keine String-Allokation
- **Browser-Erkennung:** Verwendet `strstr()` statt `String.indexOf()`

#### Erwartete Verbesserung
- **Eliminierte statische Heap-Allokationen:** ~150 Bytes permanent (UILock Strings)
- **Eliminierte temporäre Allokationen:** ~100 Bytes pro Session-Operation

- **Zip release:** `Homewind-1.4.38.zip`

---

## [1.4.37] - 2026-01-29

### Optimized – Heap Fragmentation Reduction (String Allocations)

- **Version:** 1.4.37, **Build ID:** `20260129-010000`

#### Phase 1: FirmwareUpdateManager Optimierung
- **Getter zu `const char*` geändert:** `getLastError()`, `getRemoteVersion()`, `getReleaseNotes()` geben jetzt direkte Pointer auf interne Buffer zurück (keine String-Kopien mehr)
- **String-Konkatenation eliminiert:** `handleSearchComplete()` verwendet jetzt `snprintf()` statt wiederholter `+=` Operationen
- **Gecachte Werte:** Remote-Version und Release-Notes werden einmalig in interne Buffer kopiert

#### Phase 2: API-Handler Parameter-Optimierung
- **Neue WebHelpers-Funktionen:** `getParamToBuffer()`, `getIntParam()`, `getBoolParam()` für heap-freie Parameter-Extraktion
- **ApiActions optimiert:** `getBoolParam()` und `getIntParam()` verwenden jetzt Fixed-Buffers statt String
- **Weitere Handler:** `handleFanControlState()`, `handleSensorAdd()`, `handleSensorSelect()`, `handleMaintenanceBegin()` auf Fixed-Buffer umgestellt

#### Phase 3: IPAddress Formatierung
- **Neue Funktion `ipToBuffer()`:** Formatiert IPAddress direkt ohne `toString()` (keine String-Allokation)
- **FanController.cpp:** 11 Stellen von `ip.toString().c_str()` auf `ipToBuffer()` umgestellt

#### Erwartete Verbesserung
- **Eliminierte String-Allokationen:** ~20-30 pro typischem Request/Connect-Zyklus
- **Temporäre Heap-Nutzung:** ~300-500 Bytes weniger pro Request
- **Fragmentierung:** Geschätzte Reduktion von ~52% auf ~43-45%

- **Zip release:** `Homewind-1.4.37.zip`

---

## [1.4.36] - 2026-01-28

### Fixed – Production Heap Spikes (Cold Load) + WS=2 Snapshot Delivery

- **Version:** 1.4.36, **Build ID:** `20260128-020000`
- **WiFi (WML/Production)**: Pre-init now uses `WIFI_STA` (instead of `WIFI_AP_STA`) to reduce early heap pressure. `wifiMangerLite` still switches to `WIFI_AP_STA` when AP mode is actually entered.
- **BLE (Two-Phase)**: NimBLE stack pre-init now happens at `PH_RUNNING` even while STA is still coming up (Portal/STA-offline). This improves early heap locality (larger `largest_free_block` after NimBLE init).
- **WebSocket / WebUI (WS=2)**:
  - Heavy snapshots (`SENSORS_SNAPSHOT` / `FANS_SNAPSHOT`) are now refreshable **per-client via unicast** (prevents broadcast bursts during cold page loads).
  - WebUI reliably receives `FANS_SNAPSHOT` again (Fan Cards no longer missing in multi-client scenarios).
  - WebUI build updated (embedded assets regenerated).

- **Zip release:** `Homewind-1.4.36.zip`

---

## [1.4.35] - 2026-01-28

### Changed – HR Fail-Safe (Smart Home Friendly)

- **Version:** 1.4.35, **Build ID:** `20260128-010000`
- **Fans / HR**: When HR disconnects, Homewind now sends **Power OFF only twice**:
  - once immediately on HR disconnect (if HR was previously connected)
  - once more after 10 seconds
  - no further periodic OFF “reassert” traffic
- **Why**: Prevents Homewind from repeatedly turning off fans that are being controlled via Smart Home mode when no HR sensor is connected.

- **Zip release:** `Homewind-1.4.35.zip`

---

## [1.4.34] - 2026-01-28

### Added – Fan Auto-Recovery + UI Feedback

- **Version:** 1.4.34, **Build ID:** `20260128-000000`
- **Fans:** Automatic recovery when a fan enters ERROR-ish state (debounced)
  - Retries are tunable via compile-time defines:
    - `HW_FAN_AUTO_RECOVERY_UNHEALTHY_CHECKS`
    - `HW_FAN_AUTO_RECOVERY_MAX_ATTEMPTS`
    - `HW_FAN_AUTO_RECOVERY_RETRY_INTERVAL_MS`
- **WebUI:** Fan switch shows **Recovering…** (disabled) during auto-recovery, and has a **3s cooldown** after toggles to avoid rapid flip-flopping.

#### Protocol Change
- **WebSocket `FANS_SNAPSHOT`** fan entry extended: **34 → 35 bytes** with `recovery` flag
  - `recovery`: 0=normal, 1=recovering, 2=exhausted (needs user action)

- **Zip release:** `Homewind-1.4.34.zip`

---

## [1.4.33] - 2026-01-27

### Fixed – WS=2 Heap Stability (Largest Block Preservation)

- **Version:** 1.4.33, **Build ID:** `20260127-020000`
- **Fix:** WebSocket connect + snapshot scheduling no longer craters `largest_free_block` when 2 clients are connected.

#### **What Changed**
- ✅ **Staged connect is now per-client (unicast)** for heavy snapshots (SENSORS/FANS)
  - Each newly connected client receives its own SENSORS/FANS snapshots without triggering `binaryAll()` bursts.
- ✅ **Budgeted periodic heavy snapshots**
  - SYSTEM_STATUS remains periodic (lightweight)
  - SENSORS/FANS refresh is reduced and staggered for WS=2+ to avoid heap fragmentation spikes
- ✅ **Compatibility fix** for ESPAsyncWebServer client API
  - Uses `client->status() == WS_CONNECTED` (no `connected()` method in this lib)

#### **Validation (Heap Logs)**
- ✅ **WS=2 stable for minutes** without large-block collapse
  - Typical: `largest ≈ 63–65 KB`, `frag ≈ 40–42%` under WS=2
  - No recurrence of BootLog4 behavior (`largest≈36 KB`, `frag≈66%` on 2nd client)

- **Zip release:** `Homewind-1.4.33.zip`

---

## [1.4.32] - 2026-01-27

### Verified – Long-Term Stability Confirmed

- **Version:** 1.4.32, **Build ID:** `20260127-010000`
- **Stability:** 7+ hours runtime without degradation
- **Documentation:** BlockerMix11 heap analysis added

#### **Long-Term Stability Verification**
- ✅ **7+ hours runtime test** (BlockerMix11)
  - No memory leaks detected
  - Stable fragmentation: 44.5% - 47.9% (avg 47.6%)
  - Stable largest block: 55-57 KB (typical 57 KB)
  - Stable minFree: 59 KB
  - No upward trend in fragmentation
- ✅ **Heap Metrics Stability**
  - largest block: Remains stable at ~57 KB
  - fragmentation: No progressive increase
  - free heap: Stable at ~110 KB
  - minFree: Stable at 59 KB (sufficient headroom)
- ✅ **System Health**
  - 2 WebSocket clients connected
  - 2 Fans connected and ready
  - No memory pressure events
  - All operations functioning correctly

#### **Analysis Results**
| Metric | Value | Status |
|--------|-------|--------|
| **Runtime** | 7+ hours | ✅ Stable |
| **largest block** | 55-57 KB | ✅ Stable |
| **Fragmentation** | 44.5-47.9% | ✅ Stable |
| **minFree** | 59 KB | ✅ Stable |
| **Memory leaks** | None detected | ✅ Clean |

#### **Documentation**
- Added `docs/BLOCKERMIX11_HEAP_ANALYSIS.md` with detailed analysis
- Confirms boot sequence optimization (1.4.31) working correctly
- Validates long-term stability of heap management

- **Zip release:** `Homewind-1.4.32.zip`

---

## [1.4.31] - 2026-01-27

### Changed – Boot Sequence Optimization (Heap Fragmentation Reduction)

- **Version:** 1.4.31, **Build ID:** `20260127-000000`
- **Heap Optimization:** Optimized initialization order reduces fragmentation by 5.1%
- **Boot Sequence:** FirmwareUpdate → Fans → BLE (after POST_CTRL_DONE)

#### **Boot Sequence Optimization**
- ✅ **Reordered initialization sequence**
  - FirmwareUpdate: First (12KB stack, no heap, needs WebServer)
  - Fans: Second (~8KB heap, SmartConnect async)
  - BLE: Last (~64KB heap, after POST_CTRL_DONE)
- ✅ **BLE deferred until POST_CTRL_DONE**
  - BLE initializes after Fans POST-CONNECTION CONTROL SEQUENCE completes
  - Heap is "settled" before largest allocation (BLE)
  - New method: `FanController::isPostConnectDone()` to detect completion
- ✅ **Benefits**
  - Better heap compaction (small → large allocations)
  - Larger contiguous free block after Fans
  - Reduced fragmentation

#### **Measured Improvements**
| Metric | Before (BLE→Fans) | After (Fans→BLE) | Improvement |
|--------|-------------------|------------------|-------------|
| **largest after Fans** | 73 KB | 139 KB | **+66 KB (+90%)** |
| **largest after POST_CTRL_DONE** | 65 KB | 139 KB | **+74 KB (+114%)** |
| **fragmentation** | 41.2% | 36.1% | **-5.1%** |
| **free after POST_CTRL_DONE** | 113 KB | 180 KB | **+67 KB** |

#### **Technical Details**
- FirmwareUpdate initialized immediately after WebServer (no dependencies)
- Fans initialized next (small heap footprint, SmartConnect async)
- BLE deferred in `loopSubsystems()` until `isPostConnectDone()` returns true
- FanHRAdapter initialization deferred until both Fans and BLE ready
- No functional impact - all services work correctly

#### **Dependencies**
- **GitFirmwareUpdate:** v1.0.3 (HTTP/HTTPS auto-detection)

- **Zip release:** `Homewind-1.4.31.zip`

---

## [1.4.30] - 2026-01-26

### Changed – Sequential Web Asset Loading + WebSocket Max Clients UX

- **Version:** 1.4.30, **Build ID:** `20260126-235000`
- **Heap Optimization:** Sequential loading of CSS/JS assets reduces heap fragmentation by 15%
- **UX Improvement:** Clear error message when max WebSocket clients reached
- **WebUI Build:** `4ebb887b`

#### **Sequential Asset Loading**
- ✅ **Changed from parallel to sequential web asset loading**
  - Browser now loads `app.css` first, waits for completion
  - Then loads `app.js` after CSS `onload` event
  - ESP32 no longer needs to buffer both responses simultaneously
- ✅ **Implementation in `index.html`**
  - Added `onload="window._cssLoaded=true"` to CSS link tag
  - Replaced `<script defer>` with dynamic script injection after CSS load
  - Zero-overhead: inline JavaScript loader (~200 bytes)
- ✅ **DOMContentLoaded Fix for Dynamic Script Loading**
  - Fixed button initialization when `app.js` loads after `DOMContentLoaded` event
  - Added fallback pattern: check `document.readyState` before attaching listeners

#### **WebSocket Max Clients UX**
- ✅ **Server-side rejection with clear error message**
  - Server sends WebSocket Close Code 1008 (Policy Violation) when max clients reached
  - Close reason: `"MAX_CLIENTS_REACHED"`
- ✅ **Client-side error handling**
  - JavaScript detects Close Code 1008 and shows user-friendly error modal
  - Message: "Maximale Anzahl von 2 Clients erreicht"
  - Prevents automatic reconnection (user must close another tab)
- ✅ **Benefits**
  - No more empty UI when 3rd client connects
  - Clear feedback to user about connection limit
  - Prevents unnecessary reconnection attempts

#### **Measured Improvements**
| Metric | Before (parallel) | After (sequential) | Improvement |
|--------|-------------------|-------------------|-------------|
| **largest block** | 47 KB | 63 KB | **+16 KB (+35%)** |
| **fragmentation** | 58% | 42% | **-15 percentage points** |
| **minFree** | 53 KB | 67 KB | **+14 KB** |

#### **Technical Details**
- Parallel HTTP requests caused concurrent TCP buffer allocations
- Each HTTP response requires ~4-8 KB buffer on ESP32
- Sequential loading allows buffer reuse between requests
- No impact on perceived load time (CSS is render-blocking anyway)
- WebSocket and API calls unaffected (separate connections)

#### **Dependencies**
- **GitFirmwareUpdate:** v1.0.3 (HTTP/HTTPS auto-detection, lwIP crash fix)

- **Zip release:** `Homewind-1.4.30.zip`

---

## [1.4.29] - 2026-01-25

### Changed – Memory Optimization: ApiActions/ApiSettings as Global Static Instances

- **Version:** 1.4.29, **Build ID:** `20260125-221500`
- **Memory Model Consistency:** ApiActions and ApiSettings now use global static instances
- **Code Quality:** Aligned with Memory Model (Stufe 3) - all subsystems use global static instances

#### **ApiActions/ApiSettings Optimierung**
- ✅ **Changed from local static to global static instances**
  - `ApiActions s_apiActions` - now global static (like s_webServer, s_wifi, etc.)
  - `ApiSettings s_apiSettings` - now global static (like s_webServer, s_wifi, etc.)
  - Eliminates local static allocations in `initWeb()`
- ✅ **Consistent Memory Model**
  - All subsystems now use global static instances
  - Follows Memory Model Stufe 3 pattern
  - Deterministic memory usage throughout boot process
- ✅ **Code Structure**
  - All static subsystem instances in one place (App.cpp:65-113)
  - Clearer code organization
  - Easier to maintain and understand

#### **Benefits**
- ✅ **Memory Model Consistency:** All subsystems follow same pattern
- ✅ **Code Quality:** Cleaner, more maintainable structure
- ✅ **No Functional Changes:** Fully backward compatible
- ✅ **Deterministic:** Objects live for entire program lifetime

#### **Technical Details**
- Moved `static ApiActions s_apiActions` from `initWeb()` to global scope
- Moved `static ApiSettings s_apiSettings` from `initWeb()` to global scope
- Updated usage in `initWeb()` to reference global instances
- No breaking changes - all APIs preserved
- Verified: No linter errors, consistent with other subsystems

- **Zip release:** `Homewind-1.4.29.zip`

---

## [1.4.28] - 2026-01-25

### Changed – Memory Optimization: String Allocations & Boot Fragmentierung Analysis

- **Version:** 1.4.28, **Build ID:** `20260125-220000`
- **Memory Optimization:** Eliminated String allocations in critical paths
- **Documentation:** Comprehensive boot fragmentation analysis

#### **String-Optimierungen**
- ✅ **FirmwareUpdateManager::getStateString()** - Changed from `String` to `const char*` with static buffer
  - Eliminates heap allocation on every call (called multiple times per second)
  - Uses static buffer (32 bytes) instead of String objects
  - Updated all callers: `WebSocketTelemetry`, `ApiActions`
- ✅ **WiFiServiceWML Debug-Prints** - Optimized IP string formatting
  - Changed from `toString().c_str()` to `ipString()` (uses existing static buffer)
  - Eliminates 2 String allocations during WiFi initialization
- ✅ **Impact:** Reduced heap fragmentation from repeated String allocations

#### **Boot-Fragmentierungs-Analyse**
- ✅ **Comprehensive Analysis Document:** `docs/BOOT_FRAGMENTATION_ANALYSIS.md`
  - Deep analysis of boot phases and fragmentation sources
  - 7 optimization concepts with detailed evaluation
  - Recommendations: WebServer Lazy Load, Captive Portal Lazy Load, String Optimization
  - Expected impact: 50-70% fragmentation reduction
- ✅ **Boot-Phasen-Analyse:** `docs/BOOT_PHASES_OPTIMIZATION_ANALYSIS.md`
  - Detailed analysis of each boot phase (PH_CORE, PH_NVS, PH_WIFI, PH_WEB)
  - Identified optimization opportunities
  - ApiActions/ApiSettings should be global static instances

#### **Benefits**
- ✅ **Memory:** Reduced heap fragmentation from String allocations
- ✅ **Performance:** Faster execution (no String object creation overhead)
- ✅ **Documentation:** Comprehensive analysis for future optimizations
- ✅ **No Functional Changes:** Fully backward compatible

#### **Technical Details**
- All String optimizations use static buffers (deterministic memory)
- Arduino IPAddress types properly handled (no toString() calls)
- All changes verified and tested
- Documentation includes implementation proposals for future work

- **Zip release:** `Homewind-1.4.28.zip`

---

## [1.4.27] - 2026-01-25

### Changed – Code Optimization: Duplicate Code Elimination

- **Version:** 1.4.27, **Build ID:** `20260125-212434`
- **Code Deduplication:** Removed all identified code duplicates and legacy APIs
- **Flash Reduction:** ~270-380 bytes saved through consolidation

#### **CRC16-Konsolidierung**
- ✅ Created centralized `Crc16.h/.cpp` utility (namespace `Crc16`)
- ✅ Removed 3 duplicate CRC16-CCITT implementations from:
  - `FanController::calcCRC16()` (removed)
  - `HeartRateSettings::calcCRC16()` (removed)
  - `BLERelayManager::calcCRC16()` (removed)
- ✅ All classes now use `Crc16::calcCRC16()` - single source of truth
- ✅ Flash savings: ~60-80 bytes

#### **Web-Helper Deduplizierung**
- ✅ Created centralized `WebHelpers.h/.cpp` utility (namespace `WebHelpers`)
- ✅ Removed 2 duplicate implementations:
  - `sendMaintenance503()` - now `WebHelpers::sendMaintenance503()`
  - `getStringParam()` - now `WebHelpers::getStringParam()`
- ✅ Both `ApiActions` and `ApiSettings` use shared helpers
- ✅ Flash savings: ~100-150 bytes

#### **Token/ID-Resolve Extraktion**
- ✅ Extracted duplicate token resolution code into `ApiActions::resolveFanToken()`
- ✅ Removed duplicate code from:
  - `handleFanControl()` (was 20 lines)
  - `handleFanRemove()` (was 20 lines)
- ✅ Centralized function handles 8-char ID → 32-char token resolution
- ✅ Flash savings: ~80-100 bytes

#### **Legacy API Entfernung**
- ✅ Removed unused HeapDiagnostics legacy functions:
  - `hwHeapDiagIsReadyCaptured()` (not called anywhere)
  - `hwHeapDiagIsFirstWsCaptured()` (not called anywhere)
- ✅ Replaced by `hwHeapDiagIsMarkerCaptured(Marker)` - modern API
- ✅ Flash savings: ~30-50 bytes

#### **Benefits**
- ✅ **Code Reduction:** ~270-380 bytes Flash saved
- ✅ **Maintainability:** Single source of truth for common functions
- ✅ **Consistency:** All modules use identical implementations
- ✅ **No Functional Changes:** Fully backward compatible

#### **Technical Details**
- All optimizations verified against original code
- No breaking changes - all APIs preserved
- Feature gates respected (`HW_ENABLE_FANS`, `HW_ENABLE_BLE`, etc.)
- Documentation: `docs/ANALYSE_VERIFIKATION.md`, `docs/OPTIMIERUNGEN_UMGESETZT.md`

- **Zip release:** `Homewind-1.4.27.zip`

---

## [1.4.26] - 2026-01-25

### Changed – Code Quality: Centralized HTML Escaping

- **Version:** 1.4.26, **Build ID:** `20260125-194056`
- **Code Deduplication:** Centralized `escapeHtml` utility function
- **Code Reduction:** ~20 lines removed (3 duplicate implementations eliminated)

#### **HTML Escaping Centralization**
- ✅ Created global `window.escapeHtml()` utility function
- ✅ Removed duplicate `escapeHtml` implementations from:
  - `window.showErrorModal` (was local function)
  - `FirmwareUpdate` module (was private function)
  - `BLESensorManager` module (was local const)
- ✅ Updated all 14 usages to use centralized `window.escapeHtml()`
- ✅ Improved maintainability: Single source of truth for XSS protection

#### **Benefits**
- ✅ **Code Reduction:** ~20 lines eliminated
- ✅ **Maintainability:** One function to maintain instead of three
- ✅ **Consistency:** All modules use identical HTML escaping logic
- ✅ **XSS Protection:** Centralized security function

#### **Technical Details**
- Function defined globally as `window.escapeHtml` (accessible to all modules)
- Escapes: `&`, `<`, `>`, `"`, `'` to HTML entities
- Returns empty string for non-string inputs
- Fully backward compatible (no functional changes)

- **Zip release:** `Homewind-1.4.26.zip`

---

## [1.4.25] - 2026-01-25

### Changed – Major WebUI Refactoring & Code Quality Improvements

- **Version:** 1.4.25, **Build ID:** `20260125-145456`
- **WebUI Refactoring:** Comprehensive code cleanup and optimization of `app.js` (5215 → 5200 lines)
- **Code Reduction:** ~254 lines of code eliminated through deduplication and optimization
- **Performance:** Improved DOM query caching, input debouncing, and error handling patterns

#### **Phase 1: Code Deduplication & Cleanup**
- ✅ Removed unused `i8()` function
- ✅ Removed deprecated firmware callbacks (`onFirmwareSearchResult`, `onFirmwareDownloadProgress`)
- ✅ Consolidated Heart Rate callbacks (87 → 30 lines, removed legacy WSClient API)
- ✅ Unified Token Validation (3 functions → 1 flexible function with options)
- ✅ Simplified redundant API parameters (removed duplicate `id`/`name`, `control`/`controlState`)

#### **Phase 2: Error Handling & Performance**
- ✅ Created `window.showErrorModal()` utility function (replaced 6 duplicate error modal patterns)
- ✅ Optimized DOM queries (cached HR elements in fan animation system)
- ✅ Added input debouncing (150ms for token validation, 80ms for scroll events)
- ✅ Improved performance: ~130 lines reduced, better runtime efficiency

#### **Phase 3: Code Quality & Maintainability**
- ✅ FirmwareUpdate now uses `ApiV1` directly (removed wrapper indirection)
- ✅ Extracted Magic Numbers to `window.TIMING` constants object
- ✅ Memory leak prevention (ESC key handler stored for cleanup)
- ✅ Self-documenting code with named constants

#### **Build Results**
- **app.js:** 189,585 bytes → 20,003 bytes (87.8% reduction after GZIP)
- **Total embedded:** 27,732 bytes (87.8% reduction from original)
- **Build hash:** `dd6d96e4`

#### **Compatibility**
- ✅ Fully backward compatible with Homewind
- ✅ All public APIs preserved (`window.ApiV1`, `window.AppModal`, etc.)
- ✅ All Arduino callbacks maintained
- ✅ WebSocket protocol unchanged

#### **Documentation**
- Added `APP_JS_REFACTORING_ANALYSIS.md` - Comprehensive analysis document
- Added `REFACTORING_IMPLEMENTATION_LOG.md` - Detailed implementation log

- **Zip release:** `Homewind-1.4.25.zip`

---

## [1.4.24] - 2026-01-25

### Changed – Release 1.4.24

- **Version:** 1.4.24, **Build ID:** `20260125-135954`
- WebUI rebuild (build_webui.py); Device Restart button uses `btn-cta` class
- Zip release: `Homewind-1.4.24.zip`

---

## [1.4.23] - 2026-01-25

### Fixed – Device Restart & Firmware Update UI

#### **Device Restart – False "Device Restart Failed"**
- When the device restarts, the TCP connection closes before the client receives the full HTTP response. The UI previously treated this as a failure and showed "Device Restart Failed" even though the device rebooted successfully.
- **Fix:** Differentiate HTTP errors (4xx/5xx) from connection drops. On connection drop (e.g. "Load failed"), treat as "restart in progress", show "Device Restarting", and start reboot monitoring. Only show "Device Restart Failed" on explicit HTTP errors (e.g. 503).

#### **Firmware Update – Progress Bar, Installing Modal, Modal Close**
- **Progress bar:** Progress was set to `DOWNLOADING` only after the download HTTP call succeeded. WebSocket `FIRMWARE_PROGRESS` frames can arrive earlier, so early updates were ignored and the bar appeared to jump or stick.
- **Fix:** Set `DOWNLOADING` and open the download modal **before** the HTTP call. All WebSocket progress updates from 0% are now applied.
- **Installing modal:** The "Installing" phase was skipped (ESP32-OTA goes straight to Restarting). Users saw no "Installing" step.
- **Fix:** At 100% (or `handleDownloadComplete`), show "Installing Firmware" modal for ~1.5s, then "Restarting Device" and reboot monitoring.
- **Modal not closing on restart:** Reboot detection used `params.state === "idle" || params.state === "unavailable" || params.state`. The latter matched any truthy state (e.g. `installing`, `downloading`), so success could be detected too early and behaviour was inconsistent.
- **Fix:** Only treat `state === "idle"` or `state === "unavailable"` as "device back". Modal close + reload only then.

#### **Shared Reboot Monitoring**
- Firmware Update (RESTARTING) and Device Restart / WiFi Reset / Factory Reset now use a single `window.startRebootMonitoring` helper. Same polling, same "Reboot Taking Longer" modal, less duplication.

### Added – Firmware Update Robustness (WebSocket Disconnect During OTA)

#### **P1: HTTP Status Polling During Update**
- WebSocket often disconnects during OTA (device busy). Progress and phase transitions depended only on WS, so the UI could stick at e.g. 66% and never show Installing or close.
- **Fix:** While in `DOWNLOADING`, poll `GET /api/v1/action/firmware/status` every 2.5s. Use `progress` for the progress bar and `state` for phase transitions (download complete → Installing → Restarting; `idle`/`unavailable` → close + reload). Works even when WS is down.

#### **P2: WS Disconnect → Ensure Polling**
- On WebSocket `onclose`, if the firmware flow is in `DOWNLOADING`, call `ensureStatusPollingWhenDownloading`. This starts or restores HTTP status polling immediately, so we don’t rely on reconnect during the update.

### Technical Notes
- **Build ID:** `20260125-120000`
- **Docs:** `FIRMWARE_UPDATE_UI_FIXES.md`, `FIRMWARE_UPDATE_WS_DISCONNECT_SOLUTIONS.md`

---

## [1.4.22] - 2026-01-24

### Added - Fan Error Diagnostics and Auto-Recovery

#### **Fan Status Logging:**
- Added detailed fan status logging to heap diagnostics
- Shows `ready`, `state` (ACTIVE/INACTIVE/ERROR), `error` (OK/TIMEOUT/WRONG_IP/DECRYPT_FAIL/INVALID_RESP), and `token` prefix for each discovered fan
- Logs appear every 30 seconds with heap status, making it easy to identify which fan has issues

#### **Auto-Recovery on ERROR State:**
- When a fan in ERROR state is activated via UI, system now automatically attempts handshake to reconnect
- Handshake retry with 3 second timeout when fan is enabled while in ERROR state
- Success/failure is logged for debugging

#### **Benefits:**
- ✅ **Better diagnostics**: Can immediately see which fan has ERROR state and what error occurred
- ✅ **User-friendly recovery**: Clicking on ERROR fan in UI automatically attempts reconnection
- ✅ **Reduced manual intervention**: No need to restart discovery or manually trigger handshake

#### **Technical Details:**
- Fan status logging added to `HeapDiagnostics::hwHeapDiagRuntimeTick()` (lines 197-231)
- Handshake retry logic added to `FanController::setFanControlState()` (lines 402-434)
- Uses `SmartMiFanAsync.handshake()` which automatically updates `ready` and `lastError` in discovered fan array

---

## [1.4.21] - 2026-01-24

### Fixed - MAC Address Byte Order

#### **Problem:**
After sensor connection, `BikeSensorServerGetSensorMac()` returned MAC addresses in reversed byte order (e.g., "97:56:EE:C3:13:F0" instead of "F0:13:C3:EE:56:97"), causing incorrect MAC addresses to be stored in NVS.

#### **Root Cause:**
`NimBLEAddress::getVal()` returns MAC address bytes in reverse order (Little-Endian BLE format), but `updateSensorMacFromAddress()` in BluetoothBikeSensorServer was copying them directly without reversing.

#### **Solution:**
- Updated to `BluetoothBikeSensorServer` v2.2.15.4
- `updateSensorMacFromAddress()` now reverses bytes when copying from `NimBLEAddress::getVal()`
- MAC addresses are now stored in Big-Endian format (standard MAC format)

#### **Impact:**
- ✅ `BikeSensorServerGetSensorMac()` returns correct MAC addresses
- ✅ MAC addresses stored in NVS after connection are correct
- ✅ Consistent MAC address format throughout the system
- ⚠️ **Migration:** Existing NVS entries with reversed MACs need to be cleared or re-saved via discovery

#### **Technical Details:**
- `NimBLEAddress::getVal()` returns bytes in Little-Endian BLE format
- `NimBLEAddress::toString()` correctly formats bytes in Big-Endian format (used in Discovery)
- `updateSensorMacFromAddress()` now reverses bytes: `sensor.mac[i] = mac[5 - i]`

---

## [1.4.20] - 2026-01-24

### Changed - Discovery Flow Optimization (Index-Based Selection)

#### **What Changed:**
Discovery flow optimized to use index-based sensor selection instead of sending all sensor data (Name, MAC, Type, AddrType) to UI and back.

#### **New Flow:**
1. **Discovery → UI**: Only sends `[index:uint8][name_len:uint8][name:bytes]` (54% bandwidth reduction)
2. **UI → Backend**: Only sends `index:uint8` (or `-1` for cancel) instead of full sensor data
3. **Backend Resolution**: Backend resolves full sensor data (MAC, Type, AddrType) from discovery cache using `BikeSensorServerGetSensor(index)`

#### **Benefits:**
- ✅ **54% bandwidth reduction**: From ~240 bytes to ~111 bytes for 5 sensors
- ✅ **Single source of truth**: Discovery cache stays on Arduino, no data duplication
- ✅ **Error prevention**: UI cannot send invalid MAC addresses (doesn't have them)
- ✅ **Simpler code**: No data duplication between UI and backend
- ✅ **Less memory**: UI stores only index + name instead of full sensor objects

#### **Technical Changes:**
- **WebSocket Frame Format**: `DISCOVERY_RESULTS` now sends only `[index][name]` per sensor
- **API Endpoint**: `/api/v1/action/sensor/set` now accepts `index` parameter instead of `name/type/mac`
- **UI Selection**: `saveSelectedSensor()` sends only index to backend
- **Backend Resolution**: `handleSensorSet()` resolves full data from discovery cache via `BikeSensorServerGetSensor(index)`

#### **Breaking Changes:**
- **WebSocket Protocol**: `DISCOVERY_RESULTS` frame format changed (internal system, no external consumers)
- **API Endpoint**: `/api/v1/action/sensor/set` parameter changed from `name/type/mac` to `index`

#### **Migration:**
- No data migration needed (NVS format unchanged)
- UI and backend must be updated simultaneously (internal system)

### Technical Notes
- Build ID: `20260124-180000` (serial number format: YYYYMMDD-HHMMSS)
- Discovery cache persists until new discovery completes or system reboot
- Index validation: Backend validates `index < BikeSensorServerGetSensorCount()` before resolving

---

## [1.4.19] - 2026-01-24

### Fixed
- **MAC Address Byte Order**: Fixed MAC address storage and connection issues
  - Updated to use `BluetoothBikeSensorServer` v2.2.15.2 with corrected MAC byte order
  - `BikeSensorServerGetSensor()` now returns raw MAC bytes correctly
  - `formatSensorMac()` in library now outputs bytes in correct order (mac[0]..mac[5])
  - MAC addresses stored in NVS now match standard format (e.g., "24:ac:ac:0d:8a:93")
  - Sensor connections now work reliably with correct MAC addresses

### Changed
- **BluetoothBikeSensorServer Integration**: Updated to v2.2.15.2
  - `BikeSensorServerGetSensor()` signature changed: returns `uint8_t mac[6]` instead of `char* mac`
  - `BLERelayManager::getDiscoveryResults()` updated to format MAC bytes inline using `snprintf()`
  - No new conversion functions added (as requested)

### Technical Notes
- Build ID: `20260124-171630` (serial number format: YYYYMMDD-HHMMSS)
- ⚠️ **Migration:** Existing NVS entries with reversed MAC addresses need to be cleared or re-saved via discovery
- MAC addresses are stored as strings in NVS (`char[18]`), formatted from raw bytes when needed

---

## [1.4.18] - 2026-01-24

### Added
- **BLE Address Type Support**: Full support for BLE address types (Public/Random) in sensor connections
  - New `addrType` field in sensor configuration (0=Public, 1=Random, -1=Unknown/auto-detect)
  - `BikeSensorServerAddSensorByMac()` now requires `addrType` parameter for reliable MAC-direct connections
  - Address type extracted from discovery results and stored in NVS
  - Frontend WebUI updated to display and send `addrType` during sensor setup
  - WebSocket `DISCOVERY_RESULTS` frame extended with `addrType` field

### Changed
- **NVS Schema Migration**: Sensor configuration NVS schema updated from version 1 to version 2
  - `NVSSensorEntry` now includes `int8_t addrType` field
  - `NVSSensorData` version incremented to 2 (was 1)
  - Structure size: 268 bytes → 270 bytes (with `#pragma pack(push, 1)`)
  - Old NVS data is overwritten on first save (no backward-compatible migration)
- **BLERelayManager**: Updated to use `BBS_MODE_MAC` exclusively for MAC-based sensor connections
  - All sensor connections now use `BikeSensorServerAddSensorByMac()` with `addrType`
  - `DesiredSensorTarget`, `AppliedSensorState`, and `SensorConfig` structures extended with `addrType`
  - `getDiscoveryResults()` parses `addrType` from `BikeSensorServerListSensors(LIST_FORMAT_FULL)` output
- **WebSocket Protocol**: `DISCOVERY_RESULTS` frame format updated
  - Format: `[name_len:uint8][name:bytes][type:uint8][mac_len:uint8][mac:bytes][addrType:int8]`
  - Frontend parser updated to read `addrType` from frame
- **API Actions**: `handleSensorSet()` now accepts `addrType` parameter (from HTTP request)
  - Supports both `addrType` and `addrtype` parameter names (case-insensitive)

### Technical Notes
- Build ID: `20260124-011052` (serial number format: YYYYMMDD-HHMMSS)
- Address type is critical for reliable BLE connections, especially with Random addresses
- Migration overwrites old NVS data as requested (no backward compatibility needed)

---

## [1.4.17] - 2026-01-21

### Added
- **WebSocket Client Limit**: Server-side max clients configurable via `HW_WEBSOCKET_MAX_CLIENTS`
  - Default limit set to 2 in `Config.h`
  - Excess clients are rejected on connect to prevent overlap fragmentation

---

## [1.4.16] - 2026-01-21

### Fixed
- **WebSocket Overlap**: Prevents parallel connections during reconnects
  - Client now blocks new WS while old socket is `CLOSING` and clears `ws` on close
  - Staged snapshot connect now runs only when the first client connects

---

## [1.4.15] - 2026-01-21

### Added
- **Telemetry Refresh Endpoint**: New `POST /api/v1/action/telemetry/refresh` triggers immediate snapshot updates
  - Marks `SYSTEM_STATUS`, `SENSORS_SNAPSHOT`, `FANS_SNAPSHOT`, `FIRMWARE_VERSION`, and `HEART_RATE_SETTINGS`
  - Allows UI to request a fresh snapshot after load without waiting for the 30s interval

### Changed
- **WebUI Telemetry Refresh**: UI now requests a refresh on WebSocket connect
  - Refresh is retried when WebSocket reconnects
  - Prevents missed first snapshots after page reloads or reconnects

---

## [1.4.14] - 2026-01-20

### Fixed
- **Firmware Update Reboot Monitoring**: Fixed WebUI not reloading after firmware update and restart
  - `startRebootMonitoringForRestart()` now uses `/api/v1/action/firmware/status` instead of `/api/v1/system/info`
  - The firmware status endpoint is whitelisted during maintenance mode, allowing detection of device restart
  - Previously `/api/v1/system/info` returned 503 during OTA maintenance mode, preventing automatic page reload
  - Response parsing added to handle `state=idle` or `state=unavailable` from firmware status endpoint

---

## [1.4.13] - 2026-01-20

### Fixed
- **Heap Diagnostics BLE Scan Status**: Fixed incorrect scan status in heap diagnostics log
  - Changed from `isDiscoveryActive()` to `BikeSensorServerIsScanning()` from BluetoothBikeSensorServer library
  - Now correctly shows server-mode scan status (reconnect scans)
  - Previously showed `Scan=N` even when server-mode scan was active for reconnecting sensors

---

## [1.4.12] - 2026-01-20

### Changed
- **WebSocket Optimization**: Reduced frame buffer size from 1024 to 512 bytes
  - Saves 512 bytes RAM per WebSocketTelemetry instance
  - Largest frame is ~200 bytes, 512 bytes is sufficient
- **WebSocket Frame Bug Fix**: Fixed SENSOR_CONN_STATE sending incorrect sensor
  - Changed from SENSOR_CONN_STATE to SENSORS_SNAPSHOT in BLERelayManager
  - Now correctly sends all sensors instead of just the first one
- **Heart Rate Settings via WebSocket**: Removed HTTP GET endpoint for heart rate settings
  - Settings now sent via WebSocket HEART_RATE_SETTINGS frame (0x0B)
  - Reduces HTTP overhead (~250-350 bytes → 5 bytes WebSocket frame)
  - Consistent with other live-update data
- **Debug Logging**: Added granular control for FanHRAdapter verbose logs
  - New config: `HW_DEBUG_FANHR` (default: 0, disabled)
  - HR samples and speed calculation logs now gated separately
  - Reduces Serial output noise in production while keeping important state changes
- **Heap Diagnostics**: Added BLE scanning status to heap diagnostics log
  - Format: `[Heap] STATUS: WS=X, BLE=X/Y, Scan=Y/N, Discovery=Z, Fans=X/Y`
  - Shows if BLE discovery is actively scanning

### Removed
- **Legacy Migration Code**: Removed all legacy NVS migration code
  - Removed `migrateFromLegacy()` from BLERelayManager, FanController, HeartRateSettings
  - Removed `NVS_KEY_LEGACY` constants
  - Product not in production yet, migration code not needed
  - Simplified codebase (~340 lines removed)

---

## [1.4.10] - 2026-01-20

### Fixed
- **HR Min/Max Display Bug**: Fixed incorrect element IDs in WebSocket handler
  - Changed `hr-min-display`/`hr-max-display` to `hr-min-value`/`hr-max-value`
  - HR Min/Max values now correctly displayed on page load
- **WebSocket HEART_RATE_SETTINGS Frame**: Fixed frame not being sent on connect
  - `markDirty()` and `flushFrame()` now support Frame ID 11 (was limited to 1-10)
  - `HEART_RATE_SETTINGS` frame is now properly marked dirty and sent on WebSocket connect
  - HR Min/Max values are now automatically sent to clients on connection

### Changed
- **Code Optimization**: Removed all debug console output from production build
  - Removed 76 console.log/warn/error calls
  - Removed build info console log
  - Reduced app.js size: 186,288 bytes → 179,682 bytes raw (~6.6 KB reduction)
  - Reduced gzipped size: 43,338 bytes → 41,963 bytes (~1.4 KB reduction)
- **Code Cleanup**: Removed obsolete commented Arduino example code
  - Removed 18 lines of outdated HTTP-based firmware update example code
  - Code now reflects WebSocket-based implementation

---

## [1.4.9] - 2026-01-20

### Changed
- **WebUI Build**: Updated build hash and regenerated PROGMEM headers
  - Build hash: `519e09d9` (updated from `386ebdd9`)
  - Optimized app.js size: 186,288 bytes raw (reduced from 187,444 bytes)
  - All WebUI assets recompiled with latest fan animation changes

---

## [1.4.8] - 2026-01-20

### Added
- **Fan Speed Animation**: Real-time visual feedback for fan operation
  - SVG-based fan blade rotation animation
  - Animation speed corresponds to calculated fan speed (0-100%)
  - Speed calculation based on current HR and min/max HR settings
  - Linear interpolation: `speed = (currentHR - minHR) / (maxHR - minHR) * 100`
  - Animation only runs when fan toggle is ACTIVE and calculated speed > 0
  - Smooth acceleration/deceleration transitions (400°/s² accel, 300°/s² decel)
  - Maximum rotation speed: 240°/s at 100% fan speed

### Changed
- **Fan SVG**: Replaced background image with embedded SVG (`fanC.svg`)
  - New SVG design with improved visual clarity
  - ViewBox: `0 0 114 114` (optimized size)
  - Rotating elements grouped in `<g>` tag with unique IDs per fan
  - Static elements (circles, gradients) remain fixed during animation
  - Rotation center: `(57, 57)` - center of SVG viewBox

### Technical Notes
- Animation uses `requestAnimationFrame` for smooth 60fps updates
- Animation loop runs continuously while fans exist (reacts to HR/toggle changes)
- Fan speed calculated from frontend HR values (no additional WebSocket data needed)
- CSS: Removed `background: var(--fan-image-data)` from `.fan-art`
- CSS: Added styling for embedded SVG and fan blade elements
- Animation state tracked per fan (angle, speed, targetSpeed)
- Automatic animation start on fan card creation, HR changes, and toggle state changes

---

## [1.4.7] - 2026-01-20

### Changed
- **Reset Settings Modal**: Redesigned with separate reset options
  - Modal now displays 3 buttons vertically (full width)
  - **Wifi Reset**: Clears WiFi configuration and restarts device
  - **Factory Reset**: Clears all settings and restarts device
  - **Cancel**: Closes modal without action
  - Buttons use danger styling for reset actions, tertiary for cancel

### Added
- **Wifi Reset Functionality**: New API endpoint `/api/wifi-reset`
  - Resets WiFi configuration only
  - Device restarts after reset
  - Reboot monitoring waits for device to come back online
- **Factory Reset Functionality**: New API endpoint `/api/factory-reset`
  - Resets all settings (WiFi + stored settings)
  - Device restarts after reset
  - Reboot monitoring waits for device to come back online
- **Reboot Monitoring**: Automatic device restart detection
  - Polls `/api/v1/system/info` every 2 seconds after reset
  - Maximum wait time: 60 seconds (30 attempts)
  - On success: Closes modal and reloads UI automatically
  - On timeout: Shows manual reload option
  - Similar behavior to firmware update restart monitoring

### Technical Notes
- Reset operations now show loading modal during API call
- Success modal shows while device restarts in background
- UI automatically reloads when device is back online
- CSS: Added `.app-modal-footer-vertical` class for vertical button layout
- Reset buttons are disabled during reset operation (`actions: false`)

---

## [1.4.6] - 2026-01-19

### Fixed
- **BLERelayManager Heap Fragmentation**: Critical fixes for background heap drift caused by BLE subsystem
  
  **Root Cause Analysis**:
  - With `HW_ENABLE_BLE=0`: Zero heap drift over 30+ minutes
  - With `HW_ENABLE_BLE=1`: ~3-7KB drift over 10-15 minutes even without connected sensors
  - Problem traced to `SensorConfig` using `String` members, not the BluetoothBikeSensorServer library
  
  **Fix 1: SensorConfig converted to fixed buffers**
  - `SensorConfig.name`: `String` → `char[64]` (BLE_SENSOR_NAME_MAX)
  - `SensorConfig.type`: `String` → `char[4]` ("HR", "CSC", "PWR")
  - `SensorConfig.mac`: `String` → `char[18]` (MAC address format)
  - Added `setName()`, `setType()`, `setMac()` helper methods
  
  **Fix 2: _discoveryType converted to fixed buffer**
  - `String _discoveryType` → `char _discoveryType[4]`
  - Eliminates heap allocation during discovery start/stop cycles
  
  **Fix 3: MAC update optimization**
  - Only call `setMac()` + `saveConfigToNVS()` when MAC actually changed
  - Previously: saved to NVS on EVERY connect, even with same MAC
  - Reduces heap churn from String operations and NVS writes
  
  **Code changes**:
  - All `.c_str()` calls replaced with direct `char[]` access
  - All `.length()` calls replaced with `strlen()`
  - All `== name` comparisons replaced with `strcmp(..., name) == 0`
  - Constructor initializes `_discoveryType[0] = '\0'`

### Technical Notes
- BLE init cost: ~65KB (expected, NimBLE stack)
- Background drift with fixes: should be <500 bytes/hour
- String operations were causing repeated heap fragmentation in:
  - `setConfiguredSensor()` called on discovery add
  - `onSensorConnected()` MAC update path
  - `loadConfigFromNVS()` / `saveConfigToNVS()` paths

---

## [1.4.5] - 2026-01-19

### Fixed
- **WebSocket Heap Fragmentation**: Major heap stability improvements for WS connect/disconnect cycles
  - **Root Cause**: WS reconnects caused TCP buffer allocations that fragmented heap
    - First WS connect: `largest` dropped ~4KB (65,524 → 55,284)
    - Third WS connect: `largest` dropped ~10KB, frag jumped 39% → 48%
  
  - **Fix 1: loopFlush() early return when no clients**
    - When `ws_clients == 0`: skip all markDirty, clear pending dirtyFlags
    - Heap can "rest" when no one is watching
  
  - **Fix 2: Staged connect - spread heavy snapshots**
    - On connect: only send SYSTEM_STATUS + FIRMWARE_VERSION immediately
    - SENSORS_SNAPSHOT: delayed 150ms
    - FANS_SNAPSHOT: delayed 300ms
    - Reduces TCP buffer allocation burst
  
  - **Fix 3: markDirty() skips when no clients**
    - External code (BLERelayManager, FanController) calling `markDirty()` is ignored when no WS clients
    - Prevents unnecessary state tracking

### Technical Analysis (from heap diagnostics log)
```
Phase A (WS=0):     drift=1.3KB over 2min → normal settling
Phase B (WS=1):     largest=65,524 → frag=41%
Phase C (reconnect): largest=55,284 → frag=48% ← FRAGMENTATION!
```
- Discovery=0, dirty=0x0000 during drift → confirmed: not BLE, not Discovery
- Problem is WS/TCP buffering during connect + snapshot building

---

## [1.4.2] - 2026-01-18

### Changed
- **Heap Fragmentation Fixes**: Converted dynamic `String` fields to fixed-size `char[]` buffers
  - `FanConfig.token`: `String` → `char[33]` (32 hex + null)
  - `DiscoveredSensor`: `String name/type/mac` → `char name[64]/type[4]/mac[18]`
  - `SensorInfo`: `String name/type/mac` → `char name[64]/type[4]/mac[18]`
  - Added helper methods `setName()`, `setType()`, `setMac()`, `tokenEquals()` for safe buffer operations

### Fixed
- **Memory Fragmentation**: Eliminated repeated heap allocations in BLE Discovery and Telemetry hot paths
  - Previously: Each discovery cycle created variable-size String objects → heap fragmentation
  - Now: Fixed buffers with `strncpy()` → zero heap allocations in telemetry loops
  - Expected result: `ESP.getMaxAllocHeap()` stays stable over time
  
- **Header Definition Order**: Moved `BLE_SENSOR_NAME_MAX` constant before struct definitions that use it
  - Fixes compilation error: `'BLE_SENSOR_NAME_MAX' was not declared in this scope`

### Technical Notes
- Discovery/Snapshot paths now use direct buffer writes instead of String operations
- `FanConfig` comparison uses `strcasecmp()` for case-insensitive token matching
- Internal `SensorConfig` in BLERelayManager still uses String (low-frequency, boot-time only)

---

## [1.4.4] - 2026-01-19

### Added
- **HeapDiagnostics Marker System**: Clear separation of Boot vs. Background-Init vs. Runtime drift
  - **Markers (one-time):**
    - `BOOT_RAW` - Very start of setup() after Serial
    - `READY_BASELINE` - Entry to PH_RUNNING, before background init
    - `BLE_READY` - After BLERelayManager fully initialized
    - `FANS_CONNECTED` - After SmartConnect + Handshake completes
    - `POST_CTRL_DONE` - After POST-CONNECTION CONTROL SEQUENCE COMPLETE
    - `FIRST_WS_CLIENT` - First WebSocket client connected
  - **Marker (continuous):**
    - `RUNTIME_TICK` - Every 30s (fixed interval, not triggered by heap changes)
  - **Per-marker data:** `free_8bit`, `largest_8bit`, `free_internal`, `largest_internal`, `frag%`, `uptime_ms`
  - **Operational Status in RUNTIME_TICK:**
    - WebSocket client count
    - BLE sensors connected/configured
    - Discovery results count
    - Fans connected/configured
  - **Delta Analysis:**
    - Boot cost: `READY_BASELINE - BOOT_RAW`
    - BLE init cost: `BLE_READY - READY_BASELINE`
    - Fans init cost: `FANS_CONNECTED - BLE_READY`
    - Background init total: `FANS_CONNECTED - READY_BASELINE`
    - Runtime drift: `CURRENT - FANS_CONNECTED`

- **BLERelayManager getters:** `getConfiguredSensorCount()`, `getConnectedSensorCount()`, `getLastDiscoveryCount()`
- **FanController getter:** `getConnectedFanCount()`
- **WebSocketTelemetry getter:** `getClientCount()`

### Changed
- **Serial 'h' Command**: Now displays all markers in a table format with delta analysis
- **All marker logs**: Now include timestamp `@HH:MM:SS`
- **Config.h**: Added `HW_HEAP_DIAG_TICK_INTERVAL_MS` (30s, default)
- **FanController.cpp**: Calls `hwHeapDiagCaptureFansConnected()` and `hwHeapDiagCapturePostCtrlDone()`
- **App.cpp**: Calls `hwHeapDiagCaptureBleReady()` after BLE initialization; runtime tick moved here

### Technical Notes
- Each marker fires only once (bool guard)
- `RUNTIME_TICK` only logs if: time threshold exceeded OR heap changed significantly
- Internal RAM metrics via `heap_caps_get_free_size(MALLOC_CAP_INTERNAL)`
- No String usage, no dynamic allocations

---

## [1.4.3] - 2026-01-19

### Added
- **Telemetry Diagnostics (compile-time)**: Minimal-invasive debug signals from `WebSocketTelemetry::loopFlush()`
  - Per flush (when frames are sent): `FreeHeap`, `LargestBlock`, WebSocket client count
  - Every 60s: `dirtyFlags` (hex) and a bitmask of frames actually built in the interval
  - No `String`, no dynamic allocation; gated by `HW_ENABLE_TELEMETRY_DIAG`
  - Optional frame-name list via `HW_TELEMETRY_DIAG_PRINT_FRAME_LIST`

---

## [1.4.1] - 2026-01-18

### Added
- **Serial Debug Commands**: Type commands via Serial Monitor for runtime diagnostics
  - `h` - Heap info (Initial, Free, Used since boot, Fragmentation%)
  - `s` - System status (Version, Uptime, WiFi state)
  - `?` - Help
  - Controlled via `HW_ENABLE_SERIAL_COMMANDS` in Config.h (default: enabled)
  
- **Boot Heap Tracking**: Captures initial heap at boot for memory leak detection
  - "Used since boot" shows delta between initial and current heap
  - Helps identify gradual memory leaks over time

### Changed
- **WebSocket SYSTEM_STATUS Frame**: Removed WiFi RSSI and wifiConnected fields
  - Frame size reduced from 14 to 12 bytes
  - These fields were sent but never displayed in UI
  - Serial command `h` provides better heap diagnostics

### Fixed
- **GitFirmwareUpdate Logging**: Simplified error logging (removed WiFi.RSSI, kept FreeHeap)
  - Removed dependency on WiFi.h in GitFirmwareUpdate library

---

## [1.4.0] - 2026-01-18

### Added
- **Dual WiFi Backend Architecture**: Compile-time selection between two WiFi modes
  - `HW_DEV_MODE=1` (Default): Fixed credentials via `WiFiService` (fast dev builds)
  - `HW_DEV_MODE=0` (Production): Captive Portal via `wifiMangerLite` (user provisioning)
  
- **WiFiServiceWML**: New WiFi backend using `wifiMangerLite` library
  - Captive Portal at `/wml/setup` for WiFi configuration
  - NVS-based credential storage (no hardcoded SSID/passwords)
  - Automatic AP mode on first boot or missing credentials
  - Event-driven state management
  
- **Two-Level Connectivity Model**:
  - **Level A (Setup ready)**: AP *or* STA → WebUI accessible
  - **Level B (Operational)**: STA only → BLE/Fans/OTA enabled
  - `SystemState::isStationOnline()` for Level B checks
  
- **Smart Root Dispatch**: `/` redirects to Portal setup in AP-only mode, shows Homewind UI when STA connected

### Changed
- **Config.h**: New `HW_DEV_MODE`, `HW_WIFI_BACKEND_FIXED`, `HW_WIFI_BACKEND_WML` flags
- **App.cpp**: Deferred BLE/Fans/OTA initialization until STA connected (Level B gate)
- **WebServer.cpp**: Root route dispatch based on connection state
- **SystemState**: Added `setStationOnline()`/`isStationOnline()` for Level B tracking

### Fixed
- **WiFi Pre-Init for WebServer**: `WiFi.mode(WIFI_AP_STA)` must be called before `ESPAsyncWebServer` starts
  - Without this, FreeRTOS semaphore assertions fail (`xQueueSemaphoreTake`)
  - WML backend now pre-initializes WiFi hardware in `initWiFi()` before WebServer creation

### Security
- **No hardcoded credentials in Production**: Credentials stored in NVS, set via Captive Portal

### Technical Notes
- **Boot Order (WML)**: WiFi hardware init → WebServer → WiFiManagerLite/Portal
- **Feature Gates**: `WiFiService` only compiled with `HW_WIFI_BACKEND_FIXED`, `WiFiServiceWML` only with `HW_WIFI_BACKEND_WML`

---

## [1.3.3] - 2026-01-16

### Fixed
- **ESP32-OTA UI Flow**: Fixed firmware update error screen appearing after successful download
  - ESP32-OTA has no separate "Install" step - installation happens during download, device reboots automatically
  - Removed unnecessary `startFirmwareInstallation()` API call that caused 400 errors
  - State flow now: `DOWNLOADING` → `RESTARTING` (skips `INSTALLING`)
  - Added `startRebootMonitoringForRestart()` to detect device reboot and auto-reload page

### Changed
- **State Machine**: `handleDownloadProgress(100%)` now triggers `openRestarting()` instead of `openInstallation()`
- **Reboot Detection**: Polls `/api/v1/system/info` every 2s after download, auto-reloads when device responds
- **Timeout Handling**: Shows "Reload Page" button after 60s if device doesn't respond

---

## [1.3.2] - 2026-01-16

### Changed
- **API Consolidation**: Firmware actions now use `ApiV1` (Single Source of Truth)
  - `window.searchFirmware()` etc. are now aliases to `ApiV1.firmware*()`
  - Removed redundant `FW_API` endpoint definitions (except `status` for reboot detection)
  - Direct callback routing: WS frames → `FirmwareUpdate.handle*()` (no intermediate callbacks)
  - Legacy `window.onFirmware*` callbacks marked as `@deprecated`
  
- **WebSocket Reconnect Protection**: Prevents reconnect-storms
  - `connectWS()` now checks if `reconnectTimer` is active before connecting
  - `visibilitychange` handler only reconnects if `ws.readyState === CLOSED`
  - **Impact:** Stable behavior when tab is backgrounded/foregrounded repeatedly

### Improved
- **app.js size reduced**: -7.7 KB raw since 1.3.0 (167 KB → 160 KB)

---

## [1.3.1] - 2026-01-16

### Fixed
- **Firmware Update `hasUpdate` Bug**: WebSocket incorrectly sent `hasUpdate: true` when remote version was older than current version (based on `remoteVersion.length() > 0` instead of state)
- **Firmware Version Not Displayed in UI**: `buildFirmwareVersion()` was a stub returning 0, version was never sent to WebUI. Now sends `BuildInfo::getVersion()` on WebSocket connect
- **Firmware Update URL Typo**: Fixed `lastest.json` → `latest.json` in Config.h

### Changed
- **WebSocket Initial State**: `FIRMWARE_VERSION` frame now included in initial snapshot on client connect
- **Firmware Update Architecture**: Eliminated HTTP polling for search/download progress
  - Removed `pollSearchStatus()` and `pollDownloadProgress()` (~130 lines)
  - `FIRMWARE_PROGRESS` frame now sent via WebSocket during download
  - `buildFirmwareProgress()` implemented (was stub)
  - **Impact:** -6KB app.js, no more 500ms HTTP polling, reduced ESP32 load

---

## [1.3.0] - 2026-01-12

### Changed
- **Static Subsystem Instances (Stufe 3)**: All subsystems now use static allocation instead of `new`
  - SettingsStore, WiFiService, WebServerManager, WebSocketTelemetry
  - BLERelayManager, FanController, FanHRAdapter, HeartRateSettings, FirmwareUpdateManager
  - **Impact:** Deterministic memory layout, zero heap fragmentation from subsystem lifecycle
  
- **Consistent ready() API**: All subsystem classes now have `ready()` method for graceful degradation
  - SettingsStore, WebServerManager, WebSocketTelemetry, HeartRateSettings

- **Request Handler Heap Optimization**: Eliminated `substring()` allocations in fan ID resolution
  - Uses `strncasecmp()` for 8-char ID matching (no temporary String objects)

### Documentation
- Updated App.cpp with memory model documentation

---

## [1.2.1] - 2026-01-12

### Fixed
- **Fan Card Duplicate Bug**: Fixed race condition where adding a fan could create duplicate cards when WebSocket FANS_SNAPSHOT arrived before HTTP response completed
- **Consistent ERROR State on Add**: New fans now correctly show ERROR state until discovery and handshake complete (was incorrectly showing INACTIVE in some timing scenarios)
- **Consistent ERROR State on Restart**: Fans now show ERROR state after ESP32 restart until Smart Connect completes (was incorrectly showing INACTIVE due to cached IP)
- **Stale Discovered State**: Fixed issue where deleted and re-added fans would show ACTIVE state from stale discovered list entries

### Changed
- **HTTP 500 for Fan Control Errors**: Fan control endpoint now returns HTTP 500 (instead of misleading 429) when fan is not yet discovered/connected
- **Fan State Logic**: Fans with IP 0.0.0.0 (freshly added) now always show ERROR state, ignoring stale discovered list entries

### Documentation
- Added Fan API endpoints (Add, Update, Control) with status codes and throttling behavior
- Added FANS_SNAPSHOT state interpretation table and state transitions
- Documented ERROR/INACTIVE/ACTIVE state meanings

---

## [1.2.0] - 2026-01-11

### Changed
- **Deterministic NVS Storage (Stufe 2)**
  - Replaced `DynamicJsonDocument` with struct-based binary blobs
  - CRC16-CCITT checksums for data integrity validation
  - Zero heap allocation during load/save operations
  - Automatic migration from legacy NVSConfigBus MessagePack format
  - **Impact:** Eliminated heap fragmentation from NVS operations, faster boot

- **HeartRateSettings**
  - New `NVSData` struct with version, min/max HR, reserved fields, CRC16
  - Size: 10 bytes fixed (vs. dynamic JSON)
  - Migration preserves existing settings automatically

- **FanController**
  - New `NVSFanData` struct with version, fanCount, postConnectSpeed, fan entries
  - `NVSFanEntry` per fan: token (33 bytes), IP (4 bytes), DID, model, enabled
  - Total size: 270 bytes fixed for up to 4 fans
  - Migration parses legacy MessagePack tokens

- **BLERelayManager**
  - New `NVSSensorData` struct with version, sensorCount, sensor entries
  - `NVSSensorEntry` per sensor: name (64 bytes), type, MAC
  - Total size: 264 bytes fixed for 3 sensors (HR, CSC, PWR)
  - Migration parses legacy MessagePack sensor configs

### Removed
- `NVSConfigBus` dependency removed from library.properties
- `DynamicJsonDocument` members removed from HeartRateSettings, FanController, BLERelayManager
- `_msgPackBuf` fixed buffers no longer needed

---

## [1.1.0] - 2026-01-11

### Changed
- **FanController: Non-blocking Post-Connection Control**
  - Replaced blocking `delay(500)` + `delay(3000)` with millis-based state machine
  - New `PostConnectStep` enum for sequence: POWER_ON → WAIT_STABILIZE → SET_SPEED → WAIT_RUN → POWER_OFF
  - **Impact:** Eliminates 3500ms loop blockade, WebUI/BLE/WebSocket remain responsive during fan connection
  - Removed blocking `delay(10)` from discovery cancel operations

### Improved
- **String Operations in Hot Paths (Heap Optimization)**
  - `ApiActions::getBoolParam()` - Manual case-insensitive comparison without `toLowerCase()`/`trim()`
  - `ApiActions::getIntParam()` - Manual integer parsing without temporary String allocation
  - `ApiActions::validateSensorType()` - Direct pointer comparison instead of String copy
  - `ApiActions::handleMaintenanceBegin()` - Optimized reason string parsing
  - `BLERelayManager::stringToSensorType()` - `strncasecmp()` instead of `String::toLowerCase()`
  - **Impact:** Reduced heap allocations per HTTP request by ~60-80%

### Fixed
- Potential WebSocket/BLE disconnects during fan Smart Connect due to loop starvation

---

## [1.0.0] - 2026-01-11

### Added
- **Core Framework**
  - Modular architecture with compile-time feature gates
  - Deterministic boot orchestrator with phase-based initialization
  - SystemState namespace for global state management

- **WiFi Service**
  - STA mode with configurable timeout
  - AP fallback mode for configuration
  - Non-blocking connection handling

- **Web Server**
  - ESPAsyncWebServer integration
  - Static asset serving from PROGMEM (zero RAM usage)
  - ETag support for browser caching

- **WebSocket Telemetry**
  - Binary protocol for efficient data transfer
  - Snapshot/delta frame model
  - Dirty-flag + loop-flush pattern for thread safety

- **BLE Relay Manager**
  - Heart rate, speed/cadence, and power meter sensor support
  - Single-writer reconcile pattern for connection management
  - Sensor discovery with configurable duration

- **Fan Controller**
  - SmartMiFanAsync integration
  - Smart Connect for fast startup
  - NVS persistence for fan configurations
  - HR-driven fan control via FanHRAdapter

- **Firmware Updates**
  - OTA update state machine
  - GitHub release integration
  - Progress tracking and callbacks

- **Settings**
  - NVS-based persistent storage
  - Heart rate min/max configuration
  - Post-connect speed configuration

- **HTTP API**
  - RESTful action endpoints
  - Settings API
  - Maintenance mode gate

- **WebUI**
  - Real-time telemetry display
  - Sensor management
  - Fan control interface
  - Firmware update interface

### Documentation
- Comprehensive docs/ folder with architecture, APIs, and development guides
- WebSocket protocol specification
- HTTP endpoint documentation

---

## Version History

| Version | Date       | Description |
|---------|------------|-------------|
| 1.3.2   | 2026-01-16 | API consolidation, WS reconnect protection, -7.7 KB app.js |
| 1.3.1   | 2026-01-16 | Bug fixes: Firmware update detection, version display, URL typo |
| 1.3.0   | 2026-01-12 | Static subsystem instances: deterministic memory, no heap fragmentation |
| 1.2.1   | 2026-01-12 | Bug fixes: Fan card duplicates, consistent ERROR state, stale discovered state |
| 1.2.0   | 2026-01-11 | Deterministic NVS storage: struct+CRC16 blobs, zero heap allocations |
| 1.1.0   | 2026-01-11 | Performance optimizations: non-blocking fan control, string heap reduction |
| 1.0.0   | 2026-01-11 | Initial stable release |

---

## Upgrade Guide

### From pre-1.0 versions
This is the first stable release. If upgrading from development versions:
1. Backup your NVS data
2. Flash the new firmware
3. Reconfigure sensors and fans if needed
