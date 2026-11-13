// Host test for src/core/FanControlFrame.h (FAN_CONTROL 0x0E payload).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/fan_control_frame_test.cpp -o <out> && <out>

#include "FanControlFrame.h"

#include <cstdio>

using namespace hw;

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

static FanControlStatus sample() {
  FanControlStatus s;
  s.mode = FanMode::Hybrid;
  s.effective = FanMode::Smart;
  s.reason = FanModeReason::SpanTooSmall;
  s.targetOn = true;
  s.targetPct = 47;
  s.dominant = FanDominant::Hold;
  s.power = FanPowerState::Pedalling;
  s.ftpW = 0x0123;
  s.cooldown = -2;
  return s;
}

static void testLayout() {
  uint8_t b[kFanControlPayloadLen + 1] = {};
  b[kFanControlPayloadLen] = 0xAA;
  CHECK(encodeFanControl(sample(), b, sizeof(b)) == kFanControlPayloadLen);
  CHECK(b[0] == 2);                     // mode Hybrid
  CHECK(b[1] == 1);                     // effective Smart
  CHECK(b[2] == 2);                     // reason span < 30 bpm
  CHECK(b[3] == 1 && b[4] == 47);       // target ON, 47 %
  CHECK(b[5] == 2);                     // dominant HOLD
  CHECK(b[6] == 2);                     // power state pedalling
  CHECK(b[7] == 0x23 && b[8] == 0x01);  // FTP little-endian
  CHECK(b[9] == 0xFE);                  // cool-down -2, int8
  CHECK(b[10] == 0xAA);                 // nothing written past the payload
}

static void testOffSendsZeroPct() {
  FanControlStatus s = sample();
  s.targetOn = false;
  uint8_t b[kFanControlPayloadLen] = {};
  CHECK(encodeFanControl(s, b, sizeof(b)) == kFanControlPayloadLen);
  CHECK(b[3] == 0 && b[4] == 0);
}

static void testShortBuffer() {
  uint8_t b[kFanControlPayloadLen - 1] = {};
  CHECK(encodeFanControl(sample(), b, sizeof(b)) == 0);
  CHECK(encodeFanControl(sample(), nullptr, kFanControlPayloadLen) == 0);
}

static void testEquality() {
  const FanControlStatus a = sample();
  CHECK(a == sample());
  FanControlStatus b = a; b.mode = FanMode::Smart;              CHECK(a != b);
  b = a; b.effective = FanMode::Hybrid;                        CHECK(a != b);
  b = a; b.reason = FanModeReason::NoPower;                    CHECK(a != b);
  b = a; b.targetOn = false;                                   CHECK(a != b);
  b = a; b.targetPct = 48;                                     CHECK(a != b);
  b = a; b.dominant = FanDominant::Power;                      CHECK(a != b);
  b = a; b.power = FanPowerState::Idle;                        CHECK(a != b);
  b = a; b.ftpW = 0x0124;                                      CHECK(a != b);
  b = a; b.cooldown = -1;                                      CHECK(a != b);
}

int main() {
  testLayout();
  testOffSendsZeroPct();
  testShortBuffer();
  testEquality();
  if (g_failures != 0) {
    std::printf("fan_control_frame_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fan_control_frame_test: OK\n");
  return 0;
}
