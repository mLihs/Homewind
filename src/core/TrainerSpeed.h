/**
 * @file TrainerSpeed.h
 * @brief Arduino-free trainer speed from Cycling Power Measurement (0x2A63) wheel data
 *
 * Like the Wahoo app: wheel revolutions times a fixed 2096 mm (700x23c).
 * Shared by BLERelayManager and tests/trainer_speed_test.cpp.
 */

#ifndef HOMEWIND_TRAINER_SPEED_H
#define HOMEWIND_TRAINER_SPEED_H

#include <cstdint>

namespace hw {

constexpr uint32_t kWheelCircumferenceMm = 2096;
// Without a new wheel event for this long the speed is 0. A packet after a
// longer gap (reconnect, counter restart) only sets the baseline.
constexpr uint32_t kWheelStaleMs = 3000;
// More revolutions between two events (over 250 km/h within the stale time)
// is a counter restart on a live link: baseline only.
constexpr uint32_t kWheelMaxRevs = 100;

// CPS flags: bit0 pedal balance (1 B), bit2 accumulated torque (2 B), bit4
// wheel revolution data: cumulative revs (uint32) + last event (uint16, 1/2048 s).
inline bool parseCpsWheel(const uint8_t* d, uint8_t len, uint32_t& revs, uint16_t& eventTime) {
  if (d == nullptr || len < 4) return false;
  const uint16_t flags = static_cast<uint16_t>(d[0] | (d[1] << 8));
  if ((flags & 0x10) == 0) return false;
  uint8_t i = 4;
  if (flags & 0x01) i += 1;
  if (flags & 0x04) i += 2;
  if (len < i + 6) return false;
  revs = static_cast<uint32_t>(d[i]) | (static_cast<uint32_t>(d[i + 1]) << 8) |
         (static_cast<uint32_t>(d[i + 2]) << 16) | (static_cast<uint32_t>(d[i + 3]) << 24);
  eventTime = static_cast<uint16_t>(d[i + 4] | (d[i + 5] << 8));
  return true;
}

struct WheelSpeed {
  bool valid = false;          // the trainer has sent wheel data
  bool baseline = false;
  uint32_t revs = 0;
  uint16_t eventTime = 0;
  uint32_t packetMs = 0;       // last packet with wheel data
  uint32_t eventMs = 0;        // last packet with a new wheel event
  uint16_t speedDkmh = 0;      // 0.1 km/h
};

// One 0x2A63 packet; packets without wheel data are ignored.
inline void wheelSpeedUpdate(WheelSpeed& s, const uint8_t* d, uint8_t len, uint32_t nowMs) {
  uint32_t revs = 0;
  uint16_t t = 0;
  if (!parseCpsWheel(d, len, revs, t)) return;
  const uint32_t dRevs = revs - s.revs;
  const uint16_t dT = static_cast<uint16_t>(t - s.eventTime);
  const bool linked = s.baseline && nowMs - s.packetMs <= kWheelStaleMs;
  s.valid = true;
  s.packetMs = nowMs;
  if (linked && dT == 0) return;  // no new wheel event
  // The event time wraps every 32 s: events further apart than the stale
  // time (a pause) restart from a baseline.
  const bool fresh = linked && dT <= kWheelStaleMs * 2048u / 1000u && dRevs <= kWheelMaxRevs;
  if (fresh) {
    const uint64_t v = (static_cast<uint64_t>(dRevs) * kWheelCircumferenceMm * 73728u + 500u * dT) /
                       (1000u * static_cast<uint64_t>(dT));
    s.speedDkmh = v > 0xFFFF ? 0xFFFF : static_cast<uint16_t>(v);
  } else {
    s.speedDkmh = 0;
  }
  s.eventMs = nowMs;
  s.baseline = true;
  s.revs = revs;
  s.eventTime = t;
}

// 0.1 km/h; 0 when the last wheel event is older than kWheelStaleMs.
inline uint16_t wheelSpeedDkmh(const WheelSpeed& s, uint32_t nowMs) {
  if (!s.valid || nowMs - s.eventMs > kWheelStaleMs) return 0;
  return s.speedDkmh;
}

}  // namespace hw

#endif  // HOMEWIND_TRAINER_SPEED_H
