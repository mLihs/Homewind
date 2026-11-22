// Host test for src/core/DeviceIdentity.h
// xcrun clang++ -std=c++17 -Wall -Wextra -Werror -Isrc/core \
//   tests/device_identity_test.cpp -o /tmp/device_identity_test && /tmp/device_identity_test

#include "DeviceIdentity.h"

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

static void testHostnameFormat() {
  char buf[kHostnameMax];
  CHECK(formatHostname(buf, sizeof(buf), 0xda4eb580u) == 17);
  CHECK(std::strcmp(buf, "homewind-da4eb580") == 0);
  CHECK(std::strlen(buf) <= 32);
  CHECK(formatHostname(buf, 10, 0x1u) == 0);
  CHECK(formatHostname(nullptr, kHostnameMax, 0x1u) == 0);
  CHECK(formatHostname(buf, sizeof(buf), 0u) == 17);
  CHECK(std::strcmp(buf, "homewind-00000000") == 0);
}

static void testDeviceIdFormat() {
  char buf[kDeviceIdMax];
  // Bytes 0..5 = aa bb cc dd ee ff
  const uint64_t mac = 0x0000ffeeddccbbaaull;
  CHECK(formatDeviceId(buf, sizeof(buf), mac) == 12);
  CHECK(std::strcmp(buf, "aabbccddeeff") == 0);
  CHECK(formatDeviceId(buf, 8, mac) == 0);
  CHECK(formatDeviceId(nullptr, kDeviceIdMax, mac) == 0);
  CHECK(formatDeviceId(buf, sizeof(buf), 0u) == 12);
  CHECK(std::strcmp(buf, "000000000000") == 0);
}

int main() {
  testHostnameFormat();
  testDeviceIdFormat();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("device_identity_test: ok\n");
  return 0;
}
