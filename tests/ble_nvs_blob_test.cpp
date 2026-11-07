// Host test for src/core/BleNvsBlob.h (ble_data v2 -> v3 migration).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/ble_nvs_blob_test.cpp Homewind/src/core/Crc16.cpp -o <out> && <out>

#include "BleNvsBlob.h"

#include <cstdio>
#include <cstdlib>
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

static void fillEntry(BleNvsEntry& e, const char* name, const char* type, const char* mac,
                      int8_t addrType) {
  memset(&e, 0, sizeof(e));
  strcpy(e.name, name);
  strcpy(e.type, type);
  strcpy(e.mac, mac);
  e.addrType = addrType;
}

// The 182-byte blob 1.4.45 - 1.6.x write: HR public, CSC random.
static BleNvsDataV2 makeV2() {
  BleNvsDataV2 v2;
  memset(&v2, 0, sizeof(v2));
  v2.version = kBleNvsVersionV2;
  v2.sensorCount = 2;
  fillEntry(v2.sensors[0], "Polar H10", "HR", "aa:bb:cc:dd:ee:01", 0);
  fillEntry(v2.sensors[1], "CAD-BLE0446949", "CSC", "f2:23:97:c6:37:12", 1);
  v2.crc16 = bleNvsCrc(&v2, sizeof(v2));
  return v2;
}

static BleNvsData makeV3() {
  BleNvsData d;
  memset(&d, 0, sizeof(d));
  d.version = kBleNvsVersion;
  d.sensorCount = 3;
  fillEntry(d.sensors[0], "Polar H10", "HR", "aa:bb:cc:dd:ee:01", 0);
  fillEntry(d.sensors[1], "CAD-BLE0446949", "CSC", "f2:23:97:c6:37:12", 1);
  fillEntry(d.sensors[2], "ASSIOMA12345", "PWR", "e1:23:97:c6:37:12", 1);
  bleNvsSeal(d);
  return d;
}

static void testCrcMatchesFirmware() {
  // The CRC 1.6.x stores: CRC16-CCITT over all bytes before crc16.
  BleNvsDataV2 v2 = makeV2();
  CHECK(v2.crc16 == Crc16::calcCRC16(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2) - 2));
}

static void testV3RoundTrip() {
  const BleNvsData d = makeV3();
  BleNvsData out;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&d), sizeof(d), out) == BleNvsLoad::OK);
  CHECK(memcmp(&out, &d, sizeof(d)) == 0);
  CHECK(out.sensorCount == 3 && strcmp(out.sensors[2].type, "PWR") == 0);
  CHECK(out.sensors[2].addrType == 1);
}

static void testV2Migrates() {
  const BleNvsDataV2 v2 = makeV2();
  BleNvsData out;
  memset(&out, 0xA5, sizeof(out));
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2), out) == BleNvsLoad::MIGRATED);
  CHECK(out.version == kBleNvsVersion && out.sensorCount == 2);
  CHECK(memcmp(&out.sensors[0], &v2.sensors[0], sizeof(BleNvsEntry)) == 0);
  CHECK(memcmp(&out.sensors[1], &v2.sensors[1], sizeof(BleNvsEntry)) == 0);
  CHECK(out.sensors[0].addrType == 0 && out.sensors[1].addrType == 1);
  CHECK(strcmp(out.sensors[1].mac, "f2:23:97:c6:37:12") == 0);
  const BleNvsEntry empty = {};
  CHECK(memcmp(&out.sensors[2], &empty, sizeof(empty)) == 0);
  // The migrated blob is what gets written back: it decodes as v3.
  BleNvsData again;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&out), sizeof(out), again) == BleNvsLoad::OK);
  CHECK(memcmp(&again, &out, sizeof(out)) == 0);
}

static void testV2CountClamped() {
  BleNvsDataV2 v2 = makeV2();
  v2.sensorCount = 3;                       // corrupt but CRC-valid
  v2.crc16 = bleNvsCrc(&v2, sizeof(v2));
  BleNvsData out;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2), out) == BleNvsLoad::MIGRATED);
  CHECK(out.sensorCount == 2);
}

static void testV2SmallCounts() {
  for (uint8_t count = 0; count < 2; ++count) {
    BleNvsDataV2 v2 = makeV2();             // entries beyond the count are not copied
    v2.sensorCount = count;
    v2.crc16 = bleNvsCrc(&v2, sizeof(v2));
    BleNvsData out;
    CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2), out) == BleNvsLoad::MIGRATED);
    CHECK(out.version == kBleNvsVersion && out.sensorCount == count);
    CHECK(count == 0 || strcmp(out.sensors[0].type, "HR") == 0);
    const BleNvsEntry empty = {};
    CHECK(memcmp(&out.sensors[1], &empty, sizeof(empty)) == 0);
    CHECK(memcmp(&out.sensors[2], &empty, sizeof(empty)) == 0);
  }
}

static void testV3CountNotClampedHere() {
  // A CRC-valid v3 blob with sensorCount > 3 decodes unchanged; the loader
  // (BLERelayManager::loadConfigFromNVS) clamps to MAX_SENSORS.
  BleNvsData d = makeV3();
  d.sensorCount = 7;
  bleNvsSeal(d);
  BleNvsData out;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&d), sizeof(d), out) == BleNvsLoad::OK);
  CHECK(out.sensorCount == 7);
}

static void testBadCrcRejected() {
  BleNvsDataV2 v2 = makeV2();
  v2.sensors[0].mac[0] = 'b';
  BleNvsData out;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2), out) == BleNvsLoad::BAD_CRC);
  BleNvsData d = makeV3();
  d.crc16 ^= 1;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&d), sizeof(d), out) == BleNvsLoad::BAD_CRC);
}

static void testWrongVersionOrSizeRejected() {
  BleNvsData out;
  BleNvsDataV2 v2 = makeV2();
  v2.version = 3;                           // 182 bytes but not v2
  v2.crc16 = bleNvsCrc(&v2, sizeof(v2));
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2), out) == BleNvsLoad::BAD_VERSION);
  v2.version = 1;
  v2.crc16 = bleNvsCrc(&v2, sizeof(v2));
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&v2), sizeof(v2), out) == BleNvsLoad::BAD_VERSION);

  const BleNvsData d = makeV3();
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&d), sizeof(d) - 1, out) == BleNvsLoad::BAD_SIZE);
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&d), 268, out) == BleNvsLoad::BAD_SIZE);
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&d), 0, out) == BleNvsLoad::BAD_SIZE);
}

static void testLegacy270Rejected() {
  // Pre-1.4.45: same 270-byte layout (HR, CSC, PWR) but version 2, valid CRC.
  BleNvsData legacy = makeV3();
  legacy.version = 2;
  bleNvsSeal(legacy);
  BleNvsData out;
  CHECK(bleNvsDecode(reinterpret_cast<const uint8_t*>(&legacy), sizeof(legacy), out) ==
        BleNvsLoad::BAD_VERSION);
}

int main() {
  testCrcMatchesFirmware();
  testV3RoundTrip();
  testV2Migrates();
  testV2CountClamped();
  testV2SmallCounts();
  testV3CountNotClampedHere();
  testBadCrcRejected();
  testWrongVersionOrSizeRejected();
  testLegacy270Rejected();
  if (g_failures) {
    std::printf("ble_nvs_blob_test: %d failure(s)\n", g_failures);
    return EXIT_FAILURE;
  }
  std::printf("ble_nvs_blob_test: OK\n");
  return EXIT_SUCCESS;
}
