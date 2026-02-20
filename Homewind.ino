/**
 * Homewind - ESP32 WebUI Library
 * Main sketch file - minimal wiring only
 */

#include "src/app/Config.h"
#include "src/app/App.h"

// Global app instance
App app;

void setup() {
  Serial.begin(115200);
  delay(100); // Brief delay for Serial to initialize
  // Serial Monitor must be 115200 baud; open before upload or press RESET to see boot logs
  Serial.println("\n=== Homewind Starting ===");
  Serial.flush();

  // Initialize application
  if (!app.begin()) {
    Serial.println("[ERROR] Failed to initialize Homewind");
    while (1) {
      delay(1000); // Error loop - only place delay() is acceptable
    }
  }
  
  Serial.println("[INFO] Homewind initialized successfully");
}

void loop() {
  app.loop();
}

