/**
 * @file BikeCard.h
 * @brief Arduino-free state of the display's power and cadence rows
 *
 * Shared by DisplayManager and tests/bike_card_test.cpp.
 */

#ifndef HOMEWIND_BIKE_CARD_H
#define HOMEWIND_BIKE_CARD_H

#include <cstdint>

namespace hw {

enum class BikeRowState : uint8_t { Hidden, Disconnected, Active };
enum class BikeRowSource : uint8_t { None, Csc, Power, Trainer };

struct BikeRow {
  BikeRowState state;
  BikeRowSource source;  // whose name the row shows
};

// Inputs of both rows. A connected sensor is also configured. Trainer: configured =
// mirrored this boot, ready = connected and discovered. pwrCadence: the power meter
// delivered cadence this boot (BIKE_METRICS flag bit 2, latched by the caller).
struct BikeSources {
  bool cscConfigured = false;
  bool cscConnected = false;
  bool pwrConfigured = false;
  bool pwrConnected = false;
  bool pwrCadence = false;
  bool trainerConfigured = false;
  bool trainerReady = false;
};

// Power (D2): the power meter while connected, otherwise the trainer.
inline BikeRow selectPowerRow(const BikeSources& s) {
  if (!s.pwrConfigured && !s.trainerConfigured) {
    return {BikeRowState::Hidden, BikeRowSource::None};
  }
  if (s.pwrConnected) return {BikeRowState::Active, BikeRowSource::Power};
  if (s.trainerReady) return {BikeRowState::Active, BikeRowSource::Trainer};
  return {BikeRowState::Disconnected, s.pwrConfigured ? BikeRowSource::Power : BikeRowSource::Trainer};
}

// Cadence: CSC sensor, else power meter (only once it delivered cadence), else trainer.
inline BikeRow selectCadenceRow(const BikeSources& s) {
  const bool pwr = s.pwrConfigured && s.pwrCadence;
  if (!s.cscConfigured && !pwr && !s.trainerConfigured) {
    return {BikeRowState::Hidden, BikeRowSource::None};
  }
  if (s.cscConnected) return {BikeRowState::Active, BikeRowSource::Csc};
  if (pwr && s.pwrConnected) return {BikeRowState::Active, BikeRowSource::Power};
  if (s.trainerReady) return {BikeRowState::Active, BikeRowSource::Trainer};
  const BikeRowSource name =
      s.cscConfigured ? BikeRowSource::Csc : (pwr ? BikeRowSource::Power : BikeRowSource::Trainer);
  return {BikeRowState::Disconnected, name};
}

// Row value: 0 until a valid sample (BIKE_METRICS valid flag); a negative reading shows 0.
inline uint16_t bikeRowValue(bool valid, int16_t value) {
  return (!valid || value < 0) ? 0 : static_cast<uint16_t>(value);
}

}  // namespace hw

#endif  // HOMEWIND_BIKE_CARD_H
