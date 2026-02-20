/**
 * @file FirmwareUpdateManager.cpp
 * @brief Implementation of FirmwareUpdateManager
 */

#include "FirmwareUpdateManager.h"
#include "../app/BuildInfo.h"
#include "../core/DebugLog.h"
#include "../core/MaintenanceMode.h"

#if HOMEWIND_ENABLE_WEBSOCKET
#include "../web/WebSocketTelemetry.h"
extern WebSocketTelemetry* g_webSocketTelemetry;
#endif

#if HW_ENABLE_OTA

#include <new>  // For placement-new

// Static instance pointer for callbacks
FirmwareUpdateManager* FirmwareUpdateManager::_instance = nullptr;

// Static buffer for GitFirmwareUpdate (placement-new, no heap allocation)
alignas(GitFirmwareUpdate) static uint8_t s_fwUpdateBuffer[sizeof(GitFirmwareUpdate)];

// Worker task: use static allocation to avoid heap fragmentation (stack was 12KB heap block)
// This directly targets the observed largest_free_block drop after FirmwareUpdate init.
static constexpr uint32_t FW_UPD_WORKER_STACK_BYTES = 12288;
static StackType_t s_fwWorkerStack[FW_UPD_WORKER_STACK_BYTES / sizeof(StackType_t)];
static StaticTask_t s_fwWorkerTcb;

FirmwareUpdateManager::FirmwareUpdateManager()
  : _ready(false)
  , _fwUpdate(nullptr)
  , _webServer(nullptr)
  , _jobQueue(nullptr)
  , _workerTask(nullptr)
  , _stateMutex(nullptr)
  , _dlDoneSem(nullptr)
  , _dlOk(false)
  , _dlSeq(0)
  , _dlSeqDone(0)
  , _phase(PHASE_IDLE)
  , _lastProgress(0)
{
  _instance = this;
  // Initialize cached buffers
  _cachedRemoteVersion[0] = '\0';
  _cachedReleaseNotes[0] = '\0';
}

FirmwareUpdateManager::~FirmwareUpdateManager() {
  // Cleanup worker task
  if (_workerTask) {
    vTaskDelete(_workerTask);
    _workerTask = nullptr;
  }
  
  // Cleanup queue
  if (_jobQueue) {
    vQueueDelete(_jobQueue);
    _jobQueue = nullptr;
  }

  // Cleanup download completion semaphore
  if (_dlDoneSem) {
    vSemaphoreDelete(_dlDoneSem);
    _dlDoneSem = nullptr;
  }
  
  // Cleanup mutex
  if (_stateMutex) {
    vSemaphoreDelete(_stateMutex);
    _stateMutex = nullptr;
  }
  
  if (_fwUpdate) {
    // Explicit destructor call for placement-new object
    _fwUpdate->~GitFirmwareUpdate();
    _fwUpdate = nullptr;
  }
  _instance = nullptr;
}

bool FirmwareUpdateManager::begin(const char* currentVersion, const char* githubUrl) {
  if (_ready) {
    return true;
  }
  
  if (!currentVersion || !githubUrl) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Invalid parameters");
    return false;
  }
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Initializing...");
  
  // Placement-new: construct in static buffer (no heap allocation)
  _fwUpdate = new (s_fwUpdateBuffer) GitFirmwareUpdate(currentVersion, githubUrl);
  
  // Configure callbacks
  _fwUpdate->setProgressCallback(onProgress);
  _fwUpdate->setServerHandleCallback(onServerHandle);
  _fwUpdate->setTimeout(60000);  // 60 second timeout
  _fwUpdate->setRetryCount(1);   // Retry once on failure
  
  // Create state mutex for thread-safe access
  _stateMutex = xSemaphoreCreateMutex();
  if (!_stateMutex) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to create state mutex");
    delete _fwUpdate;
    _fwUpdate = nullptr;
    return false;
  }
  
  // Create job queue (length 2: one pending job + one in progress)
  _jobQueue = xQueueCreate(2, sizeof(Job));
  if (!_jobQueue) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to create job queue");
    vSemaphoreDelete(_stateMutex);
    _stateMutex = nullptr;
    delete _fwUpdate;
    _fwUpdate = nullptr;
    return false;
  }

  // Create download completion semaphore (used by sync download HTTP endpoint)
  _dlDoneSem = xSemaphoreCreateBinary();
  if (!_dlDoneSem) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to create download semaphore");
    vQueueDelete(_jobQueue);
    _jobQueue = nullptr;
    vSemaphoreDelete(_stateMutex);
    _stateMutex = nullptr;
    delete _fwUpdate;
    _fwUpdate = nullptr;
    return false;
  }
  // Ensure clean initial state (semaphore starts empty)
  _dlOk = false;
  _dlSeq = 0;
  _dlSeqDone = 0;
  _dlErrorMsg = "";
  
  // Create worker task (pinned to core 1, priority above idle)
  // Use static allocation to avoid a large heap allocation that fragments internal heap.
#if (configSUPPORT_STATIC_ALLOCATION == 1)
  _workerTask = xTaskCreateStaticPinnedToCore(
    workerTrampoline,
    "fw_upd",
    (FW_UPD_WORKER_STACK_BYTES / sizeof(StackType_t)), // Stack depth (words)
    this,
    tskIDLE_PRIORITY + 3,
    s_fwWorkerStack,
    &s_fwWorkerTcb,
    1
  );
  if (_workerTask == nullptr) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to create worker task (static)");
    vQueueDelete(_jobQueue);
    _jobQueue = nullptr;
    if (_dlDoneSem) {
      vSemaphoreDelete(_dlDoneSem);
      _dlDoneSem = nullptr;
    }
    vSemaphoreDelete(_stateMutex);
    _stateMutex = nullptr;
    delete _fwUpdate;
    _fwUpdate = nullptr;
    return false;
  }
#else
  BaseType_t taskResult = xTaskCreatePinnedToCore(
    workerTrampoline,
    "fw_upd",
    FW_UPD_WORKER_STACK_BYTES,  // Stack size (bytes)
    this,
    tskIDLE_PRIORITY + 3,  // Priority 3 above idle
    &_workerTask,
    1  // Core 1
  );
  if (taskResult != pdPASS) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to create worker task");
    vQueueDelete(_jobQueue);
    _jobQueue = nullptr;
    if (_dlDoneSem) {
      vSemaphoreDelete(_dlDoneSem);
      _dlDoneSem = nullptr;
    }
    vSemaphoreDelete(_stateMutex);
    _stateMutex = nullptr;
    delete _fwUpdate;
    _fwUpdate = nullptr;
    return false;
  }
#endif
  
  _ready = true;
  setPhase(PHASE_IDLE);
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Initialized with worker task");
  return true;
}

void FirmwareUpdateManager::loop() {
  if (!_ready || !_fwUpdate) {
    return;
  }
  
  // GitFirmwareUpdate is blocking, so we don't need to call loop()
  // State is managed by our own phase tracking
  processStateMachine();
}

bool FirmwareUpdateManager::isUpdating() const {
  if (!_fwUpdate) {
    return false;
  }

  UpdatePhase phase = getPhase();
  return _fwUpdate->isUpdating()
      || phase == PHASE_SEARCHING
      || phase == PHASE_DOWNLOADING
      || phase == PHASE_INSTALLING;
}

bool FirmwareUpdateManager::requestSearch() {
  if (!_ready || !_fwUpdate || !_jobQueue) {
    return false;
  }
  
  // Check if already busy
  if (isUpdating()) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Already busy, cannot enqueue search");
    return false;
  }
  
  // Check if queue is full (already has pending job)
  if (uxQueueSpacesAvailable(_jobQueue) == 0) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Job queue full");
    return false;
  }
  
  // Enqueue search job
  Job job = JOB_SEARCH;
  if (xQueueSend(_jobQueue, &job, 0) != pdTRUE) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to enqueue search job");
    return false;
  }
  
  // Set phase immediately so status endpoint shows searching
  setPhase(PHASE_SEARCHING);
  setError("");
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Search job enqueued");
  return true;
}

bool FirmwareUpdateManager::requestDownload() {
  if (!_ready || !_fwUpdate || !_jobQueue) {
    return false;
  }
  
  // Only allow download if we're in READY_TO_UPDATE phase
  if (getPhase() != PHASE_READY_TO_UPDATE) {
    HW_DEBUG_PRINTF("[FirmwareUpdate] Cannot download in phase: %d\n", getPhase());
    return false;
  }
  
  // Check if already busy
  if (isUpdating()) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Already busy, cannot enqueue download");
    return false;
  }
  
  // Check if queue is full
  if (uxQueueSpacesAvailable(_jobQueue) == 0) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Job queue full");
    return false;
  }
  
  // Enqueue download job
  Job job = JOB_DOWNLOAD;
  if (xQueueSend(_jobQueue, &job, 0) != pdTRUE) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to enqueue download job");
    return false;
  }
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Download job enqueued");
  return true;
}

bool FirmwareUpdateManager::beginDownloadSync(uint32_t* outSeq) {
  if (!_ready || !_fwUpdate || !_jobQueue || !_dlDoneSem) {
    if (outSeq) *outSeq = 0;
    return false;
  }
  
  // Only allow download if we're in READY_TO_UPDATE phase
  if (getPhase() != PHASE_READY_TO_UPDATE) {
    HW_DEBUG_PRINTF("[FirmwareUpdate] Cannot download in phase: %d\n", getPhase());
    if (outSeq) *outSeq = 0;
    return false;
  }
  
  // Check if already busy
  if (isUpdating()) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Already busy, cannot enqueue download");
    if (outSeq) *outSeq = 0;
    return false;
  }
  
  // Check if queue is full
  if (uxQueueSpacesAvailable(_jobQueue) == 0) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Job queue full");
    if (outSeq) *outSeq = 0;
    return false;
  }
  
  // Clear any previous completion signal (drain semaphore if already given)
  while (xSemaphoreTake(_dlDoneSem, 0) == pdTRUE) {
    // Drain any pending signals
  }
  
  // Reset download state
  _dlOk = false;
  if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    _dlErrorMsg = "";
    xSemaphoreGive(_stateMutex);
  } else {
    _dlErrorMsg = "";
  }
  
  // Increment sequence number
  uint32_t seq = ++_dlSeq;
  if (outSeq) *outSeq = seq;
  
  // Enqueue download job
  Job job = JOB_DOWNLOAD;
  if (xQueueSend(_jobQueue, &job, 0) != pdTRUE) {
    HW_ERROR_PRINTLN("[FirmwareUpdate] Failed to enqueue download job");
    if (outSeq) *outSeq = 0;
    return false;
  }
  
  HW_DEBUG_PRINTF("[FirmwareUpdate] Download job enqueued (seq=%u)\n", seq);
  return true;
}

bool FirmwareUpdateManager::waitDownloadDone(uint32_t seq, uint32_t timeoutMs, bool* okOut, String* errOut) {
  if (!_dlDoneSem) {
    if (okOut) *okOut = false;
    if (errOut) *errOut = "Download semaphore not initialized";
    return false;
  }
  
  // Wait for semaphore with timeout
  TickType_t ticks = (timeoutMs == 0) ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  if (xSemaphoreTake(_dlDoneSem, ticks) != pdTRUE) {
    // Timeout
    if (okOut) *okOut = false;
    if (errOut) *errOut = "timeout";
    return false;
  }
  
  // Semaphore received - verify sequence number matches
  if (_dlSeqDone != seq) {
    // Sequence mismatch - this completion is for a different request
    // Put semaphore back and return timeout (treat as not done yet)
    xSemaphoreGive(_dlDoneSem);
    if (okOut) *okOut = false;
    if (errOut) *errOut = "sequence_mismatch";
    return false;
  }
  
  // Sequence matches - return result
  if (okOut) *okOut = _dlOk;
  if (errOut) {
    if (!_dlOk) {
      // Copy error message
      if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        *errOut = _dlErrorMsg;
        xSemaphoreGive(_stateMutex);
      } else {
        *errOut = _dlErrorMsg;
      }
      if (errOut->length() == 0) {
        *errOut = "download_failed";
      }
    } else {
      *errOut = "";
    }
  }
  
  return true;
}

bool FirmwareUpdateManager::startSearch() {
  // DEPRECATED: This blocks async_tcp. Use requestSearch() instead.
  if (!_ready || !_fwUpdate) {
    return false;
  }
  
  if (getPhase() != PHASE_IDLE) {
    HW_DEBUG_PRINTLN("[FirmwareUpdate] Already busy");
    return false;
  }
  
  setPhase(PHASE_SEARCHING);
  setError("");
  _lastSearchResult = "";
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Starting search (blocking)...");
  
  // checkForUpdate() is blocking, but we call it directly
  if (!_fwUpdate->checkForUpdate()) {
    // Check if it's just no update available vs actual error
    GitFirmwareUpdate::UpdateError error = _fwUpdate->getLastError();
    if (error == GitFirmwareUpdate::NO_UPDATE_AVAILABLE) {
      // No update available is not an error, just a result
      handleSearchComplete();
      return true;
    } else {
      setPhase(PHASE_ERROR);
      setError(_fwUpdate->getLastErrorString());
      HW_ERROR_PRINTF("[FirmwareUpdate] Search failed: %s\n", getLastError());
      return false;
    }
  }
  
  // Update found, handle the result
  handleSearchComplete();
  return true;
}

bool FirmwareUpdateManager::startDownload() {
  // DEPRECATED: This blocks async_tcp. Use requestDownload() instead.
  // Enter maintenance mode before download starts (OTA hook)
  MaintenanceMode::begin(MaintenanceReason::OTA);
  
  if (!_ready || !_fwUpdate) {
    return false;
  }
  
  if (getPhase() != PHASE_READY_TO_UPDATE) {
    HW_DEBUG_PRINTF("[FirmwareUpdate] Cannot download in phase: %d\n", getPhase());
    return false;
  }
  
  setPhase(PHASE_DOWNLOADING);
  setProgress(0);
  setError("");
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Starting download (blocking)...");
  
  // Get firmware URL from last search
  String url = _fwUpdate->getFirmwareUrl();
  if (url.length() == 0) {
    setPhase(PHASE_ERROR);
    setError("No firmware URL available");
    return false;
  }
  
  // downloadAndInstall() is blocking and will restart device on success
  // If it returns, it means it failed
  if (!_fwUpdate->downloadAndInstall(url)) {
    setPhase(PHASE_ERROR);
    setError(_fwUpdate->getLastErrorString());
    HW_ERROR_PRINTF("[FirmwareUpdate] Download failed: %s\n", getLastError());
    return false;
  }
  
  // If we reach here, download succeeded and device should restart
  // But if it didn't restart for some reason, mark as installing
  setPhase(PHASE_INSTALLING);
  return true;
}

bool FirmwareUpdateManager::startInstall() {
  // Enter maintenance mode before install starts (OTA hook - safety measure)
  // begin() is idempotent, safe to call even if already active
  MaintenanceMode::begin(MaintenanceReason::OTA);
  
  // Installation is handled automatically by GitFirmwareUpdate after download
  // This is called by the UI, but we just verify state
  if (getPhase() != PHASE_DOWNLOADING) {
    HW_DEBUG_PRINTF("[FirmwareUpdate] Cannot install in phase: %d\n", getPhase());
    return false;
  }
  
  // Installation happens automatically after download completes
  // We just transition to installing phase
  setPhase(PHASE_INSTALLING);
  return true;
}

bool FirmwareUpdateManager::abort() {
  if (!_fwUpdate) {
    return false;
  }
  
  _fwUpdate->abortUpdate();
  setPhase(PHASE_IDLE);
  setError("Update aborted by user");
  
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Update aborted");
  return true;
}

const char* FirmwareUpdateManager::getStateString() const {
  // Static buffer for state string (no heap allocation)
  static char stateBuffer[32];
  
  UpdatePhase phase = getPhase();
  switch (phase) {
    case PHASE_IDLE:
      strncpy(stateBuffer, "idle", sizeof(stateBuffer) - 1);
      break;
    case PHASE_SEARCHING:
      strncpy(stateBuffer, "searching", sizeof(stateBuffer) - 1);
      break;
    case PHASE_READY_TO_UPDATE:
      strncpy(stateBuffer, "ready_to_update", sizeof(stateBuffer) - 1);
      break;
    case PHASE_DOWNLOADING:
      strncpy(stateBuffer, "downloading", sizeof(stateBuffer) - 1);
      break;
    case PHASE_INSTALLING:
      strncpy(stateBuffer, "installing", sizeof(stateBuffer) - 1);
      break;
    case PHASE_ERROR:
      strncpy(stateBuffer, "error", sizeof(stateBuffer) - 1);
      break;
    default:
      strncpy(stateBuffer, "unknown", sizeof(stateBuffer) - 1);
      break;
  }
  stateBuffer[sizeof(stateBuffer) - 1] = '\0';
  return stateBuffer;
}

int FirmwareUpdateManager::getProgress() const {
  return _lastProgress;  // Volatile, safe to read
}

const char* FirmwareUpdateManager::getLastError() const {
  // Return c_str() of internal String - valid until next setError() call
  // Note: _lastErrorMsg.c_str() returns stable pointer as long as String isn't modified
  return _lastErrorMsg.c_str();
}

const char* FirmwareUpdateManager::getRemoteVersion() const {
  // Return cached version from last search (no heap allocation)
  return _cachedRemoteVersion;
}

const char* FirmwareUpdateManager::getReleaseNotes() const {
  // Return cached notes from last search (no heap allocation)
  return _cachedReleaseNotes;
}

void FirmwareUpdateManager::setWebServer(AsyncWebServer* server) {
  _webServer = server;
}

void FirmwareUpdateManager::processStateMachine() {
  if (!_fwUpdate) {
    return;
  }
  
  // GitFirmwareUpdate is blocking, so state is managed by our phase tracking
  // Progress updates come via callbacks during blocking operations
  // No need to poll for state changes since operations are synchronous
  
  // Update progress from GitFirmwareUpdate if downloading
  UpdatePhase phase = getPhase();
  if (phase == PHASE_DOWNLOADING && _fwUpdate->isUpdating()) {
    size_t bytesRead, totalBytes;
    int percent;
    if (_fwUpdate->getProgress(bytesRead, totalBytes, percent)) {
      setProgress(percent);
    }
  }
}

void FirmwareUpdateManager::handleSearchComplete() {
  if (!_fwUpdate) {
    return;
  }
  
  GitFirmwareUpdate::UpdateError error = _fwUpdate->getLastError();
  
  if (error == GitFirmwareUpdate::NO_ERROR) {
    // Update available - cache values from GitFirmwareUpdate
    String remoteVersion = _fwUpdate->getRemoteVersion();
    String releaseNotes = _fwUpdate->getReleaseNotes();
    
    // Cache remote version (thread-safe via mutex)
    if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      strncpy(_cachedRemoteVersion, remoteVersion.c_str(), VERSION_BUFFER_SIZE - 1);
      _cachedRemoteVersion[VERSION_BUFFER_SIZE - 1] = '\0';
      strncpy(_cachedReleaseNotes, releaseNotes.c_str(), NOTES_BUFFER_SIZE - 1);
      _cachedReleaseNotes[NOTES_BUFFER_SIZE - 1] = '\0';
      xSemaphoreGive(_stateMutex);
    } else {
      strncpy(_cachedRemoteVersion, remoteVersion.c_str(), VERSION_BUFFER_SIZE - 1);
      _cachedRemoteVersion[VERSION_BUFFER_SIZE - 1] = '\0';
      strncpy(_cachedReleaseNotes, releaseNotes.c_str(), NOTES_BUFFER_SIZE - 1);
      _cachedReleaseNotes[NOTES_BUFFER_SIZE - 1] = '\0';
    }
    
    // Build JSON result without ArduinoJson - escape quotes and newlines in releaseNotes
    // Use fixed buffer to avoid multiple String reallocations
    char escapedBuffer[NOTES_BUFFER_SIZE];
    size_t srcIdx = 0, dstIdx = 0;
    const char* src = _cachedReleaseNotes;
    while (src[srcIdx] != '\0' && dstIdx < NOTES_BUFFER_SIZE - 3) {
      char c = src[srcIdx++];
      if (c == '"') {
        escapedBuffer[dstIdx++] = '\\';
        escapedBuffer[dstIdx++] = '"';
      } else if (c == '\n') {
        escapedBuffer[dstIdx++] = '\\';
        escapedBuffer[dstIdx++] = 'n';
      } else if (c == '\r') {
        // Skip carriage return
      } else {
        escapedBuffer[dstIdx++] = c;
      }
    }
    escapedBuffer[dstIdx] = '\0';
    
    // Build JSON string with single snprintf (no String concatenation)
    char jsonBuffer[NOTES_BUFFER_SIZE + 128];
    snprintf(jsonBuffer, sizeof(jsonBuffer),
             "{\"hasUpdate\":true,\"latestVersion\":\"%s\",\"releaseNotes\":\"%s\"}",
             _cachedRemoteVersion, escapedBuffer);
    _lastSearchResult = jsonBuffer;
    
    setPhase(PHASE_READY_TO_UPDATE);
    
    HW_DEBUG_PRINTF("[FirmwareUpdate] Update available: %s\n", _cachedRemoteVersion);
    
    // Mark WebSocket frame dirty for binary transmission
#if HOMEWIND_ENABLE_WEBSOCKET
    if (g_webSocketTelemetry) {
      g_webSocketTelemetry->markDirty(TelemetryFrameType::FIRMWARE_SEARCH_RESULT);
    }
#endif
  } else if (error == GitFirmwareUpdate::NO_UPDATE_AVAILABLE) {
    // No update available - clear cached values
    if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      _cachedRemoteVersion[0] = '\0';
      _cachedReleaseNotes[0] = '\0';
      xSemaphoreGive(_stateMutex);
    } else {
      _cachedRemoteVersion[0] = '\0';
      _cachedReleaseNotes[0] = '\0';
    }
    
    // Build JSON string: {"hasUpdate":false}
    _lastSearchResult = "{\"hasUpdate\":false}";
    
    setPhase(PHASE_IDLE);
    
    HW_DEBUG_PRINTLN("[FirmwareUpdate] No update available");
    
    // Mark WebSocket frame dirty for binary transmission
#if HOMEWIND_ENABLE_WEBSOCKET
    if (g_webSocketTelemetry) {
      g_webSocketTelemetry->markDirty(TelemetryFrameType::FIRMWARE_SEARCH_RESULT);
    }
#endif
  } else {
    // Error occurred
    setPhase(PHASE_ERROR);
    setError(_fwUpdate->getLastErrorString());
    
    HW_ERROR_PRINTF("[FirmwareUpdate] Search error: %s\n", getLastError());
  }
}

void FirmwareUpdateManager::handleDownloadProgress(int percent) {
  setProgress(percent);
  
  // Send progress via WebSocket (eliminates need for HTTP polling)
#if HOMEWIND_ENABLE_WEBSOCKET
  if (g_webSocketTelemetry) {
    g_webSocketTelemetry->markDirty(TelemetryFrameType::FIRMWARE_PROGRESS);
  }
#endif
}

void FirmwareUpdateManager::handleDownloadComplete() {
  // Download complete, installation will start automatically
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Download complete, starting installation...");
}

void FirmwareUpdateManager::handleInstallationComplete() {
  // Installation complete, device will restart
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Installation complete, device will restart");
  _phase = PHASE_IDLE;  // Will be reset after restart
}

void FirmwareUpdateManager::handleError(GitFirmwareUpdate::UpdateError error) {
  setPhase(PHASE_ERROR);
  setError(_fwUpdate->getLastErrorString());
  
  HW_ERROR_PRINTF("[FirmwareUpdate] Error: %s\n", getLastError());
}

// Static callback for progress
void FirmwareUpdateManager::onProgress(int percent, size_t bytesRead, size_t totalBytes) {
  if (_instance) {
    _instance->handleDownloadProgress(percent);
  }
}

// Static callback for server handle
void FirmwareUpdateManager::onServerHandle() {
  // Keep web server responsive during download
  // This is called periodically during download
  // NOTE: Do NOT call AsyncWebServer methods here - we're in worker task context
  // Just yield to allow other tasks to run
  delay(1);
}

// Worker task trampoline
void FirmwareUpdateManager::workerTrampoline(void* arg) {
  FirmwareUpdateManager* instance = static_cast<FirmwareUpdateManager*>(arg);
  if (instance) {
    instance->workerLoop();
  }
  vTaskDelete(nullptr);  // Should never reach here
}

// Worker task main loop
void FirmwareUpdateManager::workerLoop() {
  HW_DEBUG_PRINTLN("[FirmwareUpdate] Worker task started");
  
  Job job;
  while (true) {
    // Wait for job from queue (blocking)
    if (xQueueReceive(_jobQueue, &job, portMAX_DELAY) == pdTRUE) {
      switch (job) {
        case JOB_SEARCH: {
          HW_DEBUG_PRINTLN("[FirmwareUpdate] Processing SEARCH job");
          
          // Set phase if not already set
          if (getPhase() != PHASE_SEARCHING) {
            setPhase(PHASE_SEARCHING);
          }
          setError("");
          _lastSearchResult = "";
          
          // Small delay before blocking operation
          delay(1);
          
          // Call blocking checkForUpdate() in worker task context
          if (!_fwUpdate->checkForUpdate()) {
            // Check if it's just no update available vs actual error
            GitFirmwareUpdate::UpdateError error = _fwUpdate->getLastError();
            if (error == GitFirmwareUpdate::NO_UPDATE_AVAILABLE) {
              // No update available is not an error, just a result
              handleSearchComplete();
            } else {
              setPhase(PHASE_ERROR);
              setError(_fwUpdate->getLastErrorString());
              HW_ERROR_PRINTF("[FirmwareUpdate] Search failed: %s\n", getLastError());
            }
          } else {
            // Update found, handle the result
            handleSearchComplete();
          }
          
          delay(1);  // Small delay after operation
          break;
        }
        
        case JOB_DOWNLOAD: {
          HW_DEBUG_PRINTLN("[FirmwareUpdate] Processing DOWNLOAD job");
          
          // Capture current sequence number (snapshot after enqueue)
          uint32_t seq = _dlSeq;
          
          // Validate phase
          if (getPhase() != PHASE_READY_TO_UPDATE) {
            HW_ERROR_PRINTF("[FirmwareUpdate] Cannot download in phase: %d\n", getPhase());
            setPhase(PHASE_ERROR);
            setError("Invalid state for download");
            // Store error for sync HTTP response + signal completion
            _dlOk = false;
            if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              _dlErrorMsg = "Invalid state for download";
              xSemaphoreGive(_stateMutex);
            } else {
              _dlErrorMsg = "Invalid state for download";
            }
            _dlSeqDone = seq;
            if (_dlDoneSem) {
              xSemaphoreGive(_dlDoneSem);
            }
            MaintenanceMode::end();  // Exit maintenance mode on error
            break;
          }
          
          // Enter maintenance mode BEFORE download
          MaintenanceMode::begin(MaintenanceReason::OTA);
          
          setPhase(PHASE_DOWNLOADING);
          setProgress(0);
          setError("");
          
          // Get firmware URL from last search
          String url = _fwUpdate->getFirmwareUrl();
          if (url.length() == 0) {
            setPhase(PHASE_ERROR);
            setError("No firmware URL available");
            _dlOk = false;
            if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              _dlErrorMsg = "No firmware URL available";
              xSemaphoreGive(_stateMutex);
            } else {
              _dlErrorMsg = "No firmware URL available";
            }
            _dlSeqDone = seq;
            if (_dlDoneSem) {
              xSemaphoreGive(_dlDoneSem);
            }
            MaintenanceMode::end();  // Exit maintenance mode on error
            HW_ERROR_PRINTLN("[FirmwareUpdate] No firmware URL");
            break;
          }
          
          HW_DEBUG_PRINTF("[FirmwareUpdate] Starting download... (seq=%u)\n", seq);
          
          // Small delay before blocking operation
          delay(1);
          
          // Call blocking downloadAndInstall() in worker task context
          // This will reboot on success, so if it returns, it failed
          if (!_fwUpdate->downloadAndInstall(url)) {
            setPhase(PHASE_ERROR);
            setError(_fwUpdate->getLastErrorString());
            // Store download error for sync HTTP response
            _dlOk = false;
            if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              _dlErrorMsg = _fwUpdate->getLastErrorString();
              xSemaphoreGive(_stateMutex);
            } else {
              _dlErrorMsg = _fwUpdate->getLastErrorString();
            }
            _dlSeqDone = seq;
            // Signal completion (failure)
            if (_dlDoneSem) {
              xSemaphoreGive(_dlDoneSem);
            }
            MaintenanceMode::end();  // Exit maintenance mode on error
            HW_ERROR_PRINTF("[FirmwareUpdate] Download failed: %s\n", getLastError());
          } else {
            // Download succeeded - device should restart
            // If we reach here, it didn't restart for some reason
            setPhase(PHASE_INSTALLING);
            HW_DEBUG_PRINTLN("[FirmwareUpdate] Download succeeded, device should restart");
            // Signal completion (success without reboot)
            _dlOk = true;
            if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
              _dlErrorMsg = "";
              xSemaphoreGive(_stateMutex);
            } else {
              _dlErrorMsg = "";
            }
            _dlSeqDone = seq;
            if (_dlDoneSem) {
              xSemaphoreGive(_dlDoneSem);
            }
          }
          
          delay(1);  // Small delay after operation
          break;
        }
        
        case JOB_NONE:
        default:
          HW_ERROR_PRINTF("[FirmwareUpdate] Unknown job: %d\n", job);
          break;
      }
    }
  }
}

// Thread-safe state access helpers
void FirmwareUpdateManager::setPhase(UpdatePhase phase) {
  if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    _phase = phase;
    xSemaphoreGive(_stateMutex);
  } else {
    // Fallback if mutex not available (shouldn't happen after begin())
    _phase = phase;
  }
}

FirmwareUpdateManager::UpdatePhase FirmwareUpdateManager::getPhase() const {
  // Volatile read is atomic for enum
  return _phase;
}

void FirmwareUpdateManager::setProgress(int progress) {
  // Volatile write is atomic for int
  _lastProgress = progress;
}

void FirmwareUpdateManager::setError(const String& error) {
  if (_stateMutex && xSemaphoreTake(_stateMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    _lastErrorMsg = error;
    xSemaphoreGive(_stateMutex);
  } else {
    // Fallback if mutex not available
    _lastErrorMsg = error;
  }
}

#endif // HW_ENABLE_OTA

