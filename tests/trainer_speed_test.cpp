// Host test for src/core/TrainerSpeed.h (trainer speed from 0x2A63 wheel data).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/trainer_speed_test.cpp -o <out> && <out>

#include "TrainerSpeed.h"

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

// flags 0x0010 (wheel only) or with balance/torque fields in front.
static uint8_t packet(uint8_t* b, uint16_t flags, uint32_t revs, uint16_t t) {
  uint8_t n = 0;
  b[n++] = static_cast<uint8_t>(flags);
  b[n++] = static_cast<uint8_t>(flags >> 8);
  b[n++] = 200;  // power LE
  b[n++] = 0;
  if (flags & 0x01) b[n++] = 0x7F;
  if (flags & 0x04) { b[n++] = 0x11; b[n++] = 0x22; }
  if (flags & 0x10) {
    for (int i = 0; i < 4; ++i) b[n++] = static_cast<uint8_t>(revs >> (8 * i));
    b[n++] = static_cast<uint8_t>(t);
    b[n++] = static_cast<uint8_t>(t >> 8);
  }
  if (flags & 0x20) { b[n++] = 1; b[n++] = 0; b[n++] = 2; b[n++] = 0; }  // crank data after wheel
  return n;
}

static void feed(WheelSpeed& s, uint16_t flags, uint32_t revs, uint16_t t, uint32_t nowMs) {
  uint8_t b[32];
  const uint8_t n = packet(b, flags, revs, t);
  wheelSpeedUpdate(s, b, n, nowMs);
}

static void testParseOffsets() {
  uint8_t b[32];
  uint32_t revs = 0;
  uint16_t t = 0;
  const uint16_t flagSets[] = {0x0010, 0x0011, 0x0014, 0x0015, 0x0034};
  for (uint16_t flags : flagSets) {
    const uint8_t n = packet(b, flags, 0x01020304, 0xABCD);
    CHECK(parseCpsWheel(b, n, revs, t));
    CHECK(revs == 0x01020304 && t == 0xABCD);
  }
  CHECK(!parseCpsWheel(b, packet(b, 0x0020, 1, 1), revs, t));   // crank only
  const uint8_t n = packet(b, 0x0014, 5, 6);
  CHECK(!parseCpsWheel(b, n - 1, revs, t));                     // truncated
  CHECK(!parseCpsWheel(nullptr, 10, revs, t));
}

static void testSpeed() {
  WheelSpeed s;
  feed(s, 0x10, 100, 1000, 10000);
  CHECK(s.valid && wheelSpeedDkmh(s, 10000) == 0);              // baseline
  // 4 revs in 1 s: 4 * 2.096 m/s = 30.18 km/h
  feed(s, 0x10, 104, 1000 + 2048, 11000);
  CHECK(wheelSpeedDkmh(s, 11000) == 302);
  // Same event again: value kept until stale
  feed(s, 0x10, 104, 1000 + 2048, 11500);
  CHECK(wheelSpeedDkmh(s, 11500) == 302);
  CHECK(wheelSpeedDkmh(s, 11000 + kWheelStaleMs) == 302);
  CHECK(wheelSpeedDkmh(s, 11000 + kWheelStaleMs + 1) == 0);
  CHECK(s.valid);
}

static void testCounterWrap() {
  WheelSpeed s;
  feed(s, 0x10, 0xFFFFFFFE, 0xFF00, 0);
  // revs and event time both wrap: 4 revs in 0.5 s = 60.36 km/h
  feed(s, 0x10, 2, static_cast<uint16_t>(0xFF00 + 1024), 500);
  CHECK(wheelSpeedDkmh(s, 500) == 604);
}

static void testPauseAndReconnect() {
  WheelSpeed s;
  feed(s, 0x10, 0, 0, 0);
  feed(s, 0x10, 4, 2048, 1000);
  CHECK(wheelSpeedDkmh(s, 1000) == 302);
  // Wheel stopped for 40 s (same event, packets keep coming), then moves:
  // the 40 s gap wraps the event time, so only a new baseline
  for (uint32_t ms = 2000; ms <= 41000; ms += 1000) feed(s, 0x10, 4, 2048, ms);
  feed(s, 0x10, 5, static_cast<uint16_t>(2048 + 40 * 2048), 41000);
  CHECK(wheelSpeedDkmh(s, 41000) == 0);
  feed(s, 0x10, 9, static_cast<uint16_t>(2048 + 41 * 2048), 42000);
  CHECK(wheelSpeedDkmh(s, 42000) == 302);
  // Link gap (reconnect, counters restart): baseline, no huge value
  feed(s, 0x10, 3, 500, 50000);
  CHECK(s.valid && wheelSpeedDkmh(s, 50000) == 0);
  feed(s, 0x10, 7, 500 + 2048, 51000);
  CHECK(wheelSpeedDkmh(s, 51000) == 302);
}

static void testCounterRestartOnLiveLink() {
  WheelSpeed s;
  feed(s, 0x10, 5000, 0, 0);
  feed(s, 0x10, 5004, 2048, 1000);
  CHECK(wheelSpeedDkmh(s, 1000) == 302);
  // Trainer restarts its counters without a link gap: no huge value, new baseline
  feed(s, 0x10, 2, 4096, 2000);
  CHECK(wheelSpeedDkmh(s, 2000) == 0);
  feed(s, 0x10, 6, 4096 + 2048, 3000);
  CHECK(wheelSpeedDkmh(s, 3000) == 302);
  // 100 revs in 3 s (251.5 km/h) is still a reading
  feed(s, 0x10, 106, 4096 + 2048 + 6144, 6000);
  CHECK(wheelSpeedDkmh(s, 6000) == 2515);
}

static void testNoWheelData() {
  WheelSpeed s;
  feed(s, 0x20, 0, 0, 0);                                       // trainer without wheel data
  CHECK(!s.valid && wheelSpeedDkmh(s, 0) == 0);
  // Packets without wheel data between wheel packets are ignored
  feed(s, 0x10, 0, 0, 1000);
  feed(s, 0x20, 0, 0, 1500);
  feed(s, 0x10, 4, 2048, 2000);
  feed(s, 0x20, 0, 0, 2500);
  CHECK(s.valid && wheelSpeedDkmh(s, 2500) == 302);
  CHECK(!WheelSpeed{}.valid && wheelSpeedDkmh(WheelSpeed{}, 0) == 0);
}

int main() {
  testParseOffsets();
  testSpeed();
  testCounterWrap();
  testPauseAndReconnect();
  testCounterRestartOnLiveLink();
  testNoWheelData();
  if (g_failures) {
    std::printf("trainer_speed_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("trainer_speed_test: OK\n");
  return EXIT_SUCCESS;
}
