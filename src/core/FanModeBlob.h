/**
 * @file FanModeBlob.h
 * @brief Arduino-free layout and decoding of the `fan_mode` NVS blob
 *
 * Fan control mode, FTP and cool-down (docs/FAN_POWER_HR_CONCEPT.md §4).
 * Cool-down uses a byte that was reserved and written as 0, so blobs saved
 * before it load as cool-down 0 without a version change.
 * Shared by FanModeSettings and tests/fan_mode_blob_test.cpp.
 */

#ifndef HOMEWIND_FAN_MODE_BLOB_H
#define HOMEWIND_FAN_MODE_BLOB_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "Crc16.h"
#include "FanBlend.h"

namespace hw {

constexpr uint8_t kFanModeNvsVersion = 1;
constexpr uint16_t kFanFtpMinW = 100;
constexpr uint16_t kFanFtpMaxW = 500;
constexpr uint16_t kFanFtpDefaultW = 200;

#pragma pack(push, 1)
struct FanModeNvsData {
  uint8_t version;
  uint8_t mode;       // FanMode
  uint16_t ftpW;
  int8_t cooldown;    // kFanCooldownMin..kFanCooldownMax
  uint8_t reserved;
  uint16_t crc16;     // CRC16-CCITT over all bytes before it
};
#pragma pack(pop)

static_assert(sizeof(FanModeNvsData) == 8, "FanModeNvsData size mismatch");

enum class FanModeLoad : uint8_t { OK, BAD_SIZE, BAD_VERSION, BAD_CRC, BAD_VALUE };

inline bool fanModeValid(uint8_t mode) { return mode <= static_cast<uint8_t>(FanMode::Hybrid); }

inline uint16_t fanFtpClamp(uint16_t w) {
  return w < kFanFtpMinW ? kFanFtpMinW : (w > kFanFtpMaxW ? kFanFtpMaxW : w);
}

inline FanModeNvsData fanModeEncode(FanMode mode, uint16_t ftpW, int8_t cooldown) {
  FanModeNvsData d;
  memset(&d, 0, sizeof(d));
  d.version = kFanModeNvsVersion;
  d.mode = static_cast<uint8_t>(mode);
  d.ftpW = ftpW;
  d.cooldown = cooldown;
  d.crc16 = Crc16::calcCRC16(reinterpret_cast<const uint8_t*>(&d), sizeof(d) - sizeof(d.crc16));
  return d;
}

// On anything but OK, mode, ftpW and cooldown are left unchanged (callers keep the defaults).
inline FanModeLoad fanModeDecode(const uint8_t* blob, size_t len, FanMode& mode, uint16_t& ftpW,
                                 int8_t& cooldown) {
  if (blob == nullptr || len != sizeof(FanModeNvsData)) return FanModeLoad::BAD_SIZE;
  FanModeNvsData d;
  memcpy(&d, blob, sizeof(d));
  if (d.version != kFanModeNvsVersion) return FanModeLoad::BAD_VERSION;
  if (d.crc16 != Crc16::calcCRC16(blob, sizeof(d) - sizeof(d.crc16))) return FanModeLoad::BAD_CRC;
  if (!fanModeValid(d.mode) || d.ftpW < kFanFtpMinW || d.ftpW > kFanFtpMaxW ||
      !fanCooldownValid(d.cooldown)) {
    return FanModeLoad::BAD_VALUE;
  }
  mode = static_cast<FanMode>(d.mode);
  ftpW = d.ftpW;
  cooldown = d.cooldown;
  return FanModeLoad::OK;
}

}  // namespace hw

#endif  // HOMEWIND_FAN_MODE_BLOB_H
