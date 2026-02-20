/**
 * @file BuildInfo.h
 * @brief Build metadata (version, build ID, etc.)
 * @note Force rebuild: 2026-02-13 (1.4.50 release)
 */

#ifndef HOMEWIND_BUILDINFO_H
#define HOMEWIND_BUILDINFO_H

#include <Arduino.h>

// Firmware name
#ifndef FW_NAME
#define FW_NAME "Homewind"
#endif

// Firmware version (semantic versioning: MAJOR.MINOR.PATCH)
// - MAJOR: Breaking changes
// - MINOR: New features (backward compatible)
// - PATCH: Bug fixes (backward compatible)
#ifndef FW_VERSION
#define FW_VERSION "1.4.50"
#endif

// Build ID (set by CI/CD or build script, e.g. git short hash)
// Override via compiler flag: -DFW_BUILD_ID=\"abc1234\"
// Format: YYYYMMDD-HHMMSS (serial number)
#ifndef FW_BUILD_ID
#define FW_BUILD_ID "20260213-120000"
#endif

namespace BuildInfo {
  inline const char* getName() { return FW_NAME; }
  inline const char* getVersion() { return FW_VERSION; }
  inline const char* getBuildId() { return FW_BUILD_ID; }
}

#endif // HOMEWIND_BUILDINFO_H

