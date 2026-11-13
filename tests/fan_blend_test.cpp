// Host test for src/core/FanBlend.h (Smart/Hybrid control law, concept §8).
// c++ -std=c++17 -Wall -Wextra -Werror -IHomewind/src/core \
//   Homewind/tests/fan_blend_test.cpp -o <out> && <out>
//
// Ports the checks of tools/fan_blend_sim.py. The command stage below mirrors
// FanHRAdapter (3 s tick, 3 % deadband, OFF at once on fail-safe).

#include "FanBlend.h"

#include <cmath>
#include <cstdio>
#include <vector>

using namespace hw;

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

static constexpr uint16_t kFtp = 250;
static constexpr uint32_t kT0 = 0xFFFD0000u;  // millis() wraps about 196 s into every ride
static constexpr int kNoHr = -1;
static constexpr uint8_t kMax = 95;

struct Cmd {
  int t;
  bool on;
  int pct;
  bool operator==(const Cmd& o) const { return t == o.t && on == o.on && pct == o.pct; }
};

// HR = 95 + 70 × P/FTP, 8 s delay, τ 60 s up / 120 s down (same as the simulation).
class HrModel {
 public:
  int step(double w) {
    buf_.push_back(w);
    const double pd = buf_.front();
    buf_.erase(buf_.begin());
    const double target = 95.0 + 70.0 * std::fmin(pd, 1.2 * kFtp) / kFtp;
    const double tau = target > hr_ ? 60.0 : 120.0;
    hr_ += (target - hr_) * (1.0 - std::exp(-1.0 / tau));
    return static_cast<int>(hr_ + 0.5);
  }

 private:
  std::vector<double> buf_ = std::vector<double>(8, 0.0);
  double hr_ = 95.0;
};

struct Rig {
  FanBlend blend;
  FanBlendInput in;
  std::vector<int> hrBuf;
  std::vector<Cmd> cmds;
  FanBlendOutput last;
  bool sentOn = false;
  int sent = 0;
  int lastTick = -1000;
  int failsafeEdges = 0;

  explicit Rig(FanMode mode, uint16_t minHr = 110, uint16_t maxHr = 170) {
    in.mode = mode;
    in.ftpW = kFtp;
    in.minHr = minHr;
    in.maxHr = maxHr;
    in.maxPct = kMax;
    in.powerConfigured = true;
  }

  void send(int t, bool on, int pct) {
    sentOn = on;
    sent = on ? pct : 0;
    cmds.push_back({t, on, sent});
  }

  // hr: kNoHr = HR missing. fresh: a new power sample this second.
  void step(int t, double watts, bool fresh, int hr, int cadence = 0) {
    const uint32_t now = kT0 + static_cast<uint32_t>(t) * 1000u;
    if (fresh) blend.onPowerSample(now, static_cast<int32_t>(std::lround(watts)), in.ftpW);
    if (cadence > 0) blend.onCadence(now, static_cast<uint16_t>(cadence));
    if (hr == kNoHr) {
      hrBuf.clear();
      in.hrValid = false;
      in.hrUsed = 0;
    } else {
      hrBuf.push_back(hr);
      if (hrBuf.size() > 5) hrBuf.erase(hrBuf.begin());
      int sum = 0;
      for (int x : hrBuf) sum += x;
      const int n = static_cast<int>(hrBuf.size());
      in.hrValid = true;
      in.hrUsed = static_cast<uint16_t>((sum + n / 2) / n);
    }
    in.powerFresh = fresh;
    last = blend.update(now, in);
    if (last.failsafeEntered) failsafeEdges++;
    if (last.failsafe) {
      if (sentOn) send(t, false, 0);
      return;
    }
    if (t - lastTick < 3) return;
    lastTick = t;
    if (!last.on) {
      if (sentOn) send(t, false, 0);
      return;
    }
    const int db = (last.pct <= 3 || sent <= 3) ? 1 : 3;
    if (!sentOn || std::abs(last.pct - sent) >= db) send(t, true, last.pct);
  }

  int sentAt(int t) const {
    int pct = 0;
    for (const Cmd& c : cmds) {
      if (c.t > t) break;
      pct = c.on ? c.pct : 0;
    }
    return pct;
  }

  int offAfter(int t0) const {
    for (const Cmd& c : cmds) {
      if (!c.on && c.t >= t0) return c.t - t0;
    }
    return -1;
  }
};

// Power profile helpers.
static std::vector<double> seg(std::initializer_list<std::pair<int, double>> parts) {
  std::vector<double> p;
  for (const auto& s : parts) p.insert(p.end(), static_cast<size_t>(s.first), s.second);
  return p;
}

static std::vector<double> block() { return seg({{60, 0}, {240, kFtp}, {300, 0}}); }  // ends at t = 300

static std::vector<double> intervals3030() {
  std::vector<double> p = seg({{60, 0}});
  for (int i = 0; i < 10; i++) {
    p.insert(p.end(), 30, 1.4 * kFtp);
    p.insert(p.end(), 30, 0.4 * kFtp);
  }
  uint32_t lcg = 1;
  for (double& w : p) {
    lcg = lcg * 1664525u + 1013904223u;
    const double r = static_cast<double>(lcg >> 8) / 16777216.0;  // 0..1
    w *= 1.0 + (r * 2.0 - 1.0) * 0.06;
  }
  return p;
}

enum class HrSrc { Model, Missing, Low };

static Rig ride(FanMode mode, const std::vector<double>& p, HrSrc src, int staleFrom = -1,
                const std::vector<int>* cadence = nullptr, uint16_t minHr = 110, uint16_t maxHr = 170) {
  Rig rig(mode, minHr, maxHr);
  HrModel model;
  for (int t = 0; t < static_cast<int>(p.size()); t++) {
    const int m = model.step(p[t]);
    const int hr = src == HrSrc::Model ? m : (src == HrSrc::Low ? 100 : kNoHr);
    const bool fresh = staleFrom < 0 || t < staleFrom;
    const int cad = cadence ? (*cadence)[t] : 0;
    rig.step(t, p[t], fresh, hr, cad);
  }
  return rig;
}

static void testStepResponse() {
  Rig rig(FanMode::Hybrid);
  const std::vector<double> p = seg({{120, 150}, {60, 300}});
  int t95 = -1;
  for (int t = 0; t < static_cast<int>(p.size()); t++) {
    rig.step(t, p[t], true, kNoHr);
    const double target = 150 + 0.95 * (std::fmin(300, 1.05 * kFtp) - 150);
    if (t >= 120 && t95 < 0 && rig.blend.powerFiltered() >= target) t95 = t - 120;
  }
  CHECK(t95 >= 0 && t95 <= 13);
}

static void testSampleGaps() {
  FanBlend b;
  b.onPowerSample(kT0, 0, kFtp);
  const int gaps[] = {1, 7, 12, 3, 9, 2, 12, 5, 11, 4};
  uint32_t t = 0;
  float err = 0;
  for (int g : gaps) {
    t += static_cast<uint32_t>(g) * 1000u;
    b.onPowerSample(kT0 + t, 200, kFtp);
    const float expect = 200.0f * (1.0f - std::exp(-static_cast<float>(t) / 4000.0f));
    err = std::fmax(err, std::fabs(b.powerFiltered() - expect));
  }
  CHECK(err < 0.5f);
}

static void testSprint() {
  Rig rig(FanMode::Hybrid);
  const std::vector<double> p = seg({{900, 150}, {10, 800}, {240, 150}});
  int back = -1;
  float peak = 0;
  for (int t = 0; t < static_cast<int>(p.size()); t++) {
    rig.step(t, p[t], true, 100);
    peak = std::fmax(peak, rig.blend.powerFiltered());
    if (t > 910 && back < 0 && rig.blend.powerFiltered() < kFanMinWFactor * kFtp) back = t - 910;
  }
  CHECK(peak > kFanMinWFactor * kFtp);           // the sprint did raise P_ff
  CHECK(peak <= kFanMaxWFactor * kFtp + 0.01f);  // clamped to maxW (§0.6)
  CHECK(back >= 10 && back <= 90);
}

static void testAfterBlock() {
  const Rig model = ride(FanMode::Hybrid, block(), HrSrc::Model);
  CHECK(model.sentAt(360) >= 55);

  const Rig low = ride(FanMode::Hybrid, block(), HrSrc::Low);
  CHECK(low.sentAt(300) >= 80);
  const double fallTick = kFanFallRatePerMin * kMax / 60.0 * 3;
  bool stepsOk = true;
  for (int t = 300; t < 599; t++) {
    const int a = low.sentAt(t), b = low.sentAt(t + 1);
    if (a - b > fallTick + 3 && !(b == 0 && a <= kFanMinSpeedPct + 3)) stepsOk = false;
  }
  CHECK(stepsOk);
  CHECK(low.sentAt(599) == 0);
}

// Cool-down -2..+2 sets the fall rate: 30 / 25 / 20 / 15 / 10 % of full scale per minute.
static void testCooldown() {
  CHECK(std::fabs(fanFallRatePerMin(0) - kFanFallRatePerMin) < 1e-6f);
  CHECK(std::fabs(fanFallRatePerMin(-2) - 0.30f) < 1e-6f);
  CHECK(std::fabs(fanFallRatePerMin(2) - 0.10f) < 1e-6f);
  CHECK(std::fabs(fanFallRatePerMin(5) - 0.10f) < 1e-6f);  // clamped
  int prevOff = -1;
  for (int c = kFanCooldownMin; c <= kFanCooldownMax; c++) {
    Rig rig(FanMode::Hybrid);
    rig.in.cooldown = static_cast<int8_t>(c);
    const std::vector<double> p = seg({{60, 0}, {240, kFtp}, {600, 0}});
    for (int t = 0; t < static_cast<int>(p.size()); t++) rig.step(t, p[t], true, 100);
    const double drop = rig.sentAt(300) - rig.sentAt(360);
    const double expect = fanFallRatePerMin(static_cast<int8_t>(c)) * kMax;
    CHECK(std::fabs(drop - expect) <= 2);  // 3 s ticks and 3 % deadband steps
    const int off = rig.offAfter(300);
    CHECK(off > prevOff);  // longer cool-down switches off later
    prevOff = off;
  }
}

// Phase-5 hardware cases (§8), mid-ride: steady FTP, HR model, maxHR 190 so power dominates.
// Times in s; kNever = the event does not happen.
static constexpr int kNever = 1 << 30;

static Rig midRide(FanMode mode, int powerOffFrom, int powerOffTo, int hrOffFrom, int stopAt, int end) {
  Rig rig(mode, 110, 190);
  HrModel model;
  for (int t = 0; t < end; t++) {
    const double w = t < stopAt ? kFtp : 0;
    const int hr = model.step(w);
    rig.step(t, w, !(t >= powerOffFrom && t < powerOffTo), t < hrOffFrom ? hr : kNoHr);
  }
  return rig;
}

static int largestStep(const Rig& rig, int from, int to, bool up) {
  int worst = 0;
  for (int t = from; t < to; t++) {
    const int d = rig.sentAt(t + 1) - rig.sentAt(t);
    worst = std::max(worst, up ? d : -d);
  }
  return worst;
}

static std::vector<Cmd> cmdsFrom(const Rig& rig, int t0) {
  std::vector<Cmd> out;
  for (const Cmd& c : rig.cmds) {
    if (c.t >= t0) out.push_back(c);
  }
  return out;
}

static void testPowerDropoutMidRide() {
  // Power meter disconnected 600..900 s: Smart behaviour without a drop faster than
  // FALL_RATE; when it is back, P_ff has decayed (§3), so the fans behave as for a
  // meter connected at that moment.
  const Rig rig = midRide(FanMode::Hybrid, 600, 900, kNever, kNever, 1200);
  const Rig smart = midRide(FanMode::Smart, 600, 900, kNever, kNever, 1200);
  const Rig late = midRide(FanMode::Hybrid, 0, 900, kNever, kNever, 1200);
  const double fallTick = kFanFallRatePerMin * kMax / 60.0 * 3;
  CHECK(rig.sentAt(599) > smart.sentAt(599) + 10);  // power dominated before the dropout
  CHECK(largestStep(rig, 599, 899, false) <= fallTick + 3);
  CHECK(std::abs(rig.sentAt(899) - smart.sentAt(899)) <= 3);
  CHECK(cmdsFrom(rig, 900) == cmdsFrom(late, 900));
  CHECK(std::abs(rig.sentAt(1199) - rig.sentAt(599)) <= 1);
}

static void testHrLossWhilePedalling() {
  // HR sensor disconnected at 600 s while pedalling: the fans run as in a ride without
  // HR; fail-safe only when pedalling stops (900 s, trainer keeps sending 0 W).
  const Rig rig = midRide(FanMode::Hybrid, kNever, kNever, 600, 900, 1100);
  const Rig noHr = midRide(FanMode::Hybrid, kNever, kNever, 0, 900, 1100);
  CHECK(rig.cmds == noHr.cmds);
  CHECK(rig.offAfter(600) >= 359 && rig.offAfter(600) <= 364);
  CHECK(rig.failsafeEdges == 1);
}

static void testDismount() {
  for (int stale = 0; stale < 2; stale++) {
    // HR and pedalling end together at 900 s; the trainer keeps sending 0 W or stops sending.
    const Rig gone = midRide(FanMode::Hybrid, stale ? 900 : kNever, kNever, 900, 900, 1100);
    CHECK(gone.offAfter(900) >= 59 && gone.offAfter(900) <= 64);
    CHECK(gone.failsafeEdges == 1);
    CHECK(gone.sentAt(1099) == 0);
    // Strap still connected: no fail-safe; FALL_RATE, then the HR hold band, switches off.
    const Rig strapOn = midRide(FanMode::Hybrid, stale ? 900 : kNever, kNever, kNever, 900, 1500);
    CHECK(strapOn.failsafeEdges == 0);
    CHECK(strapOn.offAfter(900) > 200);
  }
}

static void testPresenceEnds() {
  const Rig fresh = ride(FanMode::Hybrid, block(), HrSrc::Missing);
  CHECK(fresh.offAfter(300) >= 59 && fresh.offAfter(300) <= 64);
  const Rig stale = ride(FanMode::Hybrid, block(), HrSrc::Missing, 300);
  CHECK(stale.offAfter(300) >= 59 && stale.offAfter(300) <= 64);
}

static void testCadencePresence() {
  std::vector<int> cad(600, 0);
  for (int t = 0; t < 420; t++) cad[t] = 80;
  const Rig meter = ride(FanMode::Hybrid, block(), HrSrc::Missing, -1, &cad);
  CHECK(meter.offAfter(300) >= 179 && meter.offAfter(300) <= 184);
  const Rig csc = ride(FanMode::Hybrid, block(), HrSrc::Missing, 300, &cad);
  CHECK(csc.offAfter(300) >= 179 && csc.offAfter(300) <= 184);
}

static void testCoasting() {
  const std::vector<double> p = seg({{120, 200}, {10, 0}, {60, 200}});
  const Rig fresh = ride(FanMode::Hybrid, p, HrSrc::Missing);
  CHECK(fresh.sentAt(119) > 0);
  CHECK(fresh.offAfter(60) < 0);
  Rig stale(FanMode::Hybrid);
  for (int t = 0; t < static_cast<int>(p.size()); t++) {
    stale.step(t, p[t], !(t >= 120 && t < 130), kNoHr);
  }
  CHECK(stale.sentAt(119) > 0);
  CHECK(stale.offAfter(60) < 0);
}

static void testIntervalBand() {
  const Rig rig = ride(FanMode::Hybrid, intervals3030(), HrSrc::Missing);
  int lo = 100, hi = 0;
  for (int t = 240; t < 660; t++) {
    lo = std::min(lo, rig.sentAt(t));
    hi = std::max(hi, rig.sentAt(t));
  }
  CHECK(hi - lo <= 20);
  CHECK(lo >= kFanMinSpeedPct);
}

// §6.8: while Hold, a new effort above the held level must raise the fan within
// one tick and must not drop first; a short spike at the HR level stays ignored.
static void testHoldReact() {
  std::vector<double> p = seg({{60, 0.5 * kFtp}});
  for (int i = 0; i < 5; i++) {
    p.insert(p.end(), 30, 1.4 * kFtp);
    p.insert(p.end(), 30, 0.4 * kFtp);
  }
  // HR below minHR: power alone drives the fan, so Hold is pure FALL_RATE.
  const Rig rig = ride(FanMode::Hybrid, p, HrSrc::Low);
  for (int i = 1; i < 5; i++) {
    const int rise = 60 + i * 60;
    const int base = rig.sentAt(rise - 1);
    CHECK(base > kFanMinSpeedPct);
    int firstHigher = -1;
    int lowest = base;
    for (int t = rise; t <= rise + 6; t++) {
      const int pct = rig.sentAt(t);
      if (pct < lowest) lowest = pct;
      if (firstHigher < 0 && pct > base) firstHigher = t - rise;
    }
    CHECK(lowest >= base);          // no drop before the rise
    CHECK(firstHigher >= 0 && firstHigher <= 3);  // ≤ one command tick
  }

  Rig spike(FanMode::Hybrid);
  for (int t = 0; t < 180; t++) spike.step(t, 100, true, 130);
  const int base = spike.sentAt(179);
  CHECK(base >= kFanMinSpeedPct);
  for (int t = 180; t < 183; t++) spike.step(t, 400, true, 130);
  for (int t = 183; t < 240; t++) spike.step(t, 100, true, 130);
  int peak = base;
  for (int t = 180; t < 240; t++) peak = std::max(peak, spike.sentAt(t));
  CHECK(peak <= base);  // still following HR, not the spike

  // Same spike right after the smoothed HR fell by 1 bpm: FALL_RATE holds the
  // fan (dominant Hold), but power never led, so the spike stays ignored.
  Rig dip(FanMode::Hybrid);
  for (int t = 0; t < 177; t++) dip.step(t, 100, true, 131);
  for (int t = 177; t < 180; t++) dip.step(t, 100, true, 130);
  CHECK(dip.last.dominant == FanDominant::Hold);
  const int dipBase = dip.sentAt(179);
  for (int t = 180; t < 183; t++) dip.step(t, 400, true, 130);
  for (int t = 183; t < 240; t++) dip.step(t, 100, true, 130);
  int dipPeak = dipBase;
  for (int t = 180; t < 240; t++) dipPeak = std::max(dipPeak, dip.sentAt(t));
  CHECK(dipPeak <= dipBase);

  // After a 4-min FTP block the HR model can sit slightly above s_pw; the next
  // sprint must still seed because power had reached the held level.
  for (double recW : {0.4 * kFtp, 0.0}) {
    const std::vector<double> afterBlock =
        seg({{60, 0}, {240, static_cast<double>(kFtp)}, {120, recW}, {10, 2.0 * kFtp}, {60, 0.4 * kFtp}});
    const Rig block = ride(FanMode::Hybrid, afterBlock, HrSrc::Model);
    const int sprint = 420;
    const int held = block.sentAt(sprint - 1);
    CHECK(held > kFanMinSpeedPct);
    int up = -1, low = held;
    for (int t = sprint; t <= sprint + 6; t++) {
      const int pct = block.sentAt(t);
      if (pct < low) low = pct;
      if (up < 0 && pct > held) up = t - sprint;
    }
    CHECK(low >= held);
    CHECK(up >= 0 && up <= 3);
  }

  // An early surge must not leave a peak that seeds a later HR-only Hold
  // (check soon after the surge Hold ends, not only minutes later).
  Rig stale(FanMode::Hybrid);
  for (int t = 0; t < 30; t++) stale.step(t, 300, true, 140);
  for (int t = 30; t < 130; t++) stale.step(t, 200, true, 152);
  for (int t = 130; t < 133; t++) stale.step(t, 200, true, 151);
  CHECK(stale.last.dominant == FanDominant::Hold);
  const int staleBase = stale.sentAt(132);
  for (int t = 133; t < 136; t++) stale.step(t, 400, true, 151);
  for (int t = 136; t < 190; t++) stale.step(t, 200, true, 151);
  int stalePeak = staleBase;
  for (int t = 133; t < 190; t++) stalePeak = std::max(stalePeak, stale.sentAt(t));
  CHECK(stalePeak <= staleBase);

  // Near minHR with power never on the map: the 0.02 margin must not seed.
  Rig near(FanMode::Hybrid);
  for (int t = 0; t < 177; t++) near.step(t, 100, true, 111);
  for (int t = 177; t < 180; t++) near.step(t, 100, true, 110);
  const int nearBase = near.sentAt(179);
  for (int t = 180; t < 183; t++) near.step(t, 400, true, 110);
  for (int t = 183; t < 240; t++) near.step(t, 100, true, 110);
  int nearPeak = nearBase;
  for (int t = 180; t < 240; t++) nearPeak = std::max(nearPeak, near.sentAt(t));
  CHECK(nearPeak <= nearBase);
}

static void testSmartShaping() {
  Rig rig(FanMode::Smart);
  HrModel model;
  const std::vector<double> p = block();
  int hrAt420 = 0;
  for (int t = 0; t < static_cast<int>(p.size()); t++) {
    rig.step(t, p[t], true, model.step(p[t]));
    if (t == 420) hrAt420 = rig.in.hrUsed;
  }
  const int unshaped = std::max(hrAt420 >= 110 ? (hrAt420 - 110) * kMax / 60 : 0,
                                static_cast<int>(kFanMinSpeedPct));
  CHECK(rig.sentAt(420) >= unshaped + 10);
}

static void testFailsafe() {
  Rig smart(FanMode::Smart);
  for (int t = 0; t < 150; t++) smart.step(t, 0, true, t < 100 ? 130 : kNoHr);
  CHECK(smart.offAfter(100) == 0);
  CHECK(smart.failsafeEdges == 1);
  CHECK(smart.last.dominant == FanDominant::Failsafe);

  const std::vector<double> p = seg({{300, kFtp}, {120, 0}, {60, 100}});
  const Rig hybrid = ride(FanMode::Hybrid, p, HrSrc::Missing);
  CHECK(hybrid.offAfter(300) >= 0);
  bool resumed = false;
  for (const Cmd& c : hybrid.cmds) {
    if (c.t >= 420 && c.on) resumed = true;
  }
  CHECK(!resumed);
}

static void testSeed() {
  Rig rig(FanMode::Smart);
  rig.in.hrValid = true;
  rig.in.hrUsed = 130;
  rig.blend.seed(true, 69, kMax);
  rig.blend.update(kT0, rig.in);  // first update: no elapsed time
  rig.step(1, 0, true, 130);
  CHECK(rig.last.on);
  CHECK(rig.last.pct >= 67 && rig.last.pct <= 69);
  CHECK(rig.last.dominant == FanDominant::Hold);

  // Seed after a long gap since the last update: the gap must not eat the seed.
  Rig gap(FanMode::Smart);
  gap.step(0, 0, true, 130);
  gap.blend.seed(true, 69, kMax);
  gap.step(3600, 0, true, 130);
  CHECK(gap.last.pct >= 67 && gap.last.pct <= 69);
  CHECK(gap.last.dominant == FanDominant::Hold);
}

static void testHoldBand() {
  // From ON, HR 107 (minHR 110, margin 5) holds the fan for minutes.
  Rig on(FanMode::Smart);
  for (int t = 0; t < 60; t++) on.step(t, 0, true, 140);
  for (int t = 60; t < 600; t++) on.step(t, 0, true, 107);
  CHECK(on.last.on);
  CHECK(on.sentAt(599) >= kFanMinSpeedPct && on.sentAt(599) < kFanMinSpeedPct + 3);  // within the deadband
  // From OFF, HR 107 does not switch it on.
  Rig off(FanMode::Smart);
  for (int t = 0; t < 120; t++) off.step(t, 0, true, 107);
  CHECK(!off.last.on);
  CHECK(off.cmds.empty());
  // HR exactly minHR switches on.
  Rig edge(FanMode::Smart);
  edge.step(0, 0, true, 110);
  CHECK(edge.last.on);
  CHECK(edge.last.pct == kFanMinSpeedPct);
  // A bounce into the band restarts the 30 s OFF timer (HR is a 5-sample mean).
  Rig bounce(FanMode::Smart);
  for (int t = 0; t < 10; t++) bounce.step(t, 0, true, 120);
  int t = 10;
  for (; t < 30; t++) bounce.step(t, 0, true, 100);
  for (; t < 38; t++) bounce.step(t, 0, true, 107);
  for (; t < 60; t++) bounce.step(t, 0, true, 100);
  CHECK(bounce.last.on);  // without the restart it would be OFF from t = 40
  for (; t < 80; t++) bounce.step(t, 0, true, 100);
  CHECK(!bounce.last.on);
}

static void testStaleDecay() {
  Rig rig(FanMode::Hybrid);
  for (int t = 0; t < 120; t++) rig.step(t, 200, true, 130);
  const float before = rig.blend.powerFiltered();
  for (int t = 120; t < 150; t++) rig.step(t, 0, false, 130);
  const float expect = before * std::exp(-30.0f / kFanTauDownS);  // from the last sample at t = 119
  CHECK(std::fabs(rig.blend.powerFiltered() - expect) < 1.0f);
  rig.step(150, 0, true, 130);
  CHECK(rig.blend.powerFiltered() < expect);
}

static void testPresencePreconditions() {
  for (int variant = 0; variant < 2; variant++) {
    Rig rig(FanMode::Hybrid, 110, variant == 0 ? 170 : 130);
    rig.in.powerConfigured = variant == 0 ? false : true;
    for (int t = 0; t < 30; t++) rig.step(t, 200, true, 130);
    rig.step(30, 200, true, kNoHr);
    CHECK(rig.last.failsafe);
    CHECK(rig.last.failsafeEntered);
  }
}

static void testPowerDominant() {
  Rig rig(FanMode::Hybrid);
  for (int t = 0; t < 30; t++) rig.step(t, kFtp, true, 115);
  CHECK(rig.last.dominant == FanDominant::Power);
  CHECK(rig.last.pct > 60);
}

static void testFtpZero() {
  Rig rig(FanMode::Hybrid);
  rig.in.ftpW = 0;
  for (int t = 0; t < 30; t++) rig.step(t, 40, true, 100);
  CHECK(!rig.last.on);
}

static void testOlderSampleTimestamp() {
  FanBlend b;
  FanBlendInput in;
  in.mode = FanMode::Hybrid;
  in.ftpW = kFtp;
  in.hrValid = true;
  in.hrUsed = 130;
  in.powerConfigured = true;
  b.onPowerSample(kT0, 0, kFtp);
  in.powerFresh = false;
  b.update(kT0 + 5000, in);
  b.onPowerSample(kT0 + 4999, 250, kFtp);  // 1 ms older than the last decay point
  CHECK(b.powerFiltered() < 1.0f);
}

static void testPresenceExpires() {
  // Pedalling once, then HR missing for longer than the presence time and
  // past the 2^32 ms point: no second fail-safe edge.
  Rig rig(FanMode::Hybrid);
  for (int t = 0; t < 10; t++) rig.step(t, 200, true, 130);
  for (int t = 10; t < 400; t++) rig.step(t, 0, false, kNoHr);
  CHECK(rig.failsafeEdges == 1);
}

static void testSpanTooSmall() {
  const Rig hybrid = ride(FanMode::Hybrid, block(), HrSrc::Model, -1, nullptr, 140, 165);
  const Rig smart = ride(FanMode::Smart, block(), HrSrc::Model, -1, nullptr, 140, 165);
  CHECK(hybrid.cmds == smart.cmds);
  CHECK(hybrid.last.effective == FanMode::Smart);
  CHECK(hybrid.last.reason == FanModeReason::SpanTooSmall);
}

static void testEffectiveMode() {
  Rig rig(FanMode::Hybrid);
  rig.step(0, 200, true, 130);
  CHECK(rig.last.effective == FanMode::Hybrid);
  CHECK(rig.last.reason == FanModeReason::None);
  CHECK(rig.last.power == FanPowerState::Pedalling);
  rig.step(1, 10, true, 130);
  CHECK(rig.last.power == FanPowerState::Idle);
  rig.step(2, 10, true, 130, 60);
  CHECK(rig.last.power == FanPowerState::Pedalling);
  rig.step(6, 0, false, 130);
  CHECK(rig.last.effective == FanMode::Smart);
  CHECK(rig.last.reason == FanModeReason::NoPower);
  CHECK(rig.last.power == FanPowerState::None);
  rig.in.powerConfigured = false;
  rig.step(7, 200, true, 130);
  CHECK(rig.last.effective == FanMode::Smart);
  CHECK(rig.last.reason == FanModeReason::NoPower);
}

static void testFtpChange() {
  Rig rig(FanMode::Hybrid);
  for (int t = 0; t < 240; t++) rig.step(t, kFtp, true, 100);
  const float pff = rig.blend.powerFiltered();
  const int before = rig.last.pct;
  rig.in.ftpW = 350;
  rig.step(240, kFtp, true, 100);
  CHECK(std::fabs(rig.blend.powerFiltered() - pff) < 1.0f);
  CHECK(rig.last.pct >= before - 1);
}

static void testImplausiblePower() {
  FanBlend b;
  b.onPowerSample(kT0, 200, kFtp);
  const float pff = b.powerFiltered();
  b.onPowerSample(kT0 + 1000, -5, kFtp);
  b.onPowerSample(kT0 + 2000, 3000, kFtp);
  CHECK(b.powerFiltered() == pff);
}

static void testEqualReadingsCount() {
  FanBlend b;
  b.onPowerSample(kT0, 0, kFtp);
  b.onPowerSample(kT0 + 1000, 200, kFtp);
  const float first = b.powerFiltered();
  b.onPowerSample(kT0 + 2000, 200, kFtp);
  CHECK(b.powerFiltered() > first);
}

int main() {
  testStepResponse();
  testSampleGaps();
  testSprint();
  testAfterBlock();
  testCooldown();
  testPowerDropoutMidRide();
  testHrLossWhilePedalling();
  testDismount();
  testPresenceEnds();
  testCadencePresence();
  testCoasting();
  testIntervalBand();
  testHoldReact();
  testSmartShaping();
  testFailsafe();
  testSeed();
  testHoldBand();
  testStaleDecay();
  testPresencePreconditions();
  testPowerDominant();
  testFtpZero();
  testOlderSampleTimestamp();
  testPresenceExpires();
  testSpanTooSmall();
  testEffectiveMode();
  testFtpChange();
  testImplausiblePower();
  testEqualReadingsCount();
  if (g_failures != 0) {
    std::printf("fan_blend_test: %d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("fan_blend_test: OK\n");
  return 0;
}
