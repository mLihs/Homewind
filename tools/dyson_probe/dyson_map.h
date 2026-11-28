// Pure helpers shared by dyson_probe and host tests (no Arduino).
#pragma once

#include <stdint.h>

namespace dysonmap {

// Concept §5.4: 0 → off; else ceil(pct/10) clamped to 1..10.
inline uint8_t pctToStage(uint8_t pct) {
  if (pct == 0) return 0;
  uint8_t s = static_cast<uint8_t>((pct + 9) / 10);
  if (s < 1) s = 1;
  if (s > 10) s = 10;
  return s;
}

// lastSent == 255 means "unknown / after reconnect" → always publish.
inline bool stageNeedsPublish(uint8_t lastSentOr255, uint8_t stage) {
  return lastSentOr255 != stage;
}

}  // namespace dysonmap
