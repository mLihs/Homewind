/**
 * @file BleNvsBlob.h
 * @brief Arduino-free layout and decoding of the `ble_data` NVS blob
 *
 * Shared by BLERelayManager and tests/ble_nvs_blob_test.cpp.
 *
 * v3 (1.7.0): 270 bytes, three entries (HR, CSC, power).
 * v2 (1.4.45 - 1.6.x): 182 bytes, two entries; migrated to v3 on load.
 * Pre-1.4.45 firmware also wrote 270 bytes, with version 2 (HR, CSC, PWR);
 * only the version byte tells it from v3, and it is rejected.
 */

#ifndef HOMEWIND_BLE_NVS_BLOB_H
#define HOMEWIND_BLE_NVS_BLOB_H

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "BleCommandState.h"
#include "Crc16.h"

namespace hw {

constexpr uint8_t kBleNvsVersion = 3;
constexpr uint8_t kBleNvsVersionV2 = 2;

#pragma pack(push, 1)
struct BleNvsEntry {
  char name[64];
  char type[4];       // "HR", "CSC", "PWR" + null
  char mac[18];       // "aa:bb:cc:dd:ee:ff" + null
  int8_t addrType;    // 0 public, 1 random, -1 unknown
  uint8_t reserved;
};

struct BleNvsData {
  uint8_t version;
  uint8_t sensorCount;
  uint8_t reserved[2];
  BleNvsEntry sensors[kBleSlots];
  uint16_t crc16;     // CRC16-CCITT over all bytes before it
};

struct BleNvsDataV2 {
  uint8_t version;
  uint8_t sensorCount;
  uint8_t reserved[2];
  BleNvsEntry sensors[2];
  uint16_t crc16;
};
#pragma pack(pop)

static_assert(sizeof(BleNvsEntry) == 88, "BleNvsEntry size mismatch");
static_assert(sizeof(BleNvsData) == 270, "BleNvsData size mismatch");
static_assert(sizeof(BleNvsDataV2) == 182, "BleNvsDataV2 size mismatch");

enum class BleNvsLoad : uint8_t { OK, MIGRATED, BAD_SIZE, BAD_VERSION, BAD_CRC };

inline uint16_t bleNvsCrc(const void* blob, size_t size) {
  return Crc16::calcCRC16(static_cast<const uint8_t*>(blob), size - sizeof(uint16_t));
}

inline void bleNvsSeal(BleNvsData& data) {
  data.crc16 = bleNvsCrc(&data, sizeof(data));
}

// MIGRATED: out holds the v2 entries as a sealed v3 blob; write it back.
inline BleNvsLoad bleNvsDecode(const uint8_t* blob, size_t len, BleNvsData& out) {
  if (len == sizeof(BleNvsData)) {
    memcpy(&out, blob, sizeof(out));
    if (out.version != kBleNvsVersion) return BleNvsLoad::BAD_VERSION;
    if (out.crc16 != bleNvsCrc(&out, sizeof(out))) return BleNvsLoad::BAD_CRC;
    return BleNvsLoad::OK;
  }
  if (len == sizeof(BleNvsDataV2)) {
    BleNvsDataV2 v2;
    memcpy(&v2, blob, sizeof(v2));
    if (v2.version != kBleNvsVersionV2) return BleNvsLoad::BAD_VERSION;
    if (v2.crc16 != bleNvsCrc(&v2, sizeof(v2))) return BleNvsLoad::BAD_CRC;
    memset(&out, 0, sizeof(out));
    out.version = kBleNvsVersion;
    out.sensorCount = v2.sensorCount < 2 ? v2.sensorCount : 2;
    for (uint8_t i = 0; i < out.sensorCount; ++i) {
      out.sensors[i] = v2.sensors[i];
    }
    bleNvsSeal(out);
    return BleNvsLoad::MIGRATED;
  }
  return BleNvsLoad::BAD_SIZE;
}

}  // namespace hw

#endif  // HOMEWIND_BLE_NVS_BLOB_H
