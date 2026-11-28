// Host test: FanCommon arena + FanErr layout (no Arduino).
#include <cstdio>
#include <cstring>
#include <stdint.h>

#include "../../FanCommon/src/FanCommon.h"
#include "../../FanCommon/src/FanArena.cpp"

static int g_fails = 0;

static void expect(bool ok, const char* name) {
  if (!ok) {
    std::printf("FAIL %s\n", name);
    ++g_fails;
  } else {
    std::printf("OK   %s\n", name);
  }
}

int main() {
  expect(FanCommon::arenaCapacity() >= 1088, "arena capacity >= 1088");
  expect(!FanCommon::arenaInUse(), "arena free initially");
  expect(FanCommon::arenaClaim(1088), "claim 1088");
  expect(FanCommon::arenaInUse(), "in use");
  expect(FanCommon::arenaClaimed() == 1088, "claimed size");
  expect(FanCommon::arenaPtr() != nullptr, "ptr");
  expect(!FanCommon::arenaClaim(16), "second claim fails");
  FanCommon::arenaRelease();
  expect(!FanCommon::arenaInUse(), "released");
  expect(FanCommon::arenaClaim(FAN_COMMON_ARENA_BYTES), "claim full");
  FanCommon::arenaRelease();
  expect(!FanCommon::arenaClaim(FAN_COMMON_ARENA_BYTES + 1), "oversized fails");

  expect(static_cast<uint8_t>(FanErr::OK) == 0, "FanErr::OK");
  expect(FAN_MAX_SLOTS >= 1 && FAN_MAX_SLOTS <= 8, "FAN_MAX_SLOTS");
  expect(FAN_ALL_MASK == ((1u << FAN_MAX_SLOTS) - 1u), "FAN_ALL_MASK");

  if (g_fails) {
    std::printf("fan_common_host_test: %d fail(s)\n", g_fails);
    return 1;
  }
  std::printf("fan_common_host_test: ok\n");
  return 0;
}
