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
#include <atomic>
#include "MaintenanceLifecycle.h"

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
   * Request firmware download (design §7.4): maintenance admission with
   * READY_TO_UPDATE→QUIESCING, then the job is enqueued with zero timeout.
   * The worker downloads only after the maintenance ACK.
   */
  enum class DownloadRequest : uint8_t {
    ACCEPTED,      // admitted and queued (or already running)
    CONFLICT,      // other maintenance active
    NOT_READY,     // no update ready
    QUEUE_FAILED   // admitted, job not queued: restart_required
  };
  DownloadRequest requestDownload();
  
  /**
   * @deprecated Use requestSearch() instead
   */
  bool startSearch();
  
  /**
   * Start firmware installation (async)
   * Called by API endpoint /api/v1/action/firmware/install
   * @return true if installation started
   */
  bool startInstall();
  
  /**
   * Abort current update operation (idempotent)
   * Called by API endpoint /api/v1/action/firmware/abort
   * QUIESCING→ABORTED by CAS; DOWNLOADING → abortUpdate(); both need a restart.
   * Never returns the phase to IDLE once maintenance was admitted.
   * @return true if handled
   */
  bool abort();
  
  /**
   * Get current update state for status endpoint
   * @return State string: "idle", "searching", "ready_to_update", "quiescing",
   *         "downloading", "installing", "error", "restart_required"
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
  
  // State tracking (values = hw::FwPhase, CAS-shared with the maintenance coordinator)
  enum UpdatePhase : uint8_t {
    PHASE_IDLE = static_cast<uint8_t>(hw::FwPhase::IDLE),
    PHASE_SEARCHING = static_cast<uint8_t>(hw::FwPhase::SEARCHING),
    PHASE_READY_TO_UPDATE = static_cast<uint8_t>(hw::FwPhase::READY_TO_UPDATE),
    PHASE_QUIESCING = static_cast<uint8_t>(hw::FwPhase::QUIESCING),
    PHASE_DOWNLOADING = static_cast<uint8_t>(hw::FwPhase::DOWNLOADING),
    PHASE_INSTALLING = static_cast<uint8_t>(hw::FwPhase::INSTALLING),
    PHASE_ERROR = static_cast<uint8_t>(hw::FwPhase::ERROR),
    PHASE_ABORTED = static_cast<uint8_t>(hw::FwPhase::ABORTED)
  };
  std::atomic<uint8_t> _phase;
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

