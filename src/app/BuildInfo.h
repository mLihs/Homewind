/**
 * @file BuildInfo.h
 * @brief Build metadata (version, build ID, etc.)
 * @note Force rebuild: 2026-11-05 (1.6.0 BikeSensor 3.0)
 */

#ifndef HOMEWIND_BUILDINFO_H
#define HOMEWIND_BUILDINFO_H

#include <Arduino.h>

// Optional stamp written by tools/build.sh. IDE builds keep the defaults below.
#if defined(__has_include)
#  if __has_include("homewind_build_stamp.h")
#    include "homewind_build_stamp.h"
#  endif
#endif

// Firmware name
#ifndef FW_NAME
#define FW_NAME "Homewind"
#endif

#ifndef FW_VERSION
#define FW_VERSION "1.6.2"
#endif

// Build ID (set by CI/CD or build script, e.g. git short hash)
// Override via compiler flag: -DFW_BUILD_ID=\"abc1234\"
// Format: YYYYMMDD-HHMMSS (serial number)
#ifndef FW_BUILD_ID
#define FW_BUILD_ID "20261106-184655"
#endif

namespace BuildInfo {
  inline const char* getName() { return FW_NAME; }
  inline const char* getVersion() { return FW_VERSION; }
  inline const char* getBuildId() { return FW_BUILD_ID; }
}

#endif // HOMEWIND_BUILDINFO_H

