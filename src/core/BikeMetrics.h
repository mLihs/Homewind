/**
 * @file BikeMetrics.h
 * @brief Arduino-free power/cadence state and its BIKE_METRICS (0x0D) payload
 *
 * Shared by BLERelayManager, WebSocketTelemetry and tests/bike_metrics_test.cpp.
 * Payload (5 bytes, little-endian): [power_w:int16][cadence_rpm:uint16][flags:uint8]
 */

#ifndef HOMEWIND_BIKE_METRICS_H
#define HOMEWIND_BIKE_METRICS_H

#include <cstddef>
#include <cstdint>

namespace hw {

constexpr uint8_t kBikePowerValid = 0x01;        // power_w is a power meter reading
constexpr uint8_t kBikeCadenceValid = 0x02;      // cadence_rpm comes from the cadence source
constexpr uint8_t kBikeCadenceFromPower = 0x04;  // the cadence source is the power meter
constexpr size_t kBikeMetricsPayloadLen = 5;
// A valid value older than this is sent as 0 (still flagged valid).
constexpr uint32_t kBikeStaleMs = 3000;

struct BikeMetrics {
  int16_t powerW = 0;
  uint16_t cadenceRpm = 0;
  uint8_t flags = 0;

  bool operator==(const BikeMetrics& o) const {
    return powerW == o.powerW && cadenceRpm == o.cadenceRpm && flags == o.flags;
  }
  bool operator!=(const BikeMetrics& o) const { return !(*this == o); }
};

// An invalid value is stored as 0, so equal readings compare equal. ageMs:
// time since the packet (power) or crank event (cadence) behind the value.
inline BikeMetrics makeBikeMetrics(bool powerValid, int16_t watts, uint32_t powerAgeMs,
                                   bool cadenceValid, uint16_t rpm, uint32_t cadenceAgeMs,
                                   bool cadenceFromPower) {
  BikeMetrics m;
  if (powerValid) {
    m.powerW = powerAgeMs > kBikeStaleMs ? 0 : watts;
    m.flags |= kBikePowerValid;
  }
  if (cadenceValid) {
    m.cadenceRpm = cadenceAgeMs > kBikeStaleMs ? 0 : rpm;
    m.flags |= kBikeCadenceValid;
  }
  if (cadenceFromPower) m.flags |= kBikeCadenceFromPower;
  return m;
}

// Returns the payload length, 0 if the buffer is too small.
inline size_t encodeBikeMetrics(const BikeMetrics& m, uint8_t* buf, size_t len) {
  if (buf == nullptr || len < kBikeMetricsPayloadLen) return 0;
  const uint16_t w = static_cast<uint16_t>(m.powerW);
  buf[0] = static_cast<uint8_t>(w & 0xFF);
  buf[1] = static_cast<uint8_t>(w >> 8);
  buf[2] = static_cast<uint8_t>(m.cadenceRpm & 0xFF);
  buf[3] = static_cast<uint8_t>(m.cadenceRpm >> 8);
  buf[4] = m.flags;
  return kBikeMetricsPayloadLen;
}

}  // namespace hw

#endif  // HOMEWIND_BIKE_METRICS_H
