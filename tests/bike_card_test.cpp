// Host test for src/core/BikeCard.h (display cadence card: CSC and power meter).
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

// Arguments: cscConfigured, cscConnected, pwrConfigured, pwrConnected

static void testNothingConfigured() {
  const BikeCard c = selectBikeCard(false, false, false, false);
  CHECK(c.state == BikeCardState::NotConfigured);
  CHECK(!c.showPower);
}

static void testCscOnly() {
  BikeCard c = selectBikeCard(true, true, false, false);
  CHECK(c.state == BikeCardState::Active && !c.nameFromPower && !c.showPower);
  c = selectBikeCard(true, false, false, false);
  CHECK(c.state == BikeCardState::Inactive && !c.nameFromPower && !c.showPower);
}

static void testPowerOnly() {
  BikeCard c = selectBikeCard(false, false, true, true);
  CHECK(c.state == BikeCardState::Active && c.nameFromPower && c.showPower);
  c = selectBikeCard(false, false, true, false);
  CHECK(c.state == BikeCardState::Inactive && c.nameFromPower && !c.showPower);
}

static void testCscAndPower() {
  // Power meter connected: its name, power shown
  BikeCard c = selectBikeCard(true, true, true, true);
  CHECK(c.state == BikeCardState::Active && c.nameFromPower && c.showPower);
  c = selectBikeCard(true, false, true, true);
  CHECK(c.state == BikeCardState::Active && c.nameFromPower && c.showPower);
  // Power meter not connected: CSC name, no power
  c = selectBikeCard(true, true, true, false);
  CHECK(c.state == BikeCardState::Active && !c.nameFromPower && !c.showPower);
  c = selectBikeCard(true, false, true, false);
  CHECK(c.state == BikeCardState::Inactive && !c.nameFromPower && !c.showPower);
}

// Arguments: showPower, powerValid, powerW
static void testWatts() {
  CHECK(bikeCardWatts(false, true, 210) == -1);
  CHECK(bikeCardWatts(true, true, 210) == 210);
  CHECK(bikeCardWatts(true, true, 0) == 0);    // valid but stale (BIKE_METRICS sends 0)
  CHECK(bikeCardWatts(true, true, -5) == 0);
  CHECK(bikeCardWatts(true, true, 1999) == 1999);
  // Connected, no valid sample yet: hidden
  CHECK(bikeCardWatts(true, false, 0) == -1);
  CHECK(bikeCardWatts(false, false, 0) == -1);
}

int main() {
  testNothingConfigured();
  testCscOnly();
  testPowerOnly();
  testCscAndPower();
  testWatts();
  if (g_failures) {
    std::printf("bike_card_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("bike_card_test: OK\n");
  return EXIT_SUCCESS;
}
