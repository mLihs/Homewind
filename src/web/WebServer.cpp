/**
 * @file WebServer.cpp
 * @brief Web server manager implementation for serving static assets from PROGMEM
 * @note WebUI Build: a570ff04 - Touch this file to force recompile of generated assets
 */

#include "WebServer.h"
#include "../core/DebugLog.h"
#include "../core/SystemState.h"

#if HOMEWIND_ENABLE_WEBSERVER

// Compile-time verification - this should print if webserver is enabled
#if HOMEWIND_ENABLE_WEBSERVER == 1
// WebServer enabled - using real implementation
#else
#error "HOMEWIND_ENABLE_WEBSERVER is not 1"
#endif

// Include generated asset headers
#include "../generated/web_assets.h"
#include "../generated/web_assets_manifest.h"
#include <new>  // For placement-new

// Static buffer for AsyncWebServer (placement-new, no heap allocation)
// Aligned to ensure proper memory alignment for AsyncWebServer
alignas(AsyncWebServer) static uint8_t s_serverBuffer[sizeof(AsyncWebServer)];

WebServerManager::WebServerManager()
  : initialized(false)
  , server(nullptr)
{
}

WebServerManager::~WebServerManager() {
  if (server) {
    // Explicit destructor call for placement-new object
    server->~AsyncWebServer();
    server = nullptr;
  }
}

bool WebServerManager::begin() {
  if (initialized) {
    return true;
  }
  
  HW_DEBUG_PRINTLN("[WebServer] begin() called - creating AsyncWebServer (placement-new)");
  
  // Placement-new: construct in static buffer (no heap allocation)
  server = new (s_serverBuffer) AsyncWebServer(HOMEWIND_WEBSERVER_PORT);
  
  HW_DEBUG_PRINTLN("[WebServer] AsyncWebServer allocated (static buffer), registering routes");
  
  // Register static asset routes
  registerAssetRoutes();
  
  // Start server
  server->begin();
  
  initialized = true;
  HW_DEBUG_PRINTF("[WebServer] Initialized on port %d\n", HOMEWIND_WEBSERVER_PORT);
  HW_DEBUG_PRINTF("[WebServer] Registered %zu asset(s)\n", WEB_ASSETS_COUNT);
  
  return true;
}

void WebServerManager::loop() {
  // ESPAsyncWebServer doesn't need loop() calls, but we keep this
  // for consistency with other subsystems
  (void)0; // Suppress unused function warning
}

// Helper function to read WebAsset struct from PROGMEM
static void readAssetFromProgmem(size_t index, WebAsset* out) {
  // Read struct from PROGMEM array
  // Note: Pointers in struct point to PROGMEM data, so we just copy the pointer value
  memcpy_P(out, &WEB_ASSETS[index], sizeof(WebAsset));
}

// Helper function to find asset index by path
static size_t findAssetIndex(const char* path) {
  if (!path) {
    return (size_t)-1; // Invalid index (using -1 cast to size_t as sentinel)
  }
  
  WebAsset asset;
  for (size_t i = 0; i < WEB_ASSETS_COUNT; i++) {
    readAssetFromProgmem(i, &asset);
    
    // asset.path now points to PROGMEM string, use strcmp_P
    if (asset.path && strcmp_P(path, asset.path) == 0) {
      return i;
    }
  }
  
  return (size_t)-1; // Not found
}

const struct WebAsset* WebServerManager::findAsset(const char* path) {
  if (!path || !server) {
    return nullptr;
  }
  
  size_t index = findAssetIndex(path);
  if (index == (size_t)-1) {
    return nullptr;
  }
  
  // Return a pointer that encodes the index
  // This is a hack, but we'll decode it in handlers
  // Cast index+1 to pointer (to avoid nullptr)
  return (const WebAsset*)((uintptr_t)(index + 1));
}

bool WebServerManager::checkETag(AsyncWebServerRequest* request, const char* etag) {
  if (!request || !etag) {
    return false;
  }
  
  // Check If-None-Match header (no allocation)
  if (!request->hasHeader("If-None-Match")) {
    return false;
  }
  
  // Get header value - minimize String lifetime by using it immediately
  // ESPAsyncWebServer requires String for header access, but we use it only for comparison
  {
    String ifNoneMatchHeader = request->header("If-None-Match");
    if (ifNoneMatchHeader.length() == 0) {
      return false;
    }
    const char* ifNoneMatch = ifNoneMatchHeader.c_str();
    
    // Extract ETag values without quotes for comparison
    // ETag format from manifest: "hash" (with quotes in PROGMEM)
    // Header format: "hash" or hash (may or may not have quotes)
    
    // Read manifest ETag from PROGMEM
    const char* etagStart = etag;
    // Skip leading quote if present
    if (pgm_read_byte(etagStart) == '"') {
      etagStart++;
    }
    
    // Find end of ETag (before trailing quote)
    size_t etagLen = 0;
    const char* etagPtr = etagStart;
    while (pgm_read_byte(etagPtr) != '\0' && pgm_read_byte(etagPtr) != '"') {
      etagLen++;
      etagPtr++;
    }
    
    // Extract header ETag (skip quotes if present)
    const char* headerStart = ifNoneMatch;
    if (*headerStart == '"') {
      headerStart++;
    }
    size_t headerLen = strlen(headerStart);
    // Remove trailing quote if present
    if (headerLen > 0 && headerStart[headerLen - 1] == '"') {
      headerLen--;
    }
    
    // Compare lengths
    if (etagLen != headerLen) {
      return false;
    }
    
    // Compare character by character (etag in PROGMEM, header in RAM)
    for (size_t i = 0; i < etagLen; i++) {
      char etagChar = pgm_read_byte(etagStart + i);
      if (etagChar != headerStart[i]) {
        return false;
      }
    }
  } // String ifNoneMatchHeader goes out of scope here, freeing memory
  
  return true;
}


void WebServerManager::handleAssetRequest(AsyncWebServerRequest* request, const struct WebAsset* asset) {
  if (!request || !asset) {
    if (request) {
      request->send(404, "text/plain", "Asset not found");
    }
    return;
  }
  
  // Decode index from pointer
  size_t index = (size_t)((uintptr_t)asset) - 1;
  if (index >= WEB_ASSETS_COUNT) {
    request->send(500, "text/plain", "Invalid asset index");
    return;
  }
  
  // Read asset struct from PROGMEM
  WebAsset assetData;
  readAssetFromProgmem(index, &assetData);
  
  const uint8_t* data = assetData.data;
  size_t len = assetData.len;
  bool gzipped = assetData.gzipped;
  const char* mimeType = assetData.mime_type;
  const char* etag = assetData.etag;
  uint32_t cacheSeconds = assetData.cacheSeconds;
  
  if (!data || len == 0) {
    request->send(500, "text/plain", "Asset data invalid");
    return;
  }
  
  // Check ETag - if match, send 304 Not Modified
  if (etag && checkETag(request, etag)) {
    AsyncWebServerResponse* response = request->beginResponse(304);
    response->addHeader("ETag", FPSTR(etag));
    
    // Build Cache-Control header with manifest cacheSeconds (stack buffer, deterministic)
    char cacheControl[64];
    snprintf(cacheControl, sizeof(cacheControl), "public, max-age=%lu", (unsigned long)cacheSeconds);
    response->addHeader("Cache-Control", cacheControl);
    
    request->send(response);
    return;
  }
  
  // Serve asset directly from PROGMEM using beginResponse_P
  // This is deterministic and low-heap: no chunk buffers, no copying to RAM
  const char* mime = mimeType ? reinterpret_cast<const char*>(FPSTR(mimeType)) : "application/octet-stream";
  AsyncWebServerResponse* response = request->beginResponse_P(200, mime, data, len);
  
  // Always set Content-Encoding: gzip for gzipped assets
  if (gzipped) {
    response->addHeader("Content-Encoding", "gzip");
  }
  
  // Set ETag header (from manifest)
  if (etag) {
    response->addHeader("ETag", FPSTR(etag));
  }
  
  // Build Cache-Control header with manifest cacheSeconds (stack buffer, deterministic)
  char cacheControl[64];
  snprintf(cacheControl, sizeof(cacheControl), "public, max-age=%lu", (unsigned long)cacheSeconds);
  response->addHeader("Cache-Control", cacheControl);
  
  request->send(response);
}

void WebServerManager::registerAssetRoutes() {
  if (!server) {
    return;
  }
  
  // Register root route (/) with smart dispatch
  // - WML backend + AP-only: Redirect to Portal setup (/wml/setup)
  // - WML backend + STA connected: Serve Homewind UI
  // - Fixed backend: Always serve Homewind UI
  server->on("/", HTTP_GET, [this](AsyncWebServerRequest* request) {
#if HW_WIFI_BACKEND_WML
    // Check if Station is online (Level B)
    // If not, user needs to configure WiFi via Portal
    if (!SystemState::isStationOnline()) {
      // Redirect to Portal setup page
      request->redirect("/wml/setup");
      return;
    }
#endif
    
    // Serve Homewind UI (index.html)
    const struct WebAsset* asset = findAsset("index.html");
    if (asset) {
      handleAssetRequest(request, asset);
    } else {
      request->send(404, "text/plain", "index.html not found");
    }
  });
  
  // Register explicit routes for all assets in manifest (deterministic, stack buffers only)
  for (size_t i = 0; i < WEB_ASSETS_COUNT; i++) {
    // Read asset struct from PROGMEM to get path
    WebAsset asset;
    readAssetFromProgmem(i, &asset);
    
    if (asset.path) {
      // Build route path using stack buffer (deterministic, no heap allocation)
      // Route format: "/assetname"
      const char* assetPath = reinterpret_cast<const char*>(FPSTR(asset.path));
      char routePath[64];
      snprintf(routePath, sizeof(routePath), "/%s", assetPath);
      
      // Capture asset index for lambda (encode as pointer, same as findAsset)
      const struct WebAsset* assetPtr = (const WebAsset*)((uintptr_t)(i + 1));
      
      // Register route
      server->on(routePath, HTTP_GET, [this, assetPtr](AsyncWebServerRequest* request) {
        handleAssetRequest(request, assetPtr);
      });
    }
  }
  
  // Generic notFound handler that checks manifest for any path
  // Handles edge cases (query params, extra slashes, etc.) with minimal allocations
  server->onNotFound([this](AsyncWebServerRequest* request) {
    // Extract path from URL - minimize String lifetime
    // ESPAsyncWebServer requires String for url(), but we use it only for path extraction
    {
      String urlStr = request->url();
      const char* url = urlStr.c_str();
      if (!url) {
        request->send(404, "text/plain", "Not found");
        return;
      }
      
      // Skip leading slash
      const char* pathStart = url;
      if (*pathStart == '/') {
        pathStart++;
      }
      
      // Find end of path (before query string or end of string)
      const char* pathEnd = pathStart;
      while (*pathEnd && *pathEnd != '?' && *pathEnd != ' ') {
        pathEnd++;
      }
      
      // If path is empty, try index.html
      if (pathEnd == pathStart) {
        const struct WebAsset* asset = findAsset("index.html");
        if (asset) {
          handleAssetRequest(request, asset);
          return;
        }
      }
      
      // Copy path to stack buffer for findAsset (requires null-terminated string)
      // Max path length is reasonable (64 chars should be enough for asset names)
      char pathBuf[64];
      size_t pathLen = pathEnd - pathStart;
      if (pathLen >= sizeof(pathBuf)) {
        pathLen = sizeof(pathBuf) - 1;
      }
      memcpy(pathBuf, pathStart, pathLen);
      pathBuf[pathLen] = '\0';
      
      const struct WebAsset* asset = findAsset(pathBuf);
      if (asset) {
        handleAssetRequest(request, asset);
        return;
      }
    } // String urlStr goes out of scope here, freeing memory
    
    request->send(404, "text/plain", "Not found");
  });
}

#else // HOMEWIND_ENABLE_WEBSERVER not defined

// Stub implementations when web server is disabled
WebServerManager::WebServerManager()
  : initialized(false)
{
}

WebServerManager::~WebServerManager() {
}

bool WebServerManager::begin() {
  HW_DEBUG_PRINTLN("[WebServer] Disabled by compile-time flag");
  return false; // Important: return false so webReady is not set
}

void WebServerManager::loop() {
  (void)0; // Suppress unused function warning
}

#endif // HOMEWIND_ENABLE_WEBSERVER

