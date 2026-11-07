// Host test for src/core/BleCommandState.h (design §7.1, §7.2, §7.5).
// c++ -std=c++17 -Wall -Wextra -Werror -pthread -IHomewind/src/core \
//   Homewind/tests/ble_command_state_test.cpp -o <out> && <out>

#include "BleCommandState.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <strings.h>

using namespace hw;

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

// Mimics the library: one slot per type, the same MAC may not be held twice.
struct FakeLibrary : BleLibraryPort {
  bool slotUsed[2] = {false, false};
  char slotMac[2][18] = {};
  bool slotConnected[2] = {false, false};
  int adds = 0, removes = 0;

  bool connected(uint8_t t) override { return slotConnected[bleSlotOf(t)]; }
  bool add(uint8_t t, const char* mac, int8_t, const char*) override {
    adds++;
    const int i = bleSlotOf(t);
    const int other = 1 - i;
    if (slotUsed[other] && strcasecmp(slotMac[other], mac) == 0) return false;
    slotUsed[i] = true;
    strncpy(slotMac[i], mac, 17);
    slotMac[i][17] = '\0';
    slotConnected[i] = false;
    return true;
  }
  void remove(uint8_t t) override {
    removes++;
    const int i = bleSlotOf(t);
    slotUsed[i] = false;
    slotMac[i][0] = '\0';
    slotConnected[i] = false;
  }
};

static const char* kMacA = "aa:bb:cc:dd:ee:01";
static const char* kMacB = "aa:bb:cc:dd:ee:02";

static void testRestoreDuplicate() {
  // NVS restore applies HR first; CSC with the same MAC becomes a duplicate.
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeHR, "HR", kMacA, 0, true, false);
  r.setTarget(kBleTypeCSC, "CSC", kMacA, 0, true, false);
  r.run(lib);
  CHECK(r.added(kBleTypeHR));
  CHECK(!r.added(kBleTypeCSC));
  CHECK(!r.duplicate(kBleTypeHR));
  CHECK(r.duplicate(kBleTypeCSC));
  CHECK(!r.pending());
  // No retry loop: a further run without changes calls nothing.
  const int adds = lib.adds;
  r.run(lib);
  CHECK(lib.adds == adds);
}

static void testSetSensorDuplicateRemovesOldSlot() {
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeHR, "HR", kMacA, 0, true, false);
  r.setTarget(kBleTypeCSC, "CSC", kMacB, 1, true, false);
  r.run(lib);
  CHECK(r.added(kBleTypeHR) && r.added(kBleTypeCSC));
  // User picks the HR address for CSC: rejected, old CSC slot is removed.
  r.setTarget(kBleTypeCSC, "CSC2", kMacA, 0, true, false);
  r.run(lib);
  CHECK(!r.added(kBleTypeCSC));
  CHECK(r.duplicate(kBleTypeCSC));
  CHECK(!lib.slotUsed[1]);
  CHECK(r.added(kBleTypeHR));
}

static void testRetryAfterOtherRemoved() {
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeHR, "HR", kMacA, 0, true, false);
  r.setTarget(kBleTypeCSC, "CSC", kMacA, 0, true, false);
  r.run(lib);
  CHECK(r.duplicate(kBleTypeCSC));
  // HR is deleted: CSC is retried in the same run and succeeds.
  r.setTarget(kBleTypeHR, "", "", -1, false, false);
  r.run(lib);
  CHECK(!r.added(kBleTypeHR));
  CHECK(r.added(kBleTypeCSC));
  CHECK(!r.duplicate(kBleTypeCSC));
  CHECK(lib.slotUsed[1] && strcmp(lib.slotMac[1], kMacA) == 0);
}

static void testRetryAfterOtherChangedMac() {
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeHR, "HR", kMacA, 0, true, false);
  r.setTarget(kBleTypeCSC, "CSC", kMacA, 0, true, false);
  r.run(lib);
  CHECK(r.duplicate(kBleTypeCSC));
  r.setTarget(kBleTypeHR, "HR2", kMacB, 0, true, false);
  r.run(lib);
  CHECK(r.added(kBleTypeHR) && r.added(kBleTypeCSC));
  CHECK(!r.duplicate(kBleTypeCSC));
}

static void testApplyOnDisconnectAndForce() {
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeHR, "HR", kMacA, 0, true, false);
  r.run(lib);
  lib.slotConnected[0] = true;
  r.setTarget(kBleTypeHR, "HR2", kMacB, 0, true, false);
  r.run(lib);
  CHECK(r.pending());                       // deferred while connected
  CHECK(strcmp(lib.slotMac[0], kMacA) == 0);
  lib.slotConnected[0] = false;
  r.run(lib);
  CHECK(!r.pending());
  CHECK(strcmp(lib.slotMac[0], kMacB) == 0);

  lib.slotConnected[0] = true;
  r.setTarget(kBleTypeHR, "HR3", kMacA, 0, true, true);
  r.run(lib);
  CHECK(!r.pending());
  CHECK(strcmp(lib.slotMac[0], kMacA) == 0);

  lib.slotConnected[0] = true;
  r.setTarget(kBleTypeHR, "", "", -1, false, false);
  r.run(lib);
  CHECK(r.pending() && lib.slotUsed[0]);
  lib.slotConnected[0] = false;
  r.run(lib);
  CHECK(!r.pending() && !lib.slotUsed[0]);
}

static void testAddrTypeChangeReapplies() {
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeCSC, "CSC", kMacA, 0, true, false);
  r.run(lib);
  const int adds = lib.adds;
  r.setTarget(kBleTypeCSC, "CSC", "AA:BB:CC:DD:EE:01", 0, true, false);
  r.run(lib);
  CHECK(lib.adds == adds);                  // same identity: no re-add
  r.setTarget(kBleTypeCSC, "CSC", kMacA, 1, true, false);
  r.run(lib);
  CHECK(lib.adds == adds + 1);
}

static void testAdoptApplied() {
  static const char* kNewMac = "f2:23:97:c6:37:12";
  BleReconciler r;
  FakeLibrary lib;
  CHECK(!r.adoptApplied(kBleTypeCSC, kNewMac, 1));          // not added
  r.setTarget(kBleTypeCSC, "CAD", kMacB, 1, true, false);
  r.run(lib);
  lib.slotConnected[1] = true;
  CHECK(!r.adoptApplied(kBleTypeCSC, "bad", 1));
  CHECK(r.adoptApplied(kBleTypeCSC, kNewMac, 1));
  CHECK(!r.pending());
  // The NVS restore of the new address is the applied identity: no re-add.
  const int adds = lib.adds, removes = lib.removes;
  r.setTarget(kBleTypeCSC, "CAD", "F2:23:97:C6:37:12", 1, true, false);
  lib.slotConnected[1] = false;
  r.run(lib);
  CHECK(lib.adds == adds && lib.removes == removes && !r.pending());
  // A pending different target is kept.
  lib.slotConnected[1] = true;
  r.setTarget(kBleTypeCSC, "CSC2", kMacA, 0, true, false);
  r.run(lib);
  CHECK(r.pending());
  CHECK(!r.adoptApplied(kBleTypeCSC, kMacB, 1));
  lib.slotConnected[1] = false;
  r.run(lib);
  CHECK(!r.pending() && strcmp(lib.slotMac[1], kMacA) == 0);
}

static void testAdoptRetriesOtherDuplicate() {
  static const char* kNewMac = "f2:23:97:c6:37:12";
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeCSC, "CAD", kMacB, 1, true, false);
  r.run(lib);
  r.setTarget(kBleTypeHR, "HR", kMacB, 1, true, false);
  r.run(lib);
  CHECK(r.duplicate(kBleTypeHR) && !r.pending());
  // The library followed CSC to a new address: HR is retried and gets the old one.
  strcpy(lib.slotMac[1], kNewMac);
  CHECK(r.adoptApplied(kBleTypeCSC, kNewMac, 1));
  CHECK(r.pending());
  r.run(lib);
  CHECK(r.added(kBleTypeHR) && !r.duplicate(kBleTypeHR) && !r.pending());
  CHECK(r.added(kBleTypeCSC) && strcmp(lib.slotMac[1], kNewMac) == 0);
}

static void testCommandQueue() {
  BleCommandQueue q;
  BleCommand c = {};
  for (uint8_t i = 0; i < BleCommandQueue::kCapacity; ++i) {
    c.kind = BleCmdKind::SET_FROM_DISCOVERY;
    c.set.generation = 7;
    c.set.index = i;
    c.set.force = (i & 1) != 0;
    CHECK(q.push(c));
  }
  CHECK(!q.push(c));                        // full -> HTTP 503
  BleCommand out = {};
  CHECK(q.pop(out) && out.set.index == 0 && !out.set.force);
  c.kind = BleCmdKind::DELETE;
  strcpy(c.name, "Polar H10");
  CHECK(q.push(c));
  for (uint8_t i = 1; i < BleCommandQueue::kCapacity; ++i) {
    CHECK(q.pop(out) && out.kind == BleCmdKind::SET_FROM_DISCOVERY && out.set.index == i);
  }
  CHECK(q.pop(out) && out.kind == BleCmdKind::DELETE && strcmp(out.name, "Polar H10") == 0);
  CHECK(!q.pop(out));
}

static void testDiscoveryQueue() {
  BleDiscoveryQueue q;
  BleDiscOp op;
  q.requestStart(kBleTypeHR);
  q.requestStart(kBleTypeCSC);              // replaces the pending START mask
  CHECK(q.size() == 1 && q.peek(op) && op == BleDiscOp::START && q.mask() == kBleTypeCSC);
  q.requestStop(false);                     // cancels the START, nothing open
  CHECK(q.size() == 0);

  q.requestStop(true);
  CHECK(q.size() == 1 && q.peek(op) && op == BleDiscOp::STOP);
  q.requestStart(kBleTypeHR);
  CHECK(q.size() == 2);
  q.requestStop(true);                      // [STOP, START] -> [STOP]
  CHECK(q.size() == 1 && q.peek(op) && op == BleDiscOp::STOP);
  q.requestStop(true);
  CHECK(q.size() == 1);
  q.requestStart(kBleTypeHR | kBleTypeCSC);
  q.pop();
  CHECK(q.size() == 1 && q.peek(op) && op == BleDiscOp::START && q.mask() == 3);
  q.pop();
  CHECK(q.size() == 0 && !q.peek(op));
  q.requestStop(false);
  CHECK(q.size() == 0);
}

static void testInvalidAddressNeverReachesLibrary() {
  CHECK(bleAddressValid("AA:bb:cc:dd:ee:01", 1));
  CHECK(bleAddressValid(kMacA, -1));
  CHECK(!bleAddressValid("aa:bb:cc:dd:ee", 0));
  CHECK(!bleAddressValid("aa:bb:cc:dd:ee:01:", 0));
  CHECK(!bleAddressValid("aa-bb-cc-dd-ee-01", 0));
  CHECK(!bleAddressValid("gg:bb:cc:dd:ee:01", 0));
  CHECK(!bleAddressValid(kMacA, 2));
  CHECK(!bleAddressValid("", 0));
  // An invalid NVS entry is skipped: no add, no duplicate status.
  BleReconciler r;
  FakeLibrary lib;
  r.setTarget(kBleTypeCSC, "CSC", "not-a-mac", 0, true, false);
  r.setTarget(kBleTypeHR, "HR", kMacA, 5, true, false);
  r.run(lib);
  CHECK(lib.adds == 0);
  CHECK(!r.duplicate(kBleTypeCSC) && !r.duplicate(kBleTypeHR));
  CHECK(!r.pending());
}

int main() {
  testInvalidAddressNeverReachesLibrary();
  testRestoreDuplicate();
  testSetSensorDuplicateRemovesOldSlot();
  testRetryAfterOtherRemoved();
  testRetryAfterOtherChangedMac();
  testApplyOnDisconnectAndForce();
  testAddrTypeChangeReapplies();
  testAdoptApplied();
  testAdoptRetriesOtherDuplicate();
  testCommandQueue();
  testDiscoveryQueue();
  if (g_failures) {
    std::printf("ble_command_state_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("ble_command_state_test: OK\n");
  return EXIT_SUCCESS;
}
