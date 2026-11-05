/**
 * @file MaintenanceLifecycle.h
 * @brief Arduino-free one-way maintenance lifecycle and coordinator (design §7.4)
 *
 * Shared by MaintenanceMode.cpp, FirmwareUpdateManager and
 * tests/maintenance_lifecycle_test.cpp. Admission and abort may run on any task;
 * tick() runs on the loop task only. The coordinator lock is never held across
 * a wait.
 */

#ifndef HOMEWIND_MAINTENANCE_LIFECYCLE_H
#define HOMEWIND_MAINTENANCE_LIFECYCLE_H

#include <atomic>
#include <cstdint>

#if defined(ESP_PLATFORM)
#include "freertos/FreeRTOS.h"
#else
#include <mutex>
#endif

namespace hw {

enum class MaintState : uint8_t {
  IDLE, REQUESTED, STOPPING_DISCOVERY, STOPPING_SCANS, REMOVING, ACKED, FAILED
};
enum class MaintReason : uint8_t { NONE, OTA, FACTORY_RESET, MANUAL };
enum class MaintResult : uint8_t { PENDING, ACK, FAILED, ABORTED };
enum class MaintAdmit : uint8_t { ACCEPTED, ALREADY, CONFLICT, NOT_READY };

// FirmwareUpdateManager phase, CAS-shared with the coordinator.
enum class FwPhase : uint8_t {
  IDLE, SEARCHING, READY_TO_UPDATE, QUIESCING, DOWNLOADING, INSTALLING, ERROR, ABORTED
};

// Loop-side actions. Every call happens on the loop task inside tick().
class MaintPort {
public:
  virtual ~MaintPort() {}
  virtual bool bleRunning() = 0;
  virtual void stopFans() = 0;            // non-blocking
  virtual void stopHrAdapter() = 0;
  virtual bool quiesceBle() = 0;          // true: a discovery was open or pending
  virtual bool discoveryDoneSeen() = 0;
  virtual bool bleQuiesced() = 0;
  virtual bool removeSensors() = 0;
  virtual void discardQueues() = 0;       // BLE + fan queues, both stopped for good
};

class MaintenanceLifecycle {
public:
  static constexpr uint32_t kRequestedMs = 500;
  static constexpr uint32_t kDiscoveryMs = 6000;
  static constexpr uint32_t kTotalMs = 15000;

  explicit MaintenanceLifecycle(uint32_t scansMs) : _scansMs(scansMs) {}

  // Given on every terminal exit (any task).
  void setSignal(void (*signal)()) { _signal = signal; }

  MaintAdmit request(MaintReason reason, uint32_t nowMs) {
    Guard g(_lock);
    return admitLocked(reason, nowMs);
  }

  // OTA download: lifecycle IDLE→REQUESTED and firmware READY_TO_UPDATE→QUIESCING
  // in one critical section.
  MaintAdmit requestDownload(std::atomic<uint8_t>& fwPhase, uint32_t nowMs) {
    Guard g(_lock);
    if (state() == MaintState::IDLE &&
        fwPhase.load() != static_cast<uint8_t>(FwPhase::READY_TO_UPDATE)) {
      return MaintAdmit::NOT_READY;
    }
    const MaintAdmit r = admitLocked(MaintReason::OTA, nowMs);
    if (r == MaintAdmit::ACCEPTED) fwPhase.store(static_cast<uint8_t>(FwPhase::QUIESCING));
    return r;
  }

  // Download job could not be enqueued: no rollback, the loop may already quiesce.
  void failAdmission() {
    _state.store(static_cast<uint8_t>(MaintState::FAILED));
    finish(MaintResult::FAILED);
  }

  // Firmware abort: QUIESCING→ABORTED only. Returns false if the CAS failed.
  bool abortQuiescing(std::atomic<uint8_t>& fwPhase) {
    uint8_t expected = static_cast<uint8_t>(FwPhase::QUIESCING);
    if (!fwPhase.compare_exchange_strong(expected, static_cast<uint8_t>(FwPhase::ABORTED))) {
      return false;
    }
    finish(MaintResult::ABORTED);
    return true;
  }

  // Worker after the semaphore (or its timeout): download only on ACK and a
  // successful QUIESCING→DOWNLOADING CAS; otherwise restart_required.
  bool workerMayDownload(std::atomic<uint8_t>& fwPhase) {
    if (result() == MaintResult::ACK) {
      uint8_t expected = static_cast<uint8_t>(FwPhase::QUIESCING);
      if (fwPhase.compare_exchange_strong(expected, static_cast<uint8_t>(FwPhase::DOWNLOADING))) {
        return true;
      }
    }
    _restartRequired.store(true);
    return false;
  }

  void tick(uint32_t nowMs, MaintPort& port) {
    const MaintState s = state();
    if (s == MaintState::IDLE || s == MaintState::ACKED || s == MaintState::FAILED) return;

    if (s == MaintState::REQUESTED) {
      port.stopFans();
      port.stopHrAdapter();
      if (!port.bleRunning()) {
        ack(port);
        return;
      }
      _discoveryWasOpen = port.quiesceBle();
      if (nowMs - _requestedAtMs > kRequestedMs) {
        fail(port);
        return;
      }
      enterStep(MaintState::STOPPING_DISCOVERY, nowMs);
    }
    if (nowMs - _requestedAtMs > kTotalMs) {
      fail(port);
      return;
    }
    if (state() == MaintState::STOPPING_DISCOVERY) {
      if (_discoveryWasOpen && !port.discoveryDoneSeen()) {
        if (nowMs - _stepStartMs > kDiscoveryMs) fail(port);
        return;
      }
      enterStep(MaintState::STOPPING_SCANS, nowMs);
    }
    if (state() == MaintState::STOPPING_SCANS) {
      if (!port.bleQuiesced()) {
        if (nowMs - _stepStartMs > _scansMs) fail(port);
        return;
      }
      enterStep(MaintState::REMOVING, nowMs);
    }
    if (state() == MaintState::REMOVING) {
      if (port.removeSensors()) {
        ack(port);
      } else {
        fail(port);
      }
    }
  }

  MaintState state() const { return static_cast<MaintState>(_state.load()); }
  MaintReason reason() const { return static_cast<MaintReason>(_reason.load()); }
  MaintResult result() const { return static_cast<MaintResult>(_result.load()); }
  bool admissionClosed() const { return state() != MaintState::IDLE; }
  bool restartRequired() const { return _restartRequired.load(); }
  void setRestartRequired() { _restartRequired.store(true); }

private:
#if defined(ESP_PLATFORM)
  struct Lock {
    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    void lock() { portENTER_CRITICAL(&mux); }
    void unlock() { portEXIT_CRITICAL(&mux); }
  };
#else
  struct Lock {
    std::mutex m;
    void lock() { m.lock(); }
    void unlock() { m.unlock(); }
  };
#endif
  struct Guard {
    explicit Guard(Lock& l) : _l(l) { _l.lock(); }
    ~Guard() { _l.unlock(); }
    Lock& _l;
  };

  Lock _lock;
  std::atomic<uint8_t> _state{static_cast<uint8_t>(MaintState::IDLE)};
  std::atomic<uint8_t> _reason{static_cast<uint8_t>(MaintReason::NONE)};
  std::atomic<uint8_t> _result{static_cast<uint8_t>(MaintResult::PENDING)};
  std::atomic<bool> _restartRequired{false};
  void (*_signal)() = nullptr;
  uint32_t _scansMs;
  uint32_t _requestedAtMs = 0;
  uint32_t _stepStartMs = 0;
  bool _discoveryWasOpen = false;

  MaintAdmit admitLocked(MaintReason reason, uint32_t nowMs) {
    if (state() != MaintState::IDLE) {
      return (this->reason() == reason) ? MaintAdmit::ALREADY : MaintAdmit::CONFLICT;
    }
    _reason.store(static_cast<uint8_t>(reason));
    _result.store(static_cast<uint8_t>(MaintResult::PENDING));
    _requestedAtMs = nowMs;
    _state.store(static_cast<uint8_t>(MaintState::REQUESTED));
    return MaintAdmit::ACCEPTED;
  }

  void enterStep(MaintState s, uint32_t nowMs) {
    _stepStartMs = nowMs;
    _state.store(static_cast<uint8_t>(s));
  }

  // First terminal result wins (an abort is not overwritten by a later ACK).
  void finish(MaintResult r) {
    uint8_t expected = static_cast<uint8_t>(MaintResult::PENDING);
    _result.compare_exchange_strong(expected, static_cast<uint8_t>(r));
    if (r != MaintResult::ACK) _restartRequired.store(true);
    if (_signal) _signal();
  }

  void ack(MaintPort& port) {
    port.discardQueues();
    _state.store(static_cast<uint8_t>(MaintState::ACKED));
    finish(MaintResult::ACK);
  }

  void fail(MaintPort& port) {
    port.discardQueues();
    _state.store(static_cast<uint8_t>(MaintState::FAILED));
    finish(MaintResult::FAILED);
  }
};

}  // namespace hw

#endif  // HOMEWIND_MAINTENANCE_LIFECYCLE_H
