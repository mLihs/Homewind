/**
 * @file FirmwareUpdateManager.h
 * @brief Firmware update manager that bridges GitFirmwareUpdate to Web UI state machine
 */

#ifndef HOMEWIND_FIRMWARE_UPDATE_MANAGER_H
#define HOMEWIND_FIRMWARE_UPDATE_MANAGER_H

#include <Arduino.h>
#include "../app/Config.h"

#if HW_ENABLE_OTA

#include <GitFirmwareUpdate.h>
#include <ESPAsyncWebServer.h>

/**
 * @class FirmwareUpdateManager
 * @brief Manages firmware updates using GitFirmwareUpdate and bridges to Web UI
 * 
 * This class wraps GitFirmwareUpdate and provides:
 * - Integration with Homewind Web UI state machine
 * - JavaScript callback injection for UI updates
 * - Progress reporting via WebSocket or polling
 * - API endpoint handlers
 */
class FirmwareUpdateManager {
public:
  FirmwareUpdateManager();
  ~FirmwareUpdateManager();
  
  /**
   * Initialize firmware update manager
   * @param currentVersion Current firmware version (e.g., "1.0.0")
   * @param githubUrl URL to latest.json on GitHub (raw content)
   * @return true if initialized successfully
   */
  bool begin(const char* currentVersion, const char* githubUrl);
  
  /**
   * Main loop - MUST be called periodically
   * Processes async operations and updates state
   */
  void loop();
  
  /**
   * Check if manager is ready
   * @return true if ready
   */
  bool isReady() const { return _ready; }
  
  /**
   * Check if update is in progress
   * @return true if updating
   */
  bool isUpdating() const;
  
  /**
   * Request firmware search (async - runs in worker task)
   * Called by API endpoint /api/v1/action/firmware/search
   * @return true if search job enqueued
   */
  bool requestSearch();
  
  /**
   * Request firmware download (async - runs in worker task)
   * Called by API endpoint /api/v1/action/firmware/download (async mode)
   * @return true if download job enqueued
   */
  bool requestDownload();
  
  /**
   * Begin synchronous download and get sequence number
   * Used by sync download endpoint to track completion
   * @param outSeq Output parameter for sequence number
   * @return true if download job enqueued, false if busy/invalid state
   */
  bool beginDownloadSync(uint32_t* outSeq);
  
  /**
   * Wait for download completion (for sync download endpoint)
   * @param seq Sequence number from beginDownloadSync()
   * @param timeoutMs Timeout in milliseconds
   * @param okOut Output parameter: true if download succeeded
   * @param errOut Output parameter: error message if failed
   * @return true if completion received, false on timeout
   */
  bool waitDownloadDone(uint32_t seq, uint32_t timeoutMs, bool* okOut, String* errOut);
  
  /**
   * @deprecated Use requestSearch() instead
   */
  bool startSearch();
  
  /**
   * @deprecated Use requestDownload() or beginDownloadSync() instead
   */
  bool startDownload();
  
  /**
   * Start firmware installation (async)
   * Called by API endpoint /api/v1/action/firmware/install
   * @return true if installation started
   */
  bool startInstall();
  
  /**
   * Abort current update operation
   * Called by API endpoint /api/v1/action/firmware/abort
   * @return true if aborted
   */
  bool abort();
  
  /**
   * Get current update state for status endpoint
   * @return State string: "idle", "searching", "downloading", "installing", "error"
   * @note Returns stable c-string buffer (no heap allocation)
   */
  const char* getStateString() const;
  
  /**
   * Get current progress (0-100)
   * @return Progress percentage
   */
  int getProgress() const;
  
  /**
   * Get last error message
   * @return Error message c-string (no heap allocation)
   * @note Returns pointer to internal buffer, valid until next error
   */
  const char* getLastError() const;
  
  /**
   * Get remote version from last search
   * @return Version c-string or empty string (no heap allocation)
   * @note Returns pointer to internal buffer, valid until next search
   */
  const char* getRemoteVersion() const;
  
  /**
   * Get release notes from last search
   * @return Release notes c-string or empty string (no heap allocation)
   * @note Returns pointer to internal buffer, valid until next search
   */
  const char* getReleaseNotes() const;
  
  /**
   * Inject JavaScript bridge functions into HTML
   * Call this when serving index.html to inject window functions
   * @param html HTML content (will be modified in place)
   */
  void injectJavaScriptBridge(String& html);
  
  /**
   * Set WebServer instance for server handle callback
   * @param server WebServer instance (can be nullptr)
   */
  void setWebServer(AsyncWebServer* server);

private:
  bool _ready;
  GitFirmwareUpdate* _fwUpdate;
  AsyncWebServer* _webServer;
  
  // Worker task and queue
  QueueHandle_t _jobQueue;
  TaskHandle_t _workerTask;
  enum Job {
    JOB_NONE,
    JOB_SEARCH,
    JOB_DOWNLOAD
  };
  
  // State tracking
  enum UpdatePhase {
    PHASE_IDLE,
    PHASE_SEARCHING,
    PHASE_READY_TO_UPDATE,
    PHASE_DOWNLOADING,
    PHASE_INSTALLING,
    PHASE_ERROR
  };
  UpdatePhase _phase;
  String _lastSearchResult;  // JSON string for search result
  int _lastProgress;
  String _lastErrorMsg;
  
  // Cached values from last search (avoids String return allocations)
  static constexpr size_t VERSION_BUFFER_SIZE = 32;
  static constexpr size_t NOTES_BUFFER_SIZE = 1024;
  char _cachedRemoteVersion[VERSION_BUFFER_SIZE];
  char _cachedReleaseNotes[NOTES_BUFFER_SIZE];
  
  // Thread-safe state access
  SemaphoreHandle_t _stateMutex;
  
  // Download completion signal (for sync download endpoint)
  SemaphoreHandle_t _dlDoneSem;  // Binary semaphore
  volatile bool _dlOk;            // Download success flag
  String _dlErrorMsg;             // Download error message (protected by _stateMutex)
  volatile uint32_t _dlSeq;        // Current download sequence number
  volatile uint32_t _dlSeqDone;    // Completed download sequence number
  
  // Progress callback
  static void onProgress(int percent, size_t bytesRead, size_t totalBytes);
  static FirmwareUpdateManager* _instance;  // For static callback
  
  // Server handle callback (keeps web server responsive during download)
  static void onServerHandle();
  
  // Process state machine
  void processStateMachine();
  
  // Handle search result
  void handleSearchComplete();
  
  // Handle download progress
  void handleDownloadProgress(int percent);
  
  // Handle download complete
  void handleDownloadComplete();
  
  // Handle installation complete
  void handleInstallationComplete();
  
  // Handle error
  void handleError(GitFirmwareUpdate::UpdateError error);
  
  // Generate JavaScript callback code
  String generateSearchResultJS(const String& resultJson);
  String generateProgressJS(int percent);
  String generateErrorJS(const String& error);
  
  // Worker task
  static void workerTrampoline(void* arg);
  void workerLoop();
  
  // Thread-safe state access helpers
  void setPhase(UpdatePhase phase);
  UpdatePhase getPhase() const;
  void setProgress(int progress);
  void setError(const String& error);
};

#endif // HW_ENABLE_OTA

#endif // HOMEWIND_FIRMWARE_UPDATE_MANAGER_H

