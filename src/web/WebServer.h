/**
 * @file WebServer.h
 * @brief Web server manager for serving static assets from PROGMEM
 */

 #ifndef HOMEWIND_WEB_SERVER_H
 #define HOMEWIND_WEB_SERVER_H
 
 #include <Arduino.h>
 #include "../app/Config.h"
 
 #if HOMEWIND_ENABLE_WEBSERVER
 
 #include <ESPAsyncWebServer.h>
 #include <AsyncTCP.h>   // Required for ESP32 + ESPAsyncWebServer
 
class WebServerManager {
 public:
   WebServerManager();
   ~WebServerManager();

   bool begin();
   void loop();

   // Always available when HOMEWIND_ENABLE_WEBSERVER == 1
   AsyncWebServer* getServer() { return server; }
   
   /**
    * Check if web server is initialized and ready
    * @return true if ready
    */
   bool ready() const { return initialized; }

 private:
   bool initialized;
   AsyncWebServer* server;
 
   const struct WebAsset* findAsset(const char* path);
   void handleAssetRequest(AsyncWebServerRequest* request, const struct WebAsset* asset);
   bool checkETag(AsyncWebServerRequest* request, const char* etag);
   void registerAssetRoutes();
 };
 
 #else  // HOMEWIND_ENABLE_WEBSERVER == 0
 
// Optional stub so the rest of the app compiles in "no web" builds.
class WebServerManager {
 public:
   WebServerManager() : initialized(false) {}
   ~WebServerManager() {}
   bool begin() { return false; }
   void loop() {}
   void* getServer() { return nullptr; }
   bool ready() const { return initialized; }
 private:
   bool initialized;
};
 
 #endif // HOMEWIND_ENABLE_WEBSERVER
 
 #endif // HOMEWIND_WEB_SERVER_H
 