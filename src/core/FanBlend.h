/**
 * @file FanBlend.h
 * @brief Arduino-free control law of the Smart and Hybrid fan modes
 *
 * docs/FAN_POWER_HR_CONCEPT.md §2–3: power filter, HR/power combiner, output
 * shaping, presence and the fail-safe state. The Pure law and the command
 * stage (3 s tick, deadband, per-fan state) stay in FanHRAdapter, which calls
 * update() every loop in all modes (FAN_CONTROL shows the power state in Pure).
 * Shared by FanHRAdapter and tests/fan_blend_test.cpp.
 */

#ifndef HOMEWIND_FAN_BLEND_H
#define HOMEWIND_FAN_BLEND_H

#include "BikeMetrics.h"

#include <cmath>
#include <cstdint>

namespace hw {

// Fan control mode setting; not the per-fan FanHrMode (ACTIVE/INACTIVE).
enum class FanMode : uint8_t { Pure = 0, Smart = 1, Hybrid = 2 };
enum class FanDominant : uint8_t { Hr = 0, Power = 1, Hold = 2, Failsafe = 3 };
enum class FanPowerState : uint8_t { None = 0, Idle = 1, Pedalling = 2 };
enum class FanModeReason : uint8_t { None = 0, NoPower = 1, SpanTooSmall = 2 };

constexpr float kFanTauUpS = 4.0f;
constexpr float kFanTauDownS = 30.0f;
constexpr float kFanMinWFactor = 0.72f;   // × FTP
constexpr float kFanMaxWFactor = 1.05f;   // × FTP
constexpr float kFanFallRatePerMin = 0.20f;  // fraction of full scale per minute, cool-down 0
constexpr float kFanFallRateStepPerMin = 0.05f;  // per cool-down level, longer = slower fall
constexpr int8_t kFanCooldownMin = -2;
constexpr int8_t kFanCooldownMax = 2;
constexpr uint8_t kFanMinSpeedPct = 25;
constexpr uint16_t kFanOffHrMarginBpm = 5;
constexpr uint32_t kFanOffHoldMs = 30000;
constexpr int32_t kFanPresenceW = 30;
constexpr uint32_t kFanPresenceHoldMs = 60000;
constexpr uint16_t kFanMinSpanBpm = 30;
constexpr int32_t kFanMaxPlausibleW = 2500;
// The first sample after boot steps the filter as if one second had passed,
// so it does not jump straight to the first reading.
constexpr uint32_t kFanFirstSampleDtMs = 1000;

inline bool fanCooldownValid(int cooldown) {
  return cooldown >= kFanCooldownMin && cooldown <= kFanCooldownMax;
}

// Cool-down -2..+2: 30 / 25 / 20 / 15 / 10 % of full scale per minute.
inline float fanFallRatePerMin(int8_t cooldown) {
  const int c = cooldown < kFanCooldownMin ? kFanCooldownMin
                                           : (cooldown > kFanCooldownMax ? kFanCooldownMax : cooldown);
  return kFanFallRatePerMin - kFanFallRateStepPerMin * static_cast<float>(c);
}

struct FanBlendInput {
  FanMode mode = FanMode::Pure;
  uint16_t ftpW = 200;
  uint16_t minHr = 80;
  uint16_t maxHr = 160;
  uint8_t maxPct = 95;           // HW_FAN_MAX_SPEED_PERCENT
  int8_t cooldown = 0;           // kFanCooldownMin..kFanCooldownMax, see fanFallRatePerMin()
  bool hrValid = false;
  uint16_t hrUsed = 0;           // smoothed HR (FanHRAdapter)
  bool powerConfigured = false;  // a power source is set up
  bool powerFresh = false;       // last power sample not older than kBikeStaleMs; false when not configured
};

struct FanBlendOutput {
  bool failsafe = false;         // fans must be OFF (HR missing, no Hybrid presence)
  bool failsafeEntered = false;  // rising edge in this update
  bool on = false;
  uint8_t pct = 0;               // command while on, kFanMinSpeedPct..maxPct
  FanDominant dominant = FanDominant::Hr;
  FanMode effective = FanMode::Pure;
  FanModeReason reason = FanModeReason::None;
  FanPowerState power = FanPowerState::None;
};

class FanBlend {
 public:
  // Call once per new power sample (detected by its timestamp, not its value).
  // nowMs: sample time (now - ageMs), the clock update() is called with.
  void onPowerSample(uint32_t nowMs, int32_t watts, uint16_t ftpW) {
    if (watts < 0 || watts > kFanMaxPlausibleW) return;
    const float maxW = kFanMaxWFactor * static_cast<float>(ftpW);
    const float in = watts > maxW ? maxW : static_cast<float>(watts);
    // While FALL_RATE holds a level that power had reached, P_ff has decayed to
    // the recovery power. A new effort above that level would otherwise climb
    // from there first (§6.8). Seed at the power of the held level, then filter
    // as usual. A Hold from HR alone (e.g. 1 bpm less) stays unseeded.
    if (_holding) {
      const float lo = kFanMinWFactor * static_cast<float>(ftpW);
      const float pEq = lo + _sPrev * (maxW - lo);
      if (in > pEq && _pFf < pEq) _pFf = pEq;
    }
    const uint32_t dtMs = _filterValid ? elapsed(nowMs, _filterMs) : kFanFirstSampleDtMs;
    const float tau = in > _pFf ? kFanTauUpS : kFanTauDownS;
    _pFf += (in - _pFf) * alpha(dtMs, tau);
    _filterMs = nowMs;
    _filterValid = true;
    _lastW = watts;
    if (watts > kFanPresenceW) markPedalling(nowMs);
  }

  // Call once per new cadence value (by its timestamp), from any source (CSC or
  // power meter); rpm > 0 counts as pedalling. Repeating an old value every
  // loop would keep presence alive forever.
  void onCadence(uint32_t nowMs, uint16_t rpm) {
    if (rpm > 0) {
      markPedalling(nowMs);
      _lastCadenceMs = nowMs;
      _cadenceSeen = true;
    }
  }

  // Entering Smart/Hybrid while the fans run: start from the speed last sent.
  void seed(bool sentOn, uint8_t sentPct, uint8_t maxPct) {
    _on = sentOn;
    _sPrev = sentOn && maxPct > 0 ? static_cast<float>(sentPct) / maxPct : 0.0f;
    _belowSinceValid = false;
    _holding = false;      // first update() may Hold; seed only if power had reached it
    _maxPw = 0.0f;
    _updateValid = false;  // the next update() starts from the seed, not from an old timestamp
  }

  FanBlendOutput update(uint32_t nowMs, const FanBlendInput& in) {
    FanBlendOutput out;
    const uint32_t dtMs = _updateValid ? elapsed(nowMs, _updateMs) : 0;
    _updateMs = nowMs;
    _updateValid = true;

    if (!in.powerFresh && _filterValid) {
      _pFf *= std::exp(-static_cast<float>(elapsed(nowMs, _filterMs)) / (kFanTauDownS * 1000.0f));
      _filterMs = nowMs;
    }
    if (_pedalled && elapsed(nowMs, _lastPedalMs) > kFanPresenceHoldMs) _pedalled = false;
    if (_cadenceSeen && elapsed(nowMs, _lastCadenceMs) > kBikeStaleMs) _cadenceSeen = false;

    const bool spanOk = in.maxHr > in.minHr && in.maxHr - in.minHr >= kFanMinSpanBpm;
    const bool hybrid = in.mode == FanMode::Hybrid && in.powerConfigured && spanOk;
    const bool presence = hybrid && _pedalled;
    out.power = !in.powerFresh ? FanPowerState::None
                : (_lastW > kFanPresenceW || _cadenceSeen) ? FanPowerState::Pedalling
                                                            : FanPowerState::Idle;
    out.effective = in.mode;
    if (in.mode == FanMode::Hybrid && !(hybrid && in.powerFresh)) {
      out.effective = FanMode::Smart;
      out.reason = spanOk ? FanModeReason::NoPower : FanModeReason::SpanTooSmall;
    }

    out.failsafe = !in.hrValid && !presence;
    if (out.failsafe) {
      out.failsafeEntered = !_failsafe;
      _failsafe = true;
      _on = false;
      _sPrev = 0.0f;
      _belowSinceValid = false;
      out.dominant = FanDominant::Failsafe;
      _holding = false;
      _maxPw = 0.0f;
      return out;
    }
    _failsafe = false;

    const bool hrOn = in.hrValid && in.hrUsed >= in.minHr;
    const float sHr = hrOn ? ramp(in.hrUsed, in.minHr, in.maxHr) : 0.0f;
    float sPw = 0.0f;
    if (hybrid && in.powerFresh) {
      sPw = ramp(_pFf, kFanMinWFactor * in.ftpW, kFanMaxWFactor * in.ftpW);
    }
    const float sRaw = sHr > sPw ? sHr : sPw;
    float s = sRaw;
    if (sRaw >= _sPrev) {
      out.dominant = sRaw == sHr ? FanDominant::Hr : FanDominant::Power;
    } else {
      const float floor = _sPrev - fanFallRatePerMin(in.cooldown) * static_cast<float>(dtMs) / 60000.0f;
      if (floor > sRaw) {
        s = floor;
        out.dominant = FanDominant::Hold;
      } else {
        out.dominant = sRaw == sHr ? FanDominant::Hr : FanDominant::Power;
      }
    }
    _sPrev = s;
    // Seed a Hold only if power had reached the held level (not an HR wobble).
    // Track peak s_pw outside Hold; age it at FALL_RATE so an old surge does
    // not seed a later HR Hold.
    if (out.dominant != FanDominant::Hold) {
      if (sPw > _maxPw) {
        _maxPw = sPw;
      } else {
        if (_maxPw > s) _maxPw = s;  // drop with the output; then age at FALL_RATE
        const float decay =
            fanFallRatePerMin(in.cooldown) * static_cast<float>(dtMs) / 60000.0f;
        _maxPw = _maxPw > decay ? _maxPw - decay : 0.0f;
      }
    }
    _holding = out.dominant == FanDominant::Hold && _maxPw > 0.0f && _maxPw + 0.02f >= _sPrev;

    if (hrOn || sPw > 0.0f) {
      _on = true;
      _belowSinceValid = false;
    } else if (_on) {
      const bool hrLow = !in.hrValid || in.hrUsed + kFanOffHrMarginBpm < in.minHr;
      if (hrLow && s * in.maxPct <= kFanMinSpeedPct) {
        if (!_belowSinceValid) {
          _belowSince = nowMs;
          _belowSinceValid = true;
        }
        if (nowMs - _belowSince >= kFanOffHoldMs) _on = false;
      } else {
        _belowSinceValid = false;
      }
    }
    if (!_on) {
      _sPrev = 0.0f;
      _holding = false;
      _maxPw = 0.0f;
    }

    out.on = _on;
    if (_on) {
      const int pct = static_cast<int>(s * in.maxPct + 0.5f);
      out.pct = static_cast<uint8_t>(pct < kFanMinSpeedPct ? kFanMinSpeedPct : pct);
    }
    return out;
  }

  float powerFiltered() const { return _pFf; }

 private:
  static float alpha(uint32_t dtMs, float tauS) {
    return 1.0f - std::exp(-static_cast<float>(dtMs) / (tauS * 1000.0f));
  }
  // Signed difference: a timestamp slightly older than the last one counts as 0, not ~49 days.
  static uint32_t elapsed(uint32_t nowMs, uint32_t thenMs) {
    const int32_t d = static_cast<int32_t>(nowMs - thenMs);
    return d < 0 ? 0 : static_cast<uint32_t>(d);
  }
  static float ramp(float x, float lo, float hi) {
    if (hi <= lo) return 0.0f;
    const float r = (x - lo) / (hi - lo);
    return r < 0.0f ? 0.0f : (r > 1.0f ? 1.0f : r);
  }
  void markPedalling(uint32_t nowMs) {
    _lastPedalMs = nowMs;
    _pedalled = true;
  }
  float _pFf = 0.0f;
  float _sPrev = 0.0f;
  uint32_t _filterMs = 0;
  uint32_t _updateMs = 0;
  uint32_t _lastPedalMs = 0;
  uint32_t _lastCadenceMs = 0;
  uint32_t _belowSince = 0;
  int32_t _lastW = 0;
  bool _filterValid = false;
  bool _updateValid = false;
  bool _pedalled = false;
  bool _cadenceSeen = false;
  bool _belowSinceValid = false;
  bool _on = false;
  bool _failsafe = false;
  bool _holding = false;  // Hold that power had reached; see onPowerSample()
  float _maxPw = 0.0f;    // peak s_pw outside Hold; ages at FALL_RATE
};

}  // namespace hw

#endif  // HOMEWIND_FAN_BLEND_H
