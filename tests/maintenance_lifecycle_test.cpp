// Host test for src/core/MaintenanceLifecycle.h (design §7.4, §7.5).
// c++ -std=c++17 -Wall -Wextra -Werror -pthread -IHomewind/src/core \
//   Homewind/tests/maintenance_lifecycle_test.cpp -o <out> && <out>

#include "MaintenanceLifecycle.h"

#include <cstdio>
#include <cstdlib>

using namespace hw;

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

static int g_signals = 0;
static void onSignal() { g_signals++; }

struct FakePort : MaintPort {
  bool running = true;
  bool discoveryOpen = false;
  bool doneSeen = false;
  bool quiesced = false;
  bool removeOk = true;
  int fansStopped = 0, adapterStopped = 0, quiesceCalls = 0, removeCalls = 0, discards = 0;
  bool bleRunning() override { return running; }
  void stopFans() override { fansStopped++; }
  void stopHrAdapter() override { adapterStopped++; }
  bool quiesceBle() override { quiesceCalls++; return discoveryOpen; }
  bool discoveryDoneSeen() override { return doneSeen; }
  bool bleQuiesced() override { return quiesced; }
  bool removeSensors() override { removeCalls++; return removeOk; }
  void discardQueues() override { discards++; }
};

static constexpr uint32_t kScansMs = 8000;

static MaintenanceLifecycle* fresh() {
  g_signals = 0;
  MaintenanceLifecycle* lc = new MaintenanceLifecycle(kScansMs);
  lc->setSignal(onSignal);
  return lc;
}

static void testHappyPathSamePass() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.quiesced = true;
  CHECK(lc->request(MaintReason::MANUAL, 100) == MaintAdmit::ACCEPTED);
  CHECK(lc->admissionClosed());
  lc->tick(110, p);
  CHECK(lc->state() == MaintState::ACKED);
  CHECK(lc->result() == MaintResult::ACK);
  CHECK(!lc->restartRequired());
  CHECK(p.fansStopped == 1 && p.adapterStopped == 1 && p.quiesceCalls == 1);
  CHECK(p.removeCalls == 1 && p.discards == 1);
  CHECK(g_signals == 1);
  lc->tick(200, p);   // terminal: nothing more
  CHECK(p.discards == 1 && g_signals == 1);
  delete lc;
}

static void testSameAndDifferentReason() {
  MaintenanceLifecycle* lc = fresh();
  CHECK(lc->request(MaintReason::OTA, 0) == MaintAdmit::ACCEPTED);
  CHECK(lc->request(MaintReason::OTA, 1) == MaintAdmit::ALREADY);
  CHECK(lc->request(MaintReason::FACTORY_RESET, 2) == MaintAdmit::CONFLICT);
  CHECK(lc->reason() == MaintReason::OTA);
  delete lc;
}

static void testRequestedDeadline() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  CHECK(lc->request(MaintReason::MANUAL, 0) == MaintAdmit::ACCEPTED);
  lc->tick(MaintenanceLifecycle::kRequestedMs + 1, p);
  CHECK(lc->state() == MaintState::FAILED);
  CHECK(lc->result() == MaintResult::FAILED);
  CHECK(lc->restartRequired());
  CHECK(p.discards == 1 && g_signals == 1);
  delete lc;
}

static void testDiscoveryDeadline() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.discoveryOpen = true;
  p.quiesced = true;
  lc->request(MaintReason::MANUAL, 0);
  lc->tick(10, p);
  CHECK(lc->state() == MaintState::STOPPING_DISCOVERY);
  lc->tick(10 + MaintenanceLifecycle::kDiscoveryMs, p);
  CHECK(lc->state() == MaintState::STOPPING_DISCOVERY);
  lc->tick(11 + MaintenanceLifecycle::kDiscoveryMs, p);
  CHECK(lc->state() == MaintState::FAILED);
  CHECK(lc->restartRequired() && g_signals == 1);

  MaintenanceLifecycle* ok = fresh();
  FakePort q;
  q.discoveryOpen = true;
  q.quiesced = true;
  ok->request(MaintReason::MANUAL, 0);
  ok->tick(10, q);
  q.doneSeen = true;
  ok->tick(20, q);
  CHECK(ok->state() == MaintState::ACKED);
  delete lc;
  delete ok;
}

static void testScansDeadline() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  lc->request(MaintReason::MANUAL, 0);
  lc->tick(10, p);
  CHECK(lc->state() == MaintState::STOPPING_SCANS);
  lc->tick(10 + kScansMs, p);
  CHECK(lc->state() == MaintState::STOPPING_SCANS);
  lc->tick(11 + kScansMs, p);
  CHECK(lc->state() == MaintState::FAILED);
  delete lc;
}

static void testTotalDeadline() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.discoveryOpen = true;
  lc->request(MaintReason::MANUAL, 0);
  lc->tick(10, p);
  p.doneSeen = true;
  lc->tick(8000, p);                       // STOPPING_SCANS from 8000
  CHECK(lc->state() == MaintState::STOPPING_SCANS);
  lc->tick(MaintenanceLifecycle::kTotalMs, p);
  CHECK(lc->state() == MaintState::STOPPING_SCANS);
  lc->tick(MaintenanceLifecycle::kTotalMs + 1, p);   // scans step budget not used up
  CHECK(lc->state() == MaintState::FAILED);
  delete lc;
}

static void testRemoveFails() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.quiesced = true;
  p.removeOk = false;
  lc->request(MaintReason::FACTORY_RESET, 0);
  lc->tick(1, p);
  CHECK(lc->state() == MaintState::FAILED);
  CHECK(lc->result() == MaintResult::FAILED);
  delete lc;
}

static void testWrapAround() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.quiesced = true;
  const uint32_t t0 = 0xFFFFFF00u;
  lc->request(MaintReason::MANUAL, t0);
  lc->tick(t0 + 0x200u, p);                // wraps, 512 ms > 500 ms
  CHECK(lc->state() == MaintState::FAILED);
  MaintenanceLifecycle* ok = fresh();
  ok->request(MaintReason::MANUAL, t0);
  ok->tick(t0 + 0x150u, p);                // wraps, 336 ms
  CHECK(ok->state() == MaintState::ACKED);
  delete lc;
  delete ok;
}

static void testBleNotRunningAcksImmediately() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.running = false;
  lc->request(MaintReason::OTA, 0);
  lc->tick(1, p);
  CHECK(lc->state() == MaintState::ACKED);
  CHECK(lc->result() == MaintResult::ACK);
  CHECK(p.fansStopped == 1 && p.adapterStopped == 1);
  CHECK(p.quiesceCalls == 0 && p.removeCalls == 0);
  CHECK(p.discards == 1 && g_signals == 1);
  delete lc;
}

static void testDownloadAdmission() {
  MaintenanceLifecycle* lc = fresh();
  std::atomic<uint8_t> fw{static_cast<uint8_t>(FwPhase::IDLE)};
  CHECK(lc->requestDownload(fw, 0) == MaintAdmit::NOT_READY);
  CHECK(lc->state() == MaintState::IDLE);
  fw.store(static_cast<uint8_t>(FwPhase::READY_TO_UPDATE));
  CHECK(lc->requestDownload(fw, 0) == MaintAdmit::ACCEPTED);
  CHECK(fw.load() == static_cast<uint8_t>(FwPhase::QUIESCING));
  CHECK(lc->reason() == MaintReason::OTA);
  CHECK(lc->requestDownload(fw, 1) == MaintAdmit::ALREADY);
  CHECK(lc->request(MaintReason::FACTORY_RESET, 2) == MaintAdmit::CONFLICT);

  FakePort p;
  p.quiesced = true;
  lc->tick(5, p);
  CHECK(lc->result() == MaintResult::ACK);
  CHECK(lc->workerMayDownload(fw));
  CHECK(fw.load() == static_cast<uint8_t>(FwPhase::DOWNLOADING));
  CHECK(!lc->restartRequired());

  MaintenanceLifecycle* busy = fresh();
  CHECK(busy->request(MaintReason::MANUAL, 0) == MaintAdmit::ACCEPTED);
  std::atomic<uint8_t> fw2{static_cast<uint8_t>(FwPhase::READY_TO_UPDATE)};
  CHECK(busy->requestDownload(fw2, 1) == MaintAdmit::CONFLICT);
  CHECK(fw2.load() == static_cast<uint8_t>(FwPhase::READY_TO_UPDATE));
  delete lc;
  delete busy;
}

static void testAbortInQuiescing() {
  MaintenanceLifecycle* lc = fresh();
  std::atomic<uint8_t> fw{static_cast<uint8_t>(FwPhase::READY_TO_UPDATE)};
  lc->requestDownload(fw, 0);
  CHECK(lc->abortQuiescing(fw));
  CHECK(fw.load() == static_cast<uint8_t>(FwPhase::ABORTED));
  CHECK(lc->result() == MaintResult::ABORTED);
  CHECK(lc->restartRequired());
  CHECK(g_signals == 1);
  CHECK(!lc->abortQuiescing(fw));          // CAS-only: second abort does nothing
  CHECK(g_signals == 1);

  FakePort p;
  p.quiesced = true;
  lc->tick(5, p);                          // the loop still finishes the stop
  CHECK(lc->state() == MaintState::ACKED);
  CHECK(lc->result() == MaintResult::ABORTED);   // first terminal result wins
  CHECK(!lc->workerMayDownload(fw));
  CHECK(fw.load() == static_cast<uint8_t>(FwPhase::ABORTED));
  delete lc;
}

static void testAbortOutsideQuiescing() {
  MaintenanceLifecycle* lc = fresh();
  std::atomic<uint8_t> fw{static_cast<uint8_t>(FwPhase::DOWNLOADING)};
  CHECK(!lc->abortQuiescing(fw));
  CHECK(fw.load() == static_cast<uint8_t>(FwPhase::DOWNLOADING));
  delete lc;
}

static void testEnqueueFailure() {
  MaintenanceLifecycle* lc = fresh();
  std::atomic<uint8_t> fw{static_cast<uint8_t>(FwPhase::READY_TO_UPDATE)};
  CHECK(lc->requestDownload(fw, 0) == MaintAdmit::ACCEPTED);
  lc->failAdmission();
  CHECK(lc->state() == MaintState::FAILED);
  CHECK(lc->result() == MaintResult::FAILED);
  CHECK(lc->restartRequired());
  CHECK(lc->admissionClosed());
  CHECK(g_signals == 1);
  CHECK(lc->request(MaintReason::OTA, 1) == MaintAdmit::ALREADY);   // never back to IDLE
  delete lc;
}

static void testWorkerTimeoutWithoutAck() {
  MaintenanceLifecycle* lc = fresh();
  std::atomic<uint8_t> fw{static_cast<uint8_t>(FwPhase::READY_TO_UPDATE)};
  lc->requestDownload(fw, 0);
  CHECK(!lc->workerMayDownload(fw));       // result still PENDING
  CHECK(lc->restartRequired());
  CHECK(fw.load() == static_cast<uint8_t>(FwPhase::QUIESCING));
  delete lc;
}

// Fan admission: closed from REQUESTED on; queued work is discarded at ACK and FAILED.
static void testFanRejectAndDiscard() {
  MaintenanceLifecycle* lc = fresh();
  FakePort p;
  p.discoveryOpen = true;
  CHECK(!lc->admissionClosed());
  lc->request(MaintReason::MANUAL, 0);
  CHECK(lc->admissionClosed());
  lc->tick(1, p);
  CHECK(lc->admissionClosed() && p.discards == 0);
  p.doneSeen = true;
  p.quiesced = true;
  lc->tick(2, p);
  CHECK(lc->state() == MaintState::ACKED && p.discards == 1);
  CHECK(lc->admissionClosed());
  delete lc;
}

int main() {
  testHappyPathSamePass();
  testSameAndDifferentReason();
  testRequestedDeadline();
  testDiscoveryDeadline();
  testScansDeadline();
  testTotalDeadline();
  testRemoveFails();
  testWrapAround();
  testBleNotRunningAcksImmediately();
  testDownloadAdmission();
  testAbortInQuiescing();
  testAbortOutsideQuiescing();
  testEnqueueFailure();
  testWorkerTimeoutWithoutAck();
  testFanRejectAndDiscard();
  if (g_failures) {
    std::printf("maintenance_lifecycle_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("maintenance_lifecycle_test: OK\n");
  return EXIT_SUCCESS;
}
