// Host test for src/core/FanModeBlob.h (fan_mode NVS blob).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/fan_mode_blob_test.cpp Homewind/src/core/Crc16.cpp -o <out> && <out>

#include "FanModeBlob.h"

#include <cstdio>
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

static FanModeLoad decode(const FanModeNvsData& d, FanMode& mode, uint16_t& ftp, int8_t& cool) {
  return fanModeDecode(reinterpret_cast<const uint8_t*>(&d), sizeof(d), mode, ftp, cool);
}

static void testRoundTrip() {
  const FanModeNvsData d = fanModeEncode(FanMode::Hybrid, 275, -2);
  FanMode mode = FanMode::Pure;
  uint16_t ftp = kFanFtpDefaultW;
  int8_t cool = 0;
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::OK);
  CHECK(mode == FanMode::Hybrid);
  CHECK(ftp == 275);
  CHECK(cool == -2);
}

static void testLayout() {
  const FanModeNvsData d = fanModeEncode(FanMode::Smart, 0x0123, 0);
  const uint8_t* b = reinterpret_cast<const uint8_t*>(&d);
  CHECK(b[0] == kFanModeNvsVersion);
  CHECK(b[1] == 1);
  CHECK(b[2] == 0x23 && b[3] == 0x01);  // FTP little-endian
  CHECK(b[4] == 0 && b[5] == 0);        // cool-down 0, reserved
  CHECK(b[6] == 0x43 && b[7] == 0x7A);  // CRC16 of bytes 0..5, pinned across firmware versions

  const FanModeNvsData n = fanModeEncode(FanMode::Smart, 0x0123, -1);
  CHECK(reinterpret_cast<const uint8_t*>(&n)[4] == 0xFF);  // int8 two's complement
}

// Blobs saved before cool-down had the byte zeroed (reserved): same bytes and CRC as cool-down 0.
static void testOldBlobLoadsAsDefault() {
  const uint8_t old[8] = {1, 2, 0xFA, 0x00, 0, 0, 0, 0};
  uint8_t blob[8];
  memcpy(blob, old, sizeof(blob));
  const uint16_t crc = Crc16::calcCRC16(blob, 6);
  blob[6] = static_cast<uint8_t>(crc & 0xFF);
  blob[7] = static_cast<uint8_t>(crc >> 8);
  FanMode mode = FanMode::Pure;
  uint16_t ftp = 0;
  int8_t cool = 2;
  CHECK(fanModeDecode(blob, sizeof(blob), mode, ftp, cool) == FanModeLoad::OK);
  CHECK(mode == FanMode::Hybrid && ftp == 250 && cool == 0);
}

static void testRejects() {
  FanMode mode = FanMode::Smart;
  uint16_t ftp = 222;
  int8_t cool = 1;

  FanModeNvsData d = fanModeEncode(FanMode::Hybrid, 250, 0);
  d.ftpW = 260;  // CRC no longer matches
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_CRC);

  d = fanModeEncode(FanMode::Hybrid, 250, 0);
  d.reserved = 1;  // the reserved byte is covered by the CRC
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_CRC);

  d = fanModeEncode(FanMode::Hybrid, 250, 0);
  d.version = 2;
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_VERSION);

  d = fanModeEncode(static_cast<FanMode>(3), 250, 0);
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_VALUE);
  d = fanModeEncode(FanMode::Smart, kFanFtpMinW - 1, 0);
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_VALUE);
  d = fanModeEncode(FanMode::Smart, kFanFtpMaxW + 1, 0);
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_VALUE);
  d = fanModeEncode(FanMode::Smart, 250, kFanCooldownMax + 1);
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_VALUE);
  d = fanModeEncode(FanMode::Smart, 250, kFanCooldownMin - 1);
  CHECK(decode(d, mode, ftp, cool) == FanModeLoad::BAD_VALUE);

  uint8_t shortBlob[7] = {};
  CHECK(fanModeDecode(shortBlob, sizeof(shortBlob), mode, ftp, cool) == FanModeLoad::BAD_SIZE);
  CHECK(fanModeDecode(nullptr, sizeof(FanModeNvsData), mode, ftp, cool) == FanModeLoad::BAD_SIZE);

  CHECK(mode == FanMode::Smart);  // untouched on every rejection
  CHECK(ftp == 222);
  CHECK(cool == 1);
}

static void testLimits() {
  FanMode mode = FanMode::Pure;
  uint16_t ftp = 0;
  int8_t cool = 0;
  CHECK(decode(fanModeEncode(FanMode::Pure, kFanFtpMinW, kFanCooldownMin), mode, ftp, cool) == FanModeLoad::OK);
  CHECK(cool == kFanCooldownMin);
  CHECK(decode(fanModeEncode(FanMode::Pure, kFanFtpMaxW, kFanCooldownMax), mode, ftp, cool) == FanModeLoad::OK);
  CHECK(cool == kFanCooldownMax);
  CHECK(fanFtpClamp(0) == kFanFtpMinW);
  CHECK(fanFtpClamp(9999) == kFanFtpMaxW);
  CHECK(fanFtpClamp(321) == 321);
  CHECK(fanModeValid(2) && !fanModeValid(3));
  CHECK(fanCooldownValid(-2) && fanCooldownValid(2) && !fanCooldownValid(-3) && !fanCooldownValid(3));
}

int main() {
  testRoundTrip();
  testLayout();
  testOldBlobLoadsAsDefault();
  testRejects();
  testLimits();
  if (g_failures != 0) {
    std::printf("fan_mode_blob_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fan_mode_blob_test: OK\n");
  return 0;
}
