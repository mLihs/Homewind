/**
 * @file MaintenanceMode.cpp
 * @brief One-way maintenance lifecycle on the loop task (design §7.4)
 */

#include "MaintenanceMode.h"
#include "DebugLog.h"
#include "../app/Config.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_attr.h"

#if HW_ENABLE_FANS
#include "FanController.h"
extern FanController* g_fanController;
#endif

#if HW_ENABLE_FANS && HW_ENABLE_BLE
#include "FanHRAdapter.h"
extern FanHRAdapter* g_fanHrAdapter;
#endif

#if HW_ENABLE_BLE
#include "BLERelayManager.h"
extern BLERelayManager* g_bleRelayManager;
static constexpr uint32_t kScansMs = BBS_CONNECT_TIMEOUT_MS + 3000;
#else
static constexpr uint32_t kScansMs = 8000;
#endif

namespace {

struct FactoryResetReport {
  uint32_t magic;
  uint32_t attempted;
  uint32_t failedMask;
};
constexpr uint32_t kReportMagic = 0x48574652;   // "HWFR"
constexpr uint32_t kRebootGraceMs = 250;

RTC_NOINIT_ATTR FactoryResetReport s_rtcReport;
FactoryResetReport s_bootReport = {0, 0, 0};

hw::MaintenanceLifecycle s_lc(kScansMs);

StaticSemaphore_t s_ackSemBuf;
SemaphoreHandle_t s_ackSem = nullptr;

std::atomic<bool> s_restartRequested{false};
std::atomic<bool> s_wifiResetRequested{false};
uint32_t s_restartAtMs = 0;
bool s_restartArmed = false;
bool s_factoryResetDone = false;

void (*s_wifiResetHook)() = nullptr;
bool (*s_clearSettings)() = nullptr;
bool (*s_clearWifi)() = nullptr;

SemaphoreHandle_t ackSem() {
  // First call happens in App setup (captureBootReport) before any other task.
  if (!s_ackSem) s_ackSem = xSemaphoreCreateBinaryStatic(&s_ackSemBuf);
  return s_ackSem;
}

void giveAck() {
  xSemaphoreGive(ackSem());
}

class LoopPort : public hw::MaintPort {
public:
  bool bleRunning() override {
#if HW_ENABLE_BLE
    return g_bleRelayManager && g_bleRelayManager->ready();
#else
    return false;
#endif
  }
  void stopFans() override {
#if HW_ENABLE_FANS
    if (g_fanController) g_fanController->stopForMaintenance();
#endif
  }
  void stopHrAdapter() override {
#if HW_ENABLE_FANS && HW_ENABLE_BLE
    if (g_fanHrAdapter) g_fanHrAdapter->stop();
#endif
  }
  bool quiesceBle() override {
#if HW_ENABLE_BLE
    return g_bleRelayManager->quiesceForMaintenance();
#else
    return false;
#endif
  }
  bool discoveryDoneSeen() override {
#if HW_ENABLE_BLE
    return g_bleRelayManager->maintenanceDiscoveryDone();
#else
    return true;
#endif
  }
  bool bleQuiesced() override {
#if HW_ENABLE_BLE
    return g_bleRelayManager->maintenanceQuiesced();
#else
    return true;
#endif
  }
  bool removeSensors() override {
#if HW_ENABLE_BLE
    return g_bleRelayManager->removeSensorsForMaintenance();
#else
    return true;
#endif
  }
  void discardQueues() override {
#if HW_ENABLE_BLE
    if (g_bleRelayManager && g_bleRelayManager->ready()) g_bleRelayManager->stopForMaintenance();
#endif
#if HW_ENABLE_FANS
    if (g_fanController) g_fanController->discardQueue();
#endif
  }
};

void runFactoryReset() {
  s_rtcReport.magic = kReportMagic;
  s_rtcReport.attempted = 1;
  s_rtcReport.failedMask = 0;
  if (!s_clearSettings || !s_clearSettings()) {
    s_rtcReport.failedMask |= 1u;
  }
  if (s_clearWifi && !s_clearWifi()) {
    s_rtcReport.failedMask |= 2u;
  }
  HW_DEBUG_PRINTF("[MaintenanceMode] Factory reset erased (failedMask=0x%lx), rebooting\n",
                  static_cast<unsigned long>(s_rtcReport.failedMask));
}

}  // namespace

namespace MaintenanceMode {

hw::MaintAdmit request(hw::MaintReason reason) {
  const hw::MaintAdmit r = s_lc.request(reason, millis());
  if (r == hw::MaintAdmit::ACCEPTED) {
    HW_DEBUG_PRINTF("[MaintenanceMode] Admitted: %s\n", reasonString());
  }
  return r;
}

hw::MaintAdmit requestDownload(std::atomic<uint8_t>& fwPhase) {
  const hw::MaintAdmit r = s_lc.requestDownload(fwPhase, millis());
  if (r == hw::MaintAdmit::ACCEPTED) {
    HW_DEBUG_PRINTLN("[MaintenanceMode] Admitted: ota");
  }
  return r;
}

void failAdmission() {
  s_lc.failAdmission();
  HW_ERROR_PRINTLN("[MaintenanceMode] Admission failed (job enqueue), restart required");
}

bool abortQuiescing(std::atomic<uint8_t>& fwPhase) {
  return s_lc.abortQuiescing(fwPhase);
}

void waitForAck(uint32_t timeoutMs) {
  if (s_lc.result() != hw::MaintResult::PENDING) return;
  xSemaphoreTake(ackSem(), pdMS_TO_TICKS(timeoutMs));
}

bool workerMayDownload(std::atomic<uint8_t>& fwPhase) {
  return s_lc.workerMayDownload(fwPhase);
}

bool end() {
  return false;
}

bool isActive() {
  return s_lc.admissionClosed();
}

bool admissionClosed() {
  return s_lc.admissionClosed();
}

hw::MaintReason reason() {
  return s_lc.reason();
}

const char* reasonString() {
  switch (s_lc.reason()) {
    case hw::MaintReason::OTA:
      return "ota";
    case hw::MaintReason::FACTORY_RESET:
      return "factory_reset";
    case hw::MaintReason::MANUAL:
      return "manual";
    case hw::MaintReason::NONE:
      return "none";
    default:
      return "unknown";
  }
}

const char* stateString() {
  switch (s_lc.state()) {
    case hw::MaintState::IDLE: return "idle";
    case hw::MaintState::REQUESTED: return "requested";
    case hw::MaintState::STOPPING_DISCOVERY: return "stopping_discovery";
    case hw::MaintState::STOPPING_SCANS: return "stopping_scans";
    case hw::MaintState::REMOVING: return "removing";
    case hw::MaintState::ACKED: return "acked";
    case hw::MaintState::FAILED: return "failed";
  }
  return "unknown";
}

const char* resultString() {
  switch (s_lc.result()) {
    case hw::MaintResult::PENDING: return "pending";
    case hw::MaintResult::ACK: return "ack";
    case hw::MaintResult::FAILED: return "failed";
    case hw::MaintResult::ABORTED: return "aborted";
  }
  return "unknown";
}

bool restartRequired() {
  return s_lc.restartRequired();
}

void setRestartRequired() {
  s_lc.setRestartRequired();
}

void requestRestart() {
  s_restartRequested = true;
}

bool requestWifiReset() {
  if (s_lc.admissionClosed()) return false;
  s_wifiResetRequested = true;
  return true;
}

void setWifiResetHook(void (*hook)()) {
  s_wifiResetHook = hook;
}

void setFactoryResetHooks(bool (*clearSettings)(), bool (*clearWifi)()) {
  s_clearSettings = clearSettings;
  s_clearWifi = clearWifi;
}

void captureBootReport() {
  ackSem();
  s_lc.setSignal(giveAck);
  if (s_rtcReport.magic == kReportMagic) {
    s_bootReport = s_rtcReport;
    HW_DEBUG_PRINTF("[MaintenanceMode] Factory reset report: attempted=%lu failedMask=0x%lx\n",
                    static_cast<unsigned long>(s_bootReport.attempted),
                    static_cast<unsigned long>(s_bootReport.failedMask));
  }
  s_rtcReport.magic = 0;
  s_rtcReport.attempted = 0;
  s_rtcReport.failedMask = 0;
}

bool factoryResetReport(uint32_t& attempted, uint32_t& failedMask) {
  if (s_bootReport.magic != kReportMagic) return false;
  attempted = s_bootReport.attempted;
  failedMask = s_bootReport.failedMask;
  return true;
}

void loop() {
  const uint32_t now = millis();
  const hw::MaintState before = s_lc.state();
  LoopPort port;
  s_lc.tick(now, port);
  const hw::MaintState after = s_lc.state();
  if (after != before) {
    HW_DEBUG_PRINTF("[MaintenanceMode] %s (%s)\n", stateString(), reasonString());
  }

  if (after == hw::MaintState::ACKED && s_lc.reason() == hw::MaintReason::FACTORY_RESET &&
      !s_factoryResetDone) {
    s_factoryResetDone = true;
    runFactoryReset();
    s_restartRequested = true;
  }

  if (s_wifiResetRequested.exchange(false)) {
    // Checked again here: admission may have closed after the HTTP reply.
    if (!s_lc.admissionClosed() && s_wifiResetHook) {
      s_wifiResetHook();
    }
  }

  if (s_restartRequested && !s_restartArmed) {
    s_restartArmed = true;
    s_restartAtMs = now;
  }
  if (s_restartArmed && now - s_restartAtMs >= kRebootGraceMs) {
    HW_DEBUG_PRINTLN("[MaintenanceMode] Restarting");
    ESP.restart();
  }
}

}  // namespace MaintenanceMode
