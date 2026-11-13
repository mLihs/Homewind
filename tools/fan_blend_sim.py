#!/usr/bin/env python3
"""Reference simulation for the fan modes Pure / Smart / Hybrid.

Arduino-free model of docs/FAN_POWER_HR_CONCEPT.md: power and HR samples at
1 Hz, exact exponential filter, output shaping, presence and fail-safe, and
the command stage of FanHRAdapter (interval, deadband 3 %, 1 % near the
bottom, max 95 %). Used to pick the parameters in concept §2 and to check the
§8 criteria before any firmware code exists.

  python3 tools/fan_blend_sim.py            # check the §8 criteria (exit 1 on failure)
  python3 tools/fan_blend_sim.py --sweep    # heat-floor variants and FALL_RATE values
"""

import argparse
import math
import random
from dataclasses import dataclass, replace

MAX_PCT = 95          # HW_FAN_MAX_SPEED_PERCENT
DEADBAND = 3          # SPEED_DEADBAND_PERCENT
PURE_INTERVAL = 3     # FANHR_UPDATE_INTERVAL_MS / 1000
FTP = 250.0
MIN_HR, MAX_HR = 110, 170


@dataclass(frozen=True)
class Params:
    tau_up: float = 4.0          # s, P_ff rising
    tau_down: float = 30.0       # s, P_ff falling
    tau_th: float = 240.0        # s, heat-load floor
    min_w: float = 0.72          # × FTP, s_pw = 0 below
    max_w: float = 1.05          # × FTP, clamp and s_pw = 1
    th_lo: float = 0.72          # × FTP, s_th = 0 below
    th_hi: float = 1.05          # × FTP, s_th = 1 above
    fall_rate: float = 30.0      # % of full scale (MAX_PCT) per minute
    min_speed: int = 25          # command % while ON (Smart, Hybrid)
    off_hr_margin: int = 5       # bpm below minHR
    off_hold_s: float = 30.0     # s before OFF
    presence_w: float = 30.0     # W, pedalling above this counts as present
    presence_hold_s: float = 60.0  # s fail-safe suspension after the last pedalling
    min_span: int = 30           # bpm, Hybrid precondition
    interval_s: int = 3          # Smart, Hybrid
    exact: bool = True           # False: Euler step x += (u-x)*dt/tau
    heat: bool = True            # heat-load floor s_th


REV1 = Params()                                   # concept revision 1
REVISED = replace(REV1, heat=False, fall_rate=20.0)  # concept revision 2


def alpha(dt, tau, exact=True):
    return 1.0 - math.exp(-dt / tau) if exact else dt / tau


def ramp(x, lo, hi):
    return min(1.0, max(0.0, (x - lo) / (hi - lo)))


class HrModel:
    """HR = rest + gain × P/FTP, first order with a delay; slower recovery."""

    def __init__(self, rest=95.0, at_ftp=165.0, tau_up=60.0, tau_down=120.0, delay=8):
        self.rest, self.at_ftp = rest, at_ftp
        self.tau_up, self.tau_down = tau_up, tau_down
        self.buf = [0.0] * delay
        self.hr = rest

    def step(self, p):
        self.buf.append(p)
        p_d = self.buf.pop(0)
        target = self.rest + (self.at_ftp - self.rest) * min(p_d, 1.2 * FTP) / FTP
        tau = self.tau_up if target > self.hr else self.tau_down
        self.hr += (target - self.hr) * (1 - math.exp(-1 / tau))
        return int(self.hr + 0.5)


class Controller:
    def __init__(self, mode, prm, min_hr=MIN_HR, max_hr=MAX_HR):
        self.mode, self.p = mode, prm
        self.min_hr, self.max_hr = min_hr, max_hr
        self.p_ff = self.p_th = 0.0
        self.s_prev = 0.0          # shaped target, 0..1
        self.on = False
        self.below_since = None
        self.last_pedal = -1e9
        self.hr_buf = []
        self.failsafe = False
        self.ever_had_hr = False
        self.reassert_at = None
        self.sent_on = False
        self.sent_pct = 0
        self.last_cmd_t = -1e9
        self.commands = []         # (t, on, pct)
        self.reasserts = []
        self.dominant = "HR"
        self.holding = False       # Hold that power had reached; see feed_power
        self.max_pw = 0.0          # peak s_pw outside Hold; ages at FALL_RATE

    # -- inputs
    def feed_power(self, t, w, dt=1.0):
        p = self.p
        w_in = min(max(w, 0.0), p.max_w * FTP)
        if w > p.presence_w:
            self.last_pedal = t
        # §6.8: while Hold of a level power had reached, seed P_ff before the step.
        if self.holding:
            lo, hi = p.min_w * FTP, p.max_w * FTP
            p_eq = lo + self.s_prev * (hi - lo)
            if w_in > p_eq and self.p_ff < p_eq:
                self.p_ff = p_eq
        tau = p.tau_up if w_in > self.p_ff else p.tau_down
        self.p_ff += (w_in - self.p_ff) * alpha(dt, tau, p.exact)
        self.p_th += (w_in - self.p_th) * alpha(dt, p.tau_th, p.exact)

    def feed_cadence(self, t, rpm):
        if rpm > 0:
            self.last_pedal = t

    def decay_power(self, dt=1.0):
        self.p_ff *= math.exp(-dt / self.p.tau_down)
        self.p_th *= math.exp(-dt / self.p.tau_th)

    def push_hr(self, hr):
        if hr is None:
            self.hr_buf = []           # FanHRAdapter clears the buffer on disconnect
            return None
        self.ever_had_hr = True
        self.hr_buf = (self.hr_buf + [hr])[-5:]
        n = len(self.hr_buf)
        return (sum(self.hr_buf) + n // 2) // n

    def set_mode(self, mode):
        """Entering Smart/Hybrid seeds s_prev with the last sent target."""
        if mode != "PURE" and self.sent_on:
            self.s_prev = self.sent_pct / MAX_PCT
            self.on = True
        self.holding = False
        self.max_pw = 0.0
        self.mode = mode
        self.last_cmd_t = -1e9

    # -- control law
    def target(self, t, h, power_present):
        """h: smoothed HR or None. Returns (on, s 0..1, pure_pct) or None for fail-safe."""
        p = self.p
        if self.mode == "PURE":
            if h is None:
                return None
            if h < self.min_hr:
                return (False, 0.0, 0)
            pct = (min(h, self.max_hr) - self.min_hr) * MAX_PCT // (self.max_hr - self.min_hr)
            return (True, 0.0, max(pct, 1))
        hybrid = self.mode == "HYBRID" and self.max_hr - self.min_hr >= p.min_span
        pedalling = hybrid and (t - self.last_pedal) <= p.presence_hold_s
        if h is None and not pedalling:
            return None
        s_hr = ramp(h, self.min_hr, self.max_hr) if h is not None and h >= self.min_hr else 0.0
        s_pw = s_th = 0.0
        if hybrid and power_present:
            s_pw = ramp(self.p_ff, p.min_w * FTP, p.max_w * FTP)
            s_th = ramp(self.p_th, p.th_lo * FTP, p.th_hi * FTP) if p.heat else 0.0
        s_raw = max(s_hr, s_pw, s_th)
        if s_raw >= self.s_prev:
            s = s_raw
            self.dominant = "HR" if s_raw == s_hr else "POWER"
        else:
            s = max(s_raw, self.s_prev - p.fall_rate / 100.0 / 60.0)
            self.dominant = "HOLD" if s > s_raw else ("HR" if s_raw == s_hr else "POWER")
        self.s_prev = s
        if self.dominant != "HOLD":
            if s_pw > self.max_pw:
                self.max_pw = s_pw
            else:
                if self.max_pw > s:
                    self.max_pw = s
                decay = p.fall_rate / 100.0 / 60.0  # fraction of full scale per second
                self.max_pw = self.max_pw - decay if self.max_pw > decay else 0.0
        self.holding = (
            self.dominant == "HOLD" and self.max_pw > 0.0 and self.max_pw + 0.02 >= self.s_prev
        )
        # ON / hold band / OFF
        if (h is not None and h >= self.min_hr) or s_pw > 0 or s_th > 0:
            self.on, self.below_since = True, None
        elif self.on:
            hr_low = h is None or h < self.min_hr - p.off_hr_margin
            if hr_low and s * MAX_PCT <= p.min_speed:
                self.below_since = t if self.below_since is None else self.below_since
                if t - self.below_since >= p.off_hold_s:
                    self.on = False
            else:
                self.below_since = None
        if not self.on:
            self.s_prev = 0.0
            self.holding = False
            self.max_pw = 0.0
        return (self.on, s, None)

    # -- command stage
    def _send(self, t, on, pct):
        self.sent_on, self.sent_pct = on, pct if on else 0
        self.commands.append((t, on, self.sent_pct))

    def step(self, t, h, power_present):
        tgt = self.target(t, h, power_present)
        if tgt is None:                              # fail-safe: OFF at once, one re-assert after 10 s
            if not self.failsafe:
                self.failsafe = True
                self.on, self.s_prev, self.below_since = False, 0.0, None
                self.holding, self.max_pw = False, 0.0
                if self.ever_had_hr or self.sent_on:
                    self._send(t, False, 0)
                    self.reassert_at = t + 10
            elif self.reassert_at is not None and t >= self.reassert_at:
                self.reasserts.append(t)
                self.reassert_at = None
            return
        self.failsafe = False
        self.reassert_at = None
        on, s, pure_pct = tgt
        interval = PURE_INTERVAL if self.mode == "PURE" else self.p.interval_s
        if t - self.last_cmd_t < interval:
            return
        self.last_cmd_t = t
        if not on:
            if self.sent_on:
                self._send(t, False, 0)
            return
        pct = pure_pct if pure_pct is not None else max(int(s * MAX_PCT + 0.5), self.p.min_speed)
        db = 1 if (pct <= 3 or self.sent_pct <= 3) else DEADBAND
        if not self.sent_on or abs(pct - self.sent_pct) >= db:
            self._send(t, True, pct)


def run(mode, prm, power, hr=None, power_valid=None, min_hr=MIN_HR, max_hr=MAX_HR, switch=None,
        cadence=None):
    """power: W per second. hr: None = HR model, else list (None = HR missing).
    power_valid: list of bools (False = no fresh sample). switch: {t: mode}.
    cadence: rpm per second from any source (CSC or power meter), 0 = none."""
    c = Controller(mode, prm, min_hr, max_hr)
    model = HrModel()
    trace = []
    for t, w in enumerate(power):
        hr_t = model.step(w)
        if hr is not None:
            hr_t = hr[t]
        if switch and t in switch:
            c.set_mode(switch[t])
        present = power_valid[t] if power_valid else True
        if present:
            c.feed_power(t, w)
        else:
            c.decay_power()
        if cadence:
            c.feed_cadence(t, cadence[t])
        h = c.push_hr(hr_t)
        c.step(t, h, present)
        trace.append((t, w, hr_t, c.p_ff, c.sent_pct if c.sent_on else 0, c.dominant))
    return c, trace


def pure_reference(hr):
    """Independent FanHRAdapter 1.7.1 law: 5-sample mean, 3 s throttle, deadband."""
    buf, cmds, last_t, sent_on, sent = [], [], -1e9, False, 0
    for t, x in enumerate(hr):
        buf = (buf + [x])[-5:]
        h = (sum(buf) + len(buf) // 2) // len(buf)
        if t - last_t < 3:
            continue
        last_t = t
        if h < MIN_HR:
            if sent_on:
                sent_on, sent = False, 0
                cmds.append((t, False, 0))
            continue
        spd = (h - MIN_HR) * 95 // (MAX_HR - MIN_HR) if h <= MAX_HR else 95
        spd = spd or 1
        db = 1 if (spd <= 3 or sent <= 3) else 3
        if not sent_on or abs(spd - sent) >= db:
            sent_on, sent = True, spd
            cmds.append((t, True, spd))
    return cmds


# ---------------------------------------------------------------- scenarios

def seg(*parts):
    out = []
    for secs, watts in parts:
        out += [watts] * int(secs)
    return out


def noisy(p, amp=0.06, seed=1):
    r = random.Random(seed)
    return [w * (1 + r.uniform(-amp, amp)) if w > 0 else 0 for w in p]


def sent_at(c, t):
    pct = 0
    for ct, on, p in c.commands:
        if ct > t:
            break
        pct = p if on else 0
    return pct


def off_after(c, t0):
    return next((t - t0 for t, on, _ in c.commands if not on and t >= t0), None)


BLOCK = seg((60, 0), (240, FTP), (300, 0))           # block ends at t = 300
P3030 = noisy(seg((60, 0)) + seg(*[(30, 1.4 * FTP), (30, 0.4 * FTP)] * 10))


def check(prm, verbose=True):
    res = []

    def ok(name, cond, info):
        res.append(bool(cond))
        if verbose:
            print(f"  {'PASS' if cond else 'FAIL'}  {name}: {info}")

    no_hr = [None] * 2000
    low_hr = [100] * 2000      # HR present but below minHR - margin: isolates the power path
    tick = prm.interval_s
    fall_tick = prm.fall_rate / 100.0 * MAX_PCT / 60.0 * tick

    # filter
    c, tr = run("HYBRID", prm, seg((120, 150), (60, 300)), no_hr)
    t95 = next(t for t, w, _, pff, *_ in tr if t >= 120 and pff >= 150 + 0.95 * (min(300, prm.max_w * FTP) - 150))
    ok("step 150->300 W: P_ff 95 % within 3 x TAU_UP", t95 - 120 <= 3 * prm.tau_up + 1, f"{t95 - 120} s")
    x, t, err, r = 0.0, 0.0, 0.0, random.Random(7)
    while t < 60:
        dt = r.randint(1, 12)
        x += (300.0 - x) * alpha(dt, prm.tau_up, prm.exact)
        t += dt
        err = max(err, abs(x - 300.0 * (1 - math.exp(-t / prm.tau_up))))
    ok("sample gaps 1-12 s: P_ff equals the continuous response", err < 0.5, f"max error {err:.2f} W")
    c, tr = run("HYBRID", prm, seg((900, 150), (10, 800), (240, 150)), low_hr)
    back = next((t for t, w, _, pff, *_ in tr if t > 910 and pff < prm.min_w * FTP), None)
    ok("sprint 10 s at 800 W: s_pw back to 0 within 3 x TAU_DOWN",
       back is not None and back - 910 <= 3 * prm.tau_down, f"{(back or 0) - 910} s")

    # after a 4-min block
    c, _ = run("HYBRID", prm, BLOCK)
    ok("4 min @FTP, HR model: >= 55 % at +60 s", sent_at(c, 360) >= 55, f"{sent_at(c, 360)} %")
    c, _ = run("HYBRID", prm, BLOCK, low_hr)
    seq = [sent_at(c, t) for t in range(300, 600)]
    steps_ok = all(a - b <= fall_tick + DEADBAND or (b == 0 and a <= prm.min_speed + DEADBAND)
                   for a, b in zip(seq, seq[1:]))
    ok("4 min @FTP, low HR: no step faster than FALL_RATE, OFF only from MIN_SPEED",
       steps_ok and seq[-1] == 0, f"{seq[0]} -> {seq[60]} -> {seq[120]} -> {seq[-1]} %")
    lo, hi = prm.presence_hold_s - 1, prm.presence_hold_s + tick + 1
    for label, valid in (("fresh 0 W", None), ("power stale", [t < 300 for t in range(600)])):
        c, _ = run("HYBRID", prm, BLOCK, no_hr, power_valid=valid)
        d = off_after(c, 300)
        ok(f"4 min @FTP, no HR, then {label}: OFF when presence ends",
           d is not None and lo <= d <= hi, f"+{d} s (window {lo:.0f}-{hi:.0f})")

    cad = [80] * 420 + [0] * 180               # cadence continues 120 s after the block
    lo_c, hi_c = 120 + lo, 120 + hi
    for label, valid in (("power meter 0 W", None), ("power stale, CSC", [t < 300 for t in range(600)])):
        c, _ = run("HYBRID", prm, BLOCK, no_hr, power_valid=valid, cadence=cad)
        d = off_after(c, 300)
        ok(f"4 min @FTP, no HR, {label} + cadence 120 s: OFF 60 s after cadence stops",
           d is not None and lo_c <= d <= hi_c, f"+{d} s (window {lo_c:.0f}-{hi_c:.0f})")

    # coasting and intervals
    for label, valid in (("fresh 0 W", None), ("power stale", [not 120 <= t < 130 for t in range(190)])):
        c, _ = run("HYBRID", prm, seg((120, 200), (10, 0), (60, 200)), no_hr, power_valid=valid)
        ok(f"10 s coasting, no HR, {label}: no OFF", off_after(c, 60) is None, f"{len(c.commands)} commands")
    c, _ = run("HYBRID", prm, P3030, no_hr)
    band = [sent_at(c, t) for t in range(240, 660)]
    ok("30/30 at 1.4/0.4 FTP: band <= 20 % after 3 min", max(band) - min(band) <= 20, f"{min(band)}-{max(band)} %")

    # Pure regression
    m, hr_p = HrModel(), []
    for w in noisy(seg((120, 150), (240, FTP), (300, 0), (120, 150))):
        hr_p.append(m.step(w))
    c, _ = run("PURE", replace(prm, interval_s=12), [0] * len(hr_p), hr_p)
    ref = pure_reference(hr_p)
    low_on = any(on and 1 <= p < prm.min_speed for _, on, p in c.commands)
    has_off = any(not on for _, on, _ in c.commands)
    ok("Pure: identical to the 1.7.1 law (3 s even with interval 12), ON < 25 % and OFF occur",
       c.commands == ref and low_on and has_off, f"{len(c.commands)} commands, reference {len(ref)}")

    # Smart shaping
    c, _ = run("SMART", prm, BLOCK)
    u, _ = run("SMART", replace(prm, fall_rate=1e9), BLOCK)
    ok("Smart: shaped >= unshaped + 10 at +120 s", sent_at(c, 420) >= sent_at(u, 420) + 10,
       f"{sent_at(c, 420)} vs {sent_at(u, 420)} %")

    # fail-safe
    hr_cut = [130] * 100 + [None] * 50
    c, _ = run("SMART", prm, [0] * 150, hr_cut)
    ok("Smart, HR lost: OFF in the same second, one re-assert after 10 s",
       off_after(c, 100) == 0 and c.reasserts == [110], f"OFF +{off_after(c, 100)} s, re-assert {c.reasserts}")
    p = seg((300, FTP), (120, 0), (60, 100))
    c, _ = run("HYBRID", prm, p, no_hr)
    resumed = [x for x in c.commands if x[0] >= 420 and x[1]]
    ok("Hybrid, no HR: after fail-safe, pedalling 100 W does not restart from the old speed",
       off_after(c, 300) is not None and not resumed, f"{resumed[:1] or 'no ON command'}")

    # mode switch and span
    hr_drop = [160] * 300 + [130] * 200
    c, _ = run("PURE", prm, [0] * 500, hr_drop, switch={302: "SMART"})
    before, after = sent_at(c, 301), [x[2] for x in c.commands if x[0] >= 302][:1]
    ok("Pure -> Smart while HR falls: no step down beyond FALL_RATE",
       after and after[0] >= before - fall_tick - DEADBAND, f"{before} -> {after[0] if after else None} %")
    hr_band = [140] * 300 + [108] * 200       # 108 bpm: inside the Smart hold band
    c, _ = run("SMART", prm, [0] * 500, hr_band, switch={400: "PURE"})
    ok("Smart holding in the band -> Pure: OFF at the next tick",
       off_after(c, 300) is not None and 400 <= 300 + off_after(c, 300) <= 400 + PURE_INTERVAL,
       f"OFF at t={300 + (off_after(c, 300) or 0)}")
    a, _ = run("HYBRID", prm, BLOCK, min_hr=140, max_hr=165)
    b, _ = run("SMART", prm, BLOCK, min_hr=140, max_hr=165)
    ok("Hybrid with span < 30 bpm behaves like Smart", a.commands == b.commands, f"{len(a.commands)} commands")

    if verbose:
        counts = []
        for iv in (3, 6, 12):
            c, _ = run("HYBRID", replace(prm, interval_s=iv), P3030, no_hr)
            counts.append(len(c.commands))
        print(f"  info  commands in 11 min 30/30 at interval 3/6/12 s: {counts}")
    return all(res)


def sweep():
    print("Heat floor vs none, 4 min @FTP then 0 W (sent % at +0/+30/+60/+120 s):")
    variants = [("none", REVISED)] + [
        (f"tau {t} s, {lo}..{hi} FTP", replace(REVISED, heat=True, tau_th=t, th_lo=lo, th_hi=hi))
        for t in (120, 180, 240) for lo, hi in ((0.4, 0.8), (0.5, 0.9), (0.72, 1.05))]
    for name, q in variants:
        rows = [[sent_at(run("HYBRID", q, BLOCK, hrs)[0], 300 + d) for d in (0, 30, 60, 120)]
                for hrs in (None, [100] * 700)]
        endu = sent_at(run("HYBRID", q, seg((1800, 0.55 * FTP)), [100] * 1800)[0], 1799)
        print(f"  {name:26} HR model {rows[0]}  low HR {rows[1]}  30 min @0.55 FTP, low HR: {endu} %")
    print("\nFALL_RATE (% of full scale per min): +60 s after the block (HR model), 30/30 band, commands")
    for fall in (20, 30, 45, 60):
        q = replace(REVISED, fall_rate=fall)
        a = sent_at(run("HYBRID", q, BLOCK)[0], 360)
        c2, _ = run("HYBRID", q, P3030, [None] * 2000)
        band = [sent_at(c2, t) for t in range(240, 660)]
        print(f"  {fall:3}  +60 s {a:3} %   band {min(band)}-{max(band)} %   {len(c2.commands)} commands")
    c, _ = run("HYBRID", replace(REVISED, fall_rate=1e9), P3030, [None] * 2000)
    band = [sent_at(c, t) for t in range(240, 660)]
    print(f"  off  band {min(band)}-{max(band)} % (MIN_SPEED kept)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sweep", action="store_true")
    if ap.parse_args().sweep:
        sweep()
        return
    print("Filter stepped once per command tick, dt = 12 s, step 0 -> 300 W, TAU_UP 4 s:")
    for exact in (False, True):
        x, xs = 0.0, []
        for _ in range(6):
            x += (300.0 - x) * alpha(12.0, REV1.tau_up, exact)
            xs.append(round(x))
        print(f"  {'exact' if exact else 'Euler'}  P_ff = {xs}")
    print("\nRevision 2 (exact filter, no heat floor, FALL_RATE 20 %/min):")
    ok = check(REVISED)
    print("\nALL PASS" if ok else "\nSOME FAIL")
    raise SystemExit(0 if ok else 1)


if __name__ == "__main__":
    main()
