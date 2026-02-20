# Homewind

[![License](https://img.shields.io/badge/license-PU%2BCU-blue.svg)](LICENSE)

ESP32 WebUI library with WebSocket telemetry and HTTP API (modular architecture).

## Status

**Current Status**: Core features implemented - BLE sensor relay, web UI, firmware updates, and telemetry.

All features are disabled by default via feature gates. Enable features in `src/app/Config.h` as needed.

## Installation

### Arduino IDE

1. **Place library in Arduino libraries folder**:
   ```bash
   # Copy entire Homewind folder to:
   ~/Documents/Arduino/libraries/Homewind/
   ```

2. **Open Arduino IDE**

3. **Select Board**: 
   - Tools → Board → ESP32 Arduino → ESP32-S3 Dev Module (or your ESP32 variant)
   - Tools → CPU Frequency → 240MHz (recommended)
   - Tools → Partition Scheme → Default 4MB with spiffs (or your preference)

4. **Select Port**: Tools → Port → (your ESP32 port)

5. **Compile**: Sketch → Verify/Compile (Ctrl+R / Cmd+R)

### Expected Output

On successful compilation, you should see:
```
=== Homewind Starting ===
[App] Initializing Homewind v1.4.33
[App] Initializing subsystems...
[App]  - Settings store: DISABLED
[App]  - Web server: DISABLED
[App]  - WebSocket telemetry: DISABLED
[App] Subsystem initialization complete
[App] Initialization complete
[INFO] Homewind initialized successfully
```

## Feature Gates

All features are disabled by default. To enable features, edit `src/app/Config.h`:

```cpp
// Enable web server (requires ESPAsyncWebServer library)
#define HOMEWIND_ENABLE_WEBSERVER 1

// Enable WebSocket telemetry (requires WebSocketsServer library)
#define HOMEWIND_ENABLE_WEBSOCKET 1

// Enable settings store (NVS)
#define HOMEWIND_ENABLE_SETTINGS 1

// Enable API handlers
#define HOMEWIND_ENABLE_API_ACTIONS 1
#define HOMEWIND_ENABLE_API_SETTINGS 1
```

**Important**: Define these **before** including `Config.h` or edit `Config.h` directly.

## Dependencies

### Required (via library.properties)

- **ESPAsyncWebServer** – Web server and AsyncWebSocket (for WebUI, API, telemetry)
- **ArduinoJson** – JSON parsing for API and settings
- **BluetoothBikeSensorServer** – BLE sensor relay (HR, CSC)
- **SmartMiFanAsync** – SmartMi fan control
- **GitFirmwareUpdate** – OTA firmware updates

**Note:** Homewind uses `AsyncWebSocket` (part of ESPAsyncWebServer), not `arduinoWebSockets`.

### Conditional (depending on Config.h)

- **wifiMangerLite** – Required when `HW_WIFI_BACKEND_WML=1` (PROD mode, Captive Portal). Default when `HW_DEV_MODE=0`.
  - Install: Place in `libraries/wifiMangerLite/`
  - Dependency: wifiMangerLite requires **NVSUtilityLibrary**

- **NVSUtilityLibrary** – Required when using wifiMangerLite (NVS storage for WiFi credentials and BLE sensor persistence)
  - Install: Place in `libraries/NVSUtilityLibrary/`

- **HomeWindWSAmoled** – Required when `HW_ENABLE_DISPLAY=1` (Touch board with AMOLED)
  - Install: Place in `libraries/HomeWindWSAmoled/`
  - Set `HW_ENABLE_DISPLAY=0` in Config.h for headless/Basic builds (no display library needed)

## Project Structure

```
Homewind/
├── Homewind.ino              # Main sketch (wiring only)
├── library.properties        # Arduino library metadata
├── README.md                 # This file
├── webui_src/                # WebUI source files (HTML, CSS, JS)
│   ├── index.html            # Main HTML file
│   ├── app.css               # Stylesheet
│   └── app.js                # JavaScript application
├── tools/
│   └── build_webui.py        # WebUI build script (generates PROGMEM headers)
├── generated/                # Auto-generated files (do not edit)
│   ├── web_assets.h          # Main asset include
│   ├── web_assets_manifest.h # Asset manifest with cache info
│   └── webui_*.h             # Individual asset headers
└── src/
    ├── app/
    │   ├── App.h/cpp         # Main application coordinator
    │   ├── BuildInfo.h       # Version/build metadata
    │   └── Config.h          # Feature gates and compile-time config
    ├── core/
    │   ├── SystemState.h/cpp # Global system state
    │   └── Timing.h          # Non-blocking timing utilities
    ├── settings/
    │   └── SettingsStore.h/cpp # Settings storage (stub)
    ├── wifi/
    │   └── WiFiService.h/cpp  # WiFi connection management
    └── web/
        ├── WebServer.h/cpp      # Web server manager (stub)
        ├── WebSocketTelemetry.h/cpp # WebSocket telemetry (stub)
        ├── ApiActions.h/cpp     # HTTP action handlers (stub)
        └── ApiSettings.h/cpp    # HTTP settings handlers (stub)
```

## WebUI Caching & Cache-Busting

The WebUI implements a comprehensive caching strategy to optimize performance while ensuring browsers always fetch updated assets after firmware updates.

### Automatic Cache-Busting

The build system automatically generates a deterministic build hash from all WebUI assets. This hash is:

- **Calculated** from the content of all assets (HTML, CSS, JS)
- **Embedded** in asset URLs as a query parameter: `app.js?v=HASH` and `app.css?v=HASH`
- **Updated** automatically whenever any WebUI asset changes
- **Deterministic** - same assets produce the same hash (reproducible builds)

**Example:**
```html
<link rel="stylesheet" href="app.css?v=45fa2275">
<script src="app.js?v=45fa2275" defer></script>
```

When you modify any WebUI file and rebuild, the hash changes, forcing browsers to download fresh assets. This solves Safari cache issues without manual versioning.

### HTTP Caching Headers

The WebServer sends appropriate cache headers for each asset type:

| Asset Type | Cache Duration | Reason |
|------------|----------------|--------|
| `index.html` | 1 hour (3600s) | May change frequently, needs regular revalidation |
| `app.css` | 1 day (86400s) | Changes less frequently, hash in URL ensures updates |
| `app.js` | 1 day (86400s) | Changes less frequently, hash in URL ensures updates |

**Headers sent:**
- `ETag: <hash>` - Content-based validation
- `Cache-Control: public, max-age=<seconds>` - Cache duration
- `Content-Encoding: gzip` - Compression indicator (when applicable)

### ETag Support

The server supports ETag-based conditional requests:

- **First request**: Server sends `200 OK` with `ETag` header
- **Subsequent requests**: Browser sends `If-None-Match: <etag>`
- **Unchanged**: Server responds `304 Not Modified` (no body, saves bandwidth)
- **Changed**: Server sends `200 OK` with new content and new `ETag`

### How It Works

1. **Build Time** (`tools/build_webui.py`):
   - Processes all WebUI assets (HTML, CSS, JS)
   - Calculates hash from all asset content
   - Replaces `__WEBUI_HASH__` placeholder in HTML with actual hash
   - Generates `HOMEWIND_WEBUI_BUILD_HASH` macro in `web_assets_manifest.h`
   - Creates ETags for each asset

2. **Runtime** (WebServer):
   - Serves assets with appropriate `Cache-Control` headers
   - Handles `If-None-Match` requests (ETag validation)
   - Returns `304 Not Modified` when content unchanged
   - Query parameters (`?v=HASH`) are ignored in asset lookup (default behavior)

3. **Browser**:
   - Caches assets according to `Cache-Control` headers
   - Uses hash in URL to detect changes
   - Sends conditional requests with `If-None-Match` for bandwidth savings

### Benefits

- ✅ **No manual versioning** - Hash updates automatically
- ✅ **Safari-compatible** - URL-based cache-busting works reliably
- ✅ **Bandwidth efficient** - ETag validation reduces unnecessary transfers
- ✅ **Deterministic builds** - Same input produces same hash
- ✅ **No filesystem required** - All assets embedded in PROGMEM
- ✅ **No runtime overhead** - Hash calculated at build time

## Compilation Checklist

- [x] **Board**: ESP32 (any variant - ESP32, ESP32-S2, ESP32-S3, ESP32-C3)
- [x] **Platform**: ESP32 Arduino Core (installed via Board Manager)
- [ ] **ESPAsyncWebServer**: Optional (only if enabling web server)
- [ ] **WebSockets**: Optional (only if enabling WebSocket telemetry)
- [x] **File System**: Not used (all code in PROGMEM/flash)

## Troubleshooting

### Compilation Errors

**"ESP32 platform not found"**:
- Install ESP32 Arduino Core via: Tools → Board → Boards Manager → search "ESP32"

**"WebServer.h: No such file or directory"**:
- Expected if `HOMEWIND_ENABLE_WEBSERVER=1` but ESPAsyncWebServer not installed
- Either install ESPAsyncWebServer or disable the feature in `Config.h`

**"WebSocketsServer.h: No such file or directory"**:
- Expected if `HOMEWIND_ENABLE_WEBSOCKET=1` but WebSockets not installed
- Either install WebSockets library or disable the feature in `Config.h`

### Runtime Issues

**Serial output shows "DISABLED" for all subsystems**:
- Normal behavior - all features are disabled by default
- Enable features in `Config.h` as needed

## Next Steps

1. update Zwift Click

## License

Free for private, non-commercial use. Commercial use requires a separate license. See [LICENSE](LICENSE) for details.

## Contributing

[Contributing guidelines]

