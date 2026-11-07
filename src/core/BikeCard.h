/**
 * @file BikeCard.h
 * @brief Arduino-free state of the display's cadence card (CSC and power meter)
 *
 * Shared by DisplayManager and tests/bike_card_test.cpp.
 */

#ifndef HOMEWIND_BIKE_CARD_H
#define HOMEWIND_BIKE_CARD_H

#include <cstdint>

namespace hw {

enum class BikeCardState : uint8_t { NotConfigured, Inactive, Active };

struct BikeCard {
  BikeCardState state;
  bool nameFromPower;  // show the power meter's name, else the CSC sensor's
  bool showPower;      // power meter connected
};

// A connected sensor is also configured.
inline BikeCard selectBikeCard(bool cscConfigured, bool cscConnected,
                               bool pwrConfigured, bool pwrConnected) {
  BikeCard c;
  if (!cscConfigured && !pwrConfigured) {
    c.state = BikeCardState::NotConfigured;
  } else {
    c.state = (cscConnected || pwrConnected) ? BikeCardState::Active : BikeCardState::Inactive;
  }
  c.nameFromPower = pwrConnected || !cscConfigured;
  c.showPower = pwrConnected;
  return c;
}

// Watts for homewind_set_csc_power(): -1 hides power (no meter connected or no
// valid sample yet, BIKE_METRICS power-valid flag); a negative reading shows 0.
inline int32_t bikeCardWatts(bool showPower, bool powerValid, int16_t powerW) {
  if (!showPower || !powerValid) return -1;
  return powerW < 0 ? 0 : powerW;
}

}  // namespace hw

#endif  // HOMEWIND_BIKE_CARD_H
