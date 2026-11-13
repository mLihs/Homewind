/**
 * @file FanControlFrame.h
 * @brief Arduino-free FAN_CONTROL (0x0E) status and payload
 *
 * docs/FAN_POWER_HR_CONCEPT.md §5. Shared by FanHRAdapter, WebSocketTelemetry
 * and tests/fan_control_frame_test.cpp.
 * Payload (10 bytes, little-endian): [mode][effective][reason][target_on]
 * [target_pct][dominant][power_state][ftp_w:uint16][cooldown:int8]
 */

#ifndef HOMEWIND_FAN_CONTROL_FRAME_H
#define HOMEWIND_FAN_CONTROL_FRAME_H

#include "FanBlend.h"

#include <cstddef>
#include <cstdint>

namespace hw {

constexpr size_t kFanControlPayloadLen = 10;

struct FanControlStatus {
  FanMode mode = FanMode::Pure;             // stored mode
  FanMode effective = FanMode::Pure;        // Hybrid can run as Smart
  FanModeReason reason = FanModeReason::None;
  bool targetOn = false;                    // acknowledged fan state (FanHRAdapter)
  uint8_t targetPct = 0;
  FanDominant dominant = FanDominant::Hr;
  FanPowerState power = FanPowerState::None;
  uint16_t ftpW = 0;
  int8_t cooldown = 0;

  bool operator==(const FanControlStatus& o) const {
    return mode == o.mode && effective == o.effective && reason == o.reason &&
           targetOn == o.targetOn && targetPct == o.targetPct && dominant == o.dominant &&
           power == o.power && ftpW == o.ftpW && cooldown == o.cooldown;
  }
  bool operator!=(const FanControlStatus& o) const { return !(*this == o); }
};

// Returns the payload length, 0 if the buffer is too small.
inline size_t encodeFanControl(const FanControlStatus& s, uint8_t* buf, size_t len) {
  if (buf == nullptr || len < kFanControlPayloadLen) return 0;
  buf[0] = static_cast<uint8_t>(s.mode);
  buf[1] = static_cast<uint8_t>(s.effective);
  buf[2] = static_cast<uint8_t>(s.reason);
  buf[3] = s.targetOn ? 1 : 0;
  buf[4] = s.targetOn ? s.targetPct : 0;
  buf[5] = static_cast<uint8_t>(s.dominant);
  buf[6] = static_cast<uint8_t>(s.power);
  buf[7] = static_cast<uint8_t>(s.ftpW & 0xFF);
  buf[8] = static_cast<uint8_t>(s.ftpW >> 8);
  buf[9] = static_cast<uint8_t>(s.cooldown);
  return kFanControlPayloadLen;
}

}  // namespace hw

#endif  // HOMEWIND_FAN_CONTROL_FRAME_H
