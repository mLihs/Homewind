# Development Guide

This document provides instructions for building, configuring, and extending the Homewind library.

---

## Prerequisites

### Required
- **Arduino IDE** (1.8.x or 2.x) or **PlatformIO**
- **ESP32 Arduino Core** - Install via Board Manager:
  - Tools → Board → Boards Manager → search "ESP32"
  - Install "esp32" by Espressif Systems

### Optional (Feature-Gated)
- **ESPAsyncWebServer** - For web server (if `HW_ENABLE_WEB == 1`)
  - Tools → Manage Libraries → search "ESPAsyncWebServer"
  - Or: https://github.com/me-no-dev/ESPAsyncWebServer
- **AsyncTCP** - Required for ESP32 + ESPAsyncWebServer
  - Automatically installed with ESPAsyncWebServer
- **WebSockets** - For WebSocket support (if `HOMEWIND_ENABLE_WEBSOCKET == 1`)
  - Tools → Manage Libraries → search "WebSockets"
  - Or: https://github.com/Links2004/arduinoWebSockets
- **GitFirmwareUpdate** - For OTA updates (if `HW_ENABLE_OTA == 1`)
  - Install from: `../GitFirmwareUpdate/` (local library)

---

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

### PlatformIO

1. **Create project**:
   ```ini
   [env:esp32-s3-devkitc-1]
   platform = espressif32
   board = esp32-s3-devkitc-1
   framework = arduino
   lib_deps = 
       me-no-dev/ESPAsyncWebServer@^2.3.0
       links2004/WebSockets@^2.4.1
   lib_extra_dirs = 
       ../libraries
   ```

2. **Build**: `pio run`

---

## Configuration

### Feature Gates

Edit `src/app/Config.h` to enable/disable features:

```cpp
// Core services
#define HW_ENABLE_NVS 1      // Settings store
#define HW_ENABLE_WIFI 1     // WiFi service
#define HW_ENABLE_WEB 1      // Web server

// Background services
#define HW_ENABLE_BLE 0      // BLE relay (optional)
#define HW_ENABLE_FANS 0     // Fan controller (optional)
#define HW_ENABLE_OTA 1      // Firmware updates

// Web features
#define HOMEWIND_ENABLE_WEBSOCKET 1      // WebSocket telemetry
#define HOMEWIND_ENABLE_API_ACTIONS 1    // Action API
#define HOMEWIND_ENABLE_API_SETTINGS 1   // Settings API
```

### WiFi Configuration

Edit `src/app/Config.h`:

```cpp
// WiFi STA credentials (compile-time)
#define HW_WIFI_SSID "YourWiFiSSID"
#define HW_WIFI_PASS "YourWiFiPassword"

// WiFi AP fallback credentials
#define HW_WIFI_AP_SSID "Homewind"
#define HW_WIFI_AP_PASS "homewind123"

// WiFi timeout (milliseconds)
#define HW_WIFI_TIMEOUT_MS 20000  // 20 seconds

// AP fallback mode
#define HW_WIFI_AP_FALLBACK 1     // 1 = enabled, 0 = disabled
```

### Firmware Update Configuration

Edit `src/app/Config.h`:

```cpp
// Firmware update GitHub URL (raw JSON file)
#define HW_FIRMWARE_GITHUB_URL "https://raw.githubusercontent.com/user/repo/main/latest.json"
```

**GitHub JSON Format**:
```json
{
  "version": "1.0.0",
  "url": "https://github.com/user/repo/releases/download/v1.0.0/firmware.bin",
  "notes": "Release notes..."
}
```

### Build Configuration

Edit `src/app/BuildInfo.h`:

```cpp
#define FW_NAME "Homewind"
#define FW_VERSION "1.0.0"
#define FW_BUILD_ID "dev"  // Or use git commit hash
```

### Debug Logging

Edit `src/app/Config.h`:

```cpp
#define HW_ENABLE_DEBUG_LOGS 1  // 1 = enabled, 0 = disabled
```

**Note**: Disable in production builds to reduce memory usage.

---

## Building Web UI

The web UI (HTML, CSS, JS) is embedded in PROGMEM via auto-generated headers.

### Automatic Build

The build system automatically generates headers from `webui_src/`:
- `tools/build_webui.py` - Build script
- `generated/webui_*.h` - Auto-generated headers

**Build Process**:
1. Processes `webui_src/index.html`, `app.css`, `app.js`
2. Calculates content hash for cache-busting
3. Generates PROGMEM headers with ETags
4. Embeds assets in flash (no RAM usage)

### Manual Build

```bash
cd /path/to/Homewind
python3 tools/build_webui.py
```

### Watch Mode (Development)

```bash
python3 tools/watch_webui.py
```

Automatically rebuilds headers when web UI files change.

---

## Extending the Library

### Adding a New Feature Module

#### 1. Define Feature Gate

Edit `src/app/Config.h`:

```cpp
#ifndef HW_ENABLE_MY_FEATURE
#define HW_ENABLE_MY_FEATURE 0  // Default disabled
#endif
```

#### 2. Create Module

Create `src/myfeature/MyFeature.h`:

```cpp
#ifndef HOMEWIND_MY_FEATURE_H
#define HOMEWIND_MY_FEATURE_H

#include <Arduino.h>
#include "../app/Config.h"

#if HW_ENABLE_MY_FEATURE

class MyFeature {
public:
  MyFeature();
  ~MyFeature();
  
  bool begin();
  void loop();
  bool ready() const { return initialized; }

private:
  bool initialized;
};

#endif // HW_ENABLE_MY_FEATURE

#endif // HOMEWIND_MY_FEATURE_H
```

Create `src/myfeature/MyFeature.cpp`:

```cpp
#include "MyFeature.h"
#include "../core/DebugLog.h"

#if HW_ENABLE_MY_FEATURE

MyFeature::MyFeature()
  : initialized(false)
{
}

MyFeature::~MyFeature() {
}

bool MyFeature::begin() {
  if (initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[MyFeature] Initializing...");
  
  // Initialize feature...
  
  initialized = true;
  HW_DEBUG_PRINTLN("[MyFeature] Ready");
  return true;
}

void MyFeature::loop() {
  if (!initialized) {
    return;
  }
  
  // Update feature...
}

#endif // HW_ENABLE_MY_FEATURE
```

#### 3. Add to App

Edit `src/app/App.h`:

```cpp
#if HW_ENABLE_MY_FEATURE
class MyFeature;
#endif

class App {
private:
#if HW_ENABLE_MY_FEATURE
  MyFeature* myFeature;
  bool myFeatureReady;
#endif
};
```

Edit `src/app/App.cpp`:

```cpp
#if HW_ENABLE_MY_FEATURE
#include "../myfeature/MyFeature.h"
#endif

App::App()
  // ... other initializers
#if HW_ENABLE_MY_FEATURE
  , myFeature(nullptr)
  , myFeatureReady(false)
#endif
{
}

void App::initMyFeature() {
#if HW_ENABLE_MY_FEATURE
  if (myFeatureReady || myFeature != nullptr) {
    return;  // Already initialized
  }
  
  HW_DEBUG_PRINTLN("[App] Initializing MyFeature...");
  myFeature = new MyFeature();
  if (myFeature->begin()) {
    myFeatureReady = true;
    SystemState::setMyFeatureReady(true);  // If needed
  }
#endif
}

void App::loopSubsystems() {
  // ... other subsystems
  
#if HW_ENABLE_MY_FEATURE
  if (myFeature && myFeatureReady) {
    myFeature->loop();
  }
#endif
}
```

#### 4. Update SystemState (Optional)

If the feature needs a readiness flag:

Edit `src/core/SystemState.h`:

```cpp
#if HW_ENABLE_MY_FEATURE
  void setMyFeatureReady(bool ready);
  bool isMyFeatureReady();
#endif
```

Edit `src/core/SystemState.cpp`:

```cpp
#if HW_ENABLE_MY_FEATURE
  static bool myFeatureReady = false;
  
  void setMyFeatureReady(bool ready) {
    myFeatureReady = ready;
  }
  
  bool isMyFeatureReady() {
    return myFeatureReady;
  }
#endif
```

---

## Adding API Endpoints

### Action Endpoints

Edit `src/web/ApiActions.h`:

```cpp
static void handleMyAction(AsyncWebServerRequest* request);
```

Edit `src/web/ApiActions.cpp`:

```cpp
void ApiActions::registerRoutes(void* server) {
  // ... existing routes
  
  static_cast<AsyncWebServer*>(server)->on(
    "/api/v1/action/my/action",
    HTTP_POST,
    handleMyAction
  );
}

void ApiActions::handleMyAction(AsyncWebServerRequest* request) {
  if (!checkSystemReady()) {
    sendError(request, 503, "System not ready");
    return;
  }
  
  // Parse parameters
  String param;
  if (!getStringParam(request, "param", param, true)) {
    sendError(request, 400, "Missing required parameter: param");
    return;
  }
  
  // Process action...
  
  sendSuccess(request);
}
```

### Settings Endpoints

Edit `src/web/ApiSettings.cpp` to add new settings keys (no code changes needed if using generic API).

---

## WebSocket Frame Types

To add a new WebSocket frame type:

### 1. Define Frame Type

Edit `src/web/WebSocketTelemetry.h`:

```cpp
enum class TelemetryFrameType : uint8_t {
  // ... existing types
  MY_FRAME = 0x0A,  // Next available ID
};
```

### 2. Implement Frame Builder

Edit `src/web/WebSocketTelemetry.h`:

```cpp
size_t buildMyFrame(uint8_t* buffer, size_t bufferSize);
```

Edit `src/web/WebSocketTelemetry.cpp`:

```cpp
size_t WebSocketTelemetry::buildMyFrame(uint8_t* buffer, size_t bufferSize) {
  if (bufferSize < 4) {
    return 0;  // Buffer too small
  }
  
  size_t offset = 0;
  offset = writeUint32(buffer, offset, myValue);  // Example
  return offset;
}
```

### 3. Add to Flush Logic

Edit `src/web/WebSocketTelemetry.cpp`:

```cpp
void WebSocketTelemetry::buildAndSendFrame(TelemetryFrameType frameType) {
  size_t payloadLen = 0;
  
  switch (frameType) {
    // ... existing cases
    case TelemetryFrameType::MY_FRAME:
      payloadLen = buildMyFrame(frameBuffer, MAX_FRAME_SIZE);
      break;
  }
  
  if (payloadLen > 0) {
    sendFrame(frameType, frameBuffer, payloadLen);
  }
}
```

---

## Debugging

### Serial Output

Enable debug logging:

```cpp
#define HW_ENABLE_DEBUG_LOGS 1
```

Debug messages use `HW_DEBUG_PRINTLN()` macro (defined in `src/core/DebugLog.h`).

### Common Issues

**"System not ready" (503)**:
- Check `SystemState::isReady()` returns `true`
- Wait for boot sequence to complete (`PH_RUNNING` phase)

**WiFi timeout**:
- Check WiFi credentials in `Config.h`
- Enable AP fallback mode
- Check WiFi signal strength

**WebSocket not connecting**:
- Check WebSocket port (should be same as HTTP port, `/ws` endpoint)
- Verify `HOMEWIND_ENABLE_WEBSOCKET == 1`
- Check browser console for errors

**Firmware update fails**:
- Check `HW_FIRMWARE_GITHUB_URL` is correct
- Verify GitHub JSON format
- Check free flash space (update needs temporary space)

---

## Testing

### Unit Testing

[Unit testing framework to be determined]

### Integration Testing

Test boot sequence:
1. Power on device
2. Monitor serial output for boot phases
3. Verify all enabled services initialize

Test API endpoints:
1. Use `curl` or Postman to test endpoints
2. Verify responses and status codes
3. Test error cases (missing parameters, invalid values)

Test WebSocket:
1. Connect WebSocket client
2. Verify snapshot frames on connect
3. Trigger state changes, verify delta frames
4. Test reconnection (disconnect/reconnect)

---

## Code Style

### Naming Conventions

- **Classes**: `PascalCase` (e.g., `App`, `WebServerManager`)
- **Functions**: `camelCase` (e.g., `begin()`, `loopSubsystems()`)
- **Variables**: `camelCase` (e.g., `wifiReady`, `currentPhase`)
- **Constants**: `UPPER_SNAKE_CASE` (e.g., `HW_ENABLE_WEB`)
- **Files**: `PascalCase.h` (e.g., `App.h`, `SystemState.h`)

### Feature Gates

Always use feature gates for optional code:
```cpp
#if HW_ENABLE_MY_FEATURE
  // Optional code
#endif
```

### Memory Management

- Use static buffers where possible
- Minimize heap allocation in hot paths
- Use PROGMEM for large constants (web assets)

### Error Handling

- Return `bool` from `begin()` methods (true = success)
- Use `HW_ERROR_PRINTLN()` for errors
- Handle timeouts gracefully (don't block)

---

## Contributing

[Contributing guidelines to be added]

### Pull Request Checklist

- [ ] Code compiles (green build)
- [ ] Feature gates work (enable/disable features)
- [ ] No blocking delays in main code paths
- [ ] Documentation updated
- [ ] Tested on target hardware

---

## Related Documentation

- [01_PROJECT_OVERVIEW.md](./01_PROJECT_OVERVIEW.md) - Project overview
- [02_ARCHITECTURE.md](./02_ARCHITECTURE.md) - Architecture details
- [04_COMPONENTS.md](./04_COMPONENTS.md) - Component documentation

