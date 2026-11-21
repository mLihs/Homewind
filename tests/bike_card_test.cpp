// Host test for src/core/BikeCard.h (display power and cadence rows).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/bike_card_test.cpp -o <out> && <out>

#include "BikeCard.h"

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

static bool is(BikeRow r, BikeRowState state, BikeRowSource source) {
  return r.state == state && r.source == source;
}

static constexpr BikeRowState H = BikeRowState::Hidden;
static constexpr BikeRowState D = BikeRowState::Disconnected;
static constexpr BikeRowState A = BikeRowState::Active;

static void testNothingConfigured() {
  const BikeSources s;
  CHECK(is(selectPowerRow(s), H, BikeRowSource::None));
  CHECK(is(selectCadenceRow(s), H, BikeRowSource::None));
}

static void testCscOnly() {
  BikeSources s;
  s.cscConfigured = true;
  CHECK(is(selectPowerRow(s), H, BikeRowSource::None));
  CHECK(is(selectCadenceRow(s), D, BikeRowSource::Csc));
  s.cscConnected = true;
  CHECK(is(selectCadenceRow(s), A, BikeRowSource::Csc));
}

static void testPowerOnly() {
  BikeSources s;
  s.pwrConfigured = true;
  CHECK(is(selectPowerRow(s), D, BikeRowSource::Power));
  CHECK(is(selectCadenceRow(s), H, BikeRowSource::None));  // no cadence seen yet
  s.pwrConnected = true;
  CHECK(is(selectPowerRow(s), A, BikeRowSource::Power));
  CHECK(is(selectCadenceRow(s), H, BikeRowSource::None));
  // A meter with cadence (Assioma) keeps the row, also after a disconnect.
  s.pwrCadence = true;
  CHECK(is(selectCadenceRow(s), A, BikeRowSource::Power));
  s.pwrConnected = false;
  CHECK(is(selectPowerRow(s), D, BikeRowSource::Power));
  CHECK(is(selectCadenceRow(s), D, BikeRowSource::Power));
}

static void testCscAndPower() {
  BikeSources s;
  s.cscConfigured = s.pwrConfigured = s.pwrCadence = true;
  s.cscConnected = s.pwrConnected = true;
  CHECK(is(selectPowerRow(s), A, BikeRowSource::Power));
  CHECK(is(selectCadenceRow(s), A, BikeRowSource::Csc));
  // CSC lost: cadence falls back to the meter.
  s.cscConnected = false;
  CHECK(is(selectCadenceRow(s), A, BikeRowSource::Power));
  // Both lost: CSC name.
  s.pwrConnected = false;
  CHECK(is(selectCadenceRow(s), D, BikeRowSource::Csc));
}

static void testTrainer() {
  // Trainer alone: both rows show the trainer.
  BikeSources s;
  s.trainerConfigured = true;
  CHECK(is(selectPowerRow(s), D, BikeRowSource::Trainer));
  CHECK(is(selectCadenceRow(s), D, BikeRowSource::Trainer));
  s.trainerReady = true;
  CHECK(is(selectPowerRow(s), A, BikeRowSource::Trainer));
  CHECK(is(selectCadenceRow(s), A, BikeRowSource::Trainer));

  // D2: a connected power meter wins; while it is away the trainer gives power.
  BikeSources p;
  p.pwrConfigured = p.pwrConnected = p.trainerConfigured = p.trainerReady = true;
  CHECK(is(selectPowerRow(p), A, BikeRowSource::Power));
  CHECK(is(selectCadenceRow(p), A, BikeRowSource::Trainer));  // meter without cadence
  p.pwrConnected = false;
  CHECK(is(selectPowerRow(p), A, BikeRowSource::Trainer));
  p.trainerReady = false;
  CHECK(is(selectPowerRow(p), D, BikeRowSource::Power));
  CHECK(is(selectCadenceRow(p), D, BikeRowSource::Trainer));

  // CSC and trainer: CSC cadence, trainer power.
  BikeSources c;
  c.cscConfigured = c.cscConnected = c.trainerConfigured = c.trainerReady = true;
  CHECK(is(selectPowerRow(c), A, BikeRowSource::Trainer));
  CHECK(is(selectCadenceRow(c), A, BikeRowSource::Csc));
  c.cscConnected = false;
  CHECK(is(selectCadenceRow(c), A, BikeRowSource::Trainer));
  c.trainerReady = false;
  CHECK(is(selectCadenceRow(c), D, BikeRowSource::Csc));
}

static void testValue() {
  CHECK(bikeRowValue(true, 210) == 210);
  CHECK(bikeRowValue(true, 0) == 0);     // valid but stale (BIKE_METRICS sends 0)
  CHECK(bikeRowValue(true, -5) == 0);
  CHECK(bikeRowValue(true, 1999) == 1999);
  CHECK(bikeRowValue(false, 210) == 0);  // connected, no valid sample yet
}

int main() {
  testNothingConfigured();
  testCscOnly();
  testPowerOnly();
  testCscAndPower();
  testTrainer();
  testValue();
  if (g_failures) {
    std::printf("bike_card_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("bike_card_test: OK\n");
  return EXIT_SUCCESS;
}
