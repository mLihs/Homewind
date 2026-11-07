// Shadows the core's sdkconfig.h (the sketch folder comes first on the include
// path): HR, CSC and power sensor links plus one central. Checked by a
// static_assert in src/core/BLERelayManager.cpp.
#pragma once
#include_next <sdkconfig.h>
#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 4
