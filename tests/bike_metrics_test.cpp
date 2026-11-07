// Host test for src/core/BikeMetrics.h (WebSocket BIKE_METRICS 0x0D payload).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/bike_metrics_test.cpp -o <out> && <out>

#include "BikeMetrics.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

using namespace hw;

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

static void testEncodeLayout() {
  const BikeMetrics m = makeBikeMetrics(true, 0x0123, 0, true, 0x0258, 0, true);
  uint8_t buf[8];
  memset(buf, 0xAA, sizeof(buf));
  CHECK(encodeBikeMetrics(m, buf, sizeof(buf)) == 5);
  CHECK(buf[0] == 0x23 && buf[1] == 0x01);   // power LE
  CHECK(buf[2] == 0x58 && buf[3] == 0x02);   // cadence LE
  CHECK(buf[4] == (kBikePowerValid | kBikeCadenceValid | kBikeCadenceFromPower));
  CHECK(buf[5] == 0xAA);                     // nothing past the payload
}

static void testNegativePower() {
  uint8_t buf[5];
  CHECK(encodeBikeMetrics(makeBikeMetrics(true, -5, 0, false, 0, 0, false), buf, sizeof(buf)) == 5);
  CHECK(buf[0] == 0xFB && buf[1] == 0xFF);
  CHECK(buf[4] == kBikePowerValid);
}

static void testInvalidValuesAreZero() {
  const BikeMetrics m = makeBikeMetrics(false, 250, 0, false, 90, 0, false);
  CHECK(m.powerW == 0 && m.cadenceRpm == 0 && m.flags == 0);
  // Same validity, different stale readings: equal (no frame for a change of nothing)
  CHECK(makeBikeMetrics(false, 1, 0, false, 2, 0, false) == makeBikeMetrics(false, 3, 0, false, 4, 0, false));
  CHECK(BikeMetrics{} == makeBikeMetrics(false, 0, 0, false, 0, 0, false));
}

static void testChangeDetection() {
  const BikeMetrics a = makeBikeMetrics(true, 200, 0, true, 85, 0, false);
  CHECK(a != makeBikeMetrics(true, 201, 0, true, 85, 0, false));
  CHECK(a != makeBikeMetrics(true, 200, 0, true, 86, 0, false));
  CHECK(a != makeBikeMetrics(true, 200, 0, true, 85, 0, true));     // source changed
  CHECK(a != makeBikeMetrics(false, 0, 0, true, 85, 0, false));     // power lost
  // Stale cadence: valid with 0 rpm differs from invalid
  CHECK(makeBikeMetrics(true, 0, 0, true, 0, 0, true) != makeBikeMetrics(true, 0, 0, false, 0, 0, true));
}

static void testCadenceFromPowerWithoutCadence() {
  // Power meter is the source but has not sent a crank delta yet
  const BikeMetrics m = makeBikeMetrics(true, 150, 0, false, 0, 0, true);
  CHECK(m.flags == (kBikePowerValid | kBikeCadenceFromPower));
}

static void testStaleValues() {
  // Fresh and exactly kBikeStaleMs: value kept
  BikeMetrics m = makeBikeMetrics(true, 210, 0, true, 88, 0, false);
  CHECK(m.powerW == 210 && m.cadenceRpm == 88);
  m = makeBikeMetrics(true, 210, kBikeStaleMs, true, 88, kBikeStaleMs, false);
  CHECK(m.powerW == 210 && m.cadenceRpm == 88);
  // Older: 0, still flagged valid
  m = makeBikeMetrics(true, 210, kBikeStaleMs + 1, true, 88, 0, false);
  CHECK(m.powerW == 0 && m.cadenceRpm == 88);
  CHECK(m.flags == (kBikePowerValid | kBikeCadenceValid));
  m = makeBikeMetrics(true, 210, 0, true, 88, kBikeStaleMs + 1, true);
  CHECK(m.powerW == 210 && m.cadenceRpm == 0);
  CHECK(m.flags == (kBikePowerValid | kBikeCadenceValid | kBikeCadenceFromPower));
  // A stale reading differs from an invalid one
  CHECK(makeBikeMetrics(true, 210, 60000, false, 0, 0, false) !=
        makeBikeMetrics(false, 210, 60000, false, 0, 0, false));
}

static void testBufferTooSmall() {
  uint8_t buf[4] = {1, 2, 3, 4};
  CHECK(encodeBikeMetrics(makeBikeMetrics(true, 1, 0, true, 1, 0, true), buf, sizeof(buf)) == 0);
  CHECK(buf[0] == 1 && buf[3] == 4);
  CHECK(encodeBikeMetrics(BikeMetrics{}, nullptr, 16) == 0);
}

int main() {
  testEncodeLayout();
  testNegativePower();
  testInvalidValuesAreZero();
  testChangeDetection();
  testCadenceFromPowerWithoutCadence();
  testStaleValues();
  testBufferTooSmall();
  if (g_failures) {
    std::printf("bike_metrics_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("bike_metrics_test: OK\n");
  return EXIT_SUCCESS;
}
