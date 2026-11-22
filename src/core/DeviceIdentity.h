/**
 * @file DeviceIdentity.h
 * @brief Hostname and device-id formatting (Arduino-free, host-testable)
 *
 * Hostname: homewind-<8 hex> from the lower 32 bits of the eFuse MAC
 * (same ID as wifiMangerLite after lowercasing). Device ID: 12 hex of the
 * full 48-bit MAC, lowercase. Used for DHCP hostname, mDNS and system/info.
 */

#ifndef HOMEWIND_DEVICE_IDENTITY_H
#define HOMEWIND_DEVICE_IDENTITY_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace hw {

/** "homewind-" + 8 hex + NUL */
static constexpr size_t kHostnameMax = 18;
/** 12 hex + NUL */
static constexpr size_t kDeviceIdMax = 13;
static constexpr char kHostnamePrefix[] = "homewind-";

/**
 * Write hostname into out. Returns characters written excluding NUL, or 0
 * if out is too small (needs kHostnameMax).
 */
inline size_t formatHostname(char* out, size_t outLen, uint32_t mac32) {
  if (!out || outLen < kHostnameMax) return 0;
  const int n = std::snprintf(out, outLen, "%s%08lx", kHostnamePrefix,
                              static_cast<unsigned long>(mac32));
  if (n <= 0 || static_cast<size_t>(n) >= outLen) {
    out[0] = '\0';
    return 0;
  }
  return static_cast<size_t>(n);
}

/**
 * Write 12-hex device id (full 48-bit MAC, little-endian byte order as stored
 * by esp_efuse_mac_get_default into a uint64). Returns 12 or 0 if too small.
 */
inline size_t formatDeviceId(char* out, size_t outLen, uint64_t mac48) {
  if (!out || outLen < kDeviceIdMax) return 0;
  const int n = std::snprintf(
      out, outLen, "%02x%02x%02x%02x%02x%02x",
      static_cast<unsigned>((mac48 >> 0) & 0xffu),
      static_cast<unsigned>((mac48 >> 8) & 0xffu),
      static_cast<unsigned>((mac48 >> 16) & 0xffu),
      static_cast<unsigned>((mac48 >> 24) & 0xffu),
      static_cast<unsigned>((mac48 >> 32) & 0xffu),
      static_cast<unsigned>((mac48 >> 40) & 0xffu));
  if (n != 12) {
    out[0] = '\0';
    return 0;
  }
  return 12;
}

}  // namespace hw

#endif  // HOMEWIND_DEVICE_IDENTITY_H
