# Concept — fan modes Pure, Smart, Hybrid (power feed-forward, heart-rate feedback)

Status: **approved** by the maintainer, revision 2.1 (2026-11-08), basis for
the implementation (not started). Revision 1 was decided by the maintainer
after an independent review by two agents with literature research (§9).
Revision 2 folds in a second independent review and the reference
simulation `tools/fan_blend_sim.py` (§8); revision 2.1 the review of
revision 2 (fail-safe integration, mode switch, falsifiable checks) and the
maintainer's decision to drop the interval setting. Parameter values are to
be confirmed in the hardware tests (§8). Feature 1 in `docs/AGENT_FEATURES.md`.
Base: Homewind 1.7.1 (`FanHRAdapter`, `HeartRateSettings`,
`BLERelayManager::lastPower()`, `hw::BikeMetrics`).

Changes in revision 2 (details in the sections):
- Filters use the exact exponential step and run per power sample, separate
  from the command tick. The Euler step of revision 1 diverges once
  `dt > TAU` (sample gaps, or a 12 s tick).
- The heat-load floor is removed from stage 1: with its revision-1 window it
  never changed a fan command in the simulation; `FALL_RATE` carries the
  post-effort cooling.
- `FALL_RATE` is 20 %/min (simulation: fewer commands, narrower 30/30 band).
- Fan OFF only after the shaped output has come down to `MIN_SPEED_PCT`.
- HR fail-safe suspension needs pedalling within the last 60 s; a trainer
  that keeps sending 0 W no longer counts as presence.
- No mean over the update interval; louder is sent at the next tick.
- New WebSocket frame with the device target; the WebUI stops computing the
  fan speed itself.
- API as one action next to the HR actions; no route under `action/fan/`.

Changes in revision 2.1:
- Presence alone decides the fail-safe in Hybrid, whatever the power state
  (a crank power meter that stops sending while coasting no longer stops
  the fan).
- Fail-safe as a derived state with its own edge, reset of the shaped
  output, and OFF also when HR never connected but Homewind switched the
  fans on (§3).
- Mode switch, FTP change, ON condition and new-sample detection defined.
- `FAN_CONTROL` payload layout; dominant input `HR` / `POWER` / `HOLD`.
- Simulation checks made falsifiable (each one fails under a matching
  injected fault, §8); Pure compared against an independent 1.7.1 reference.
- No interval setting in stage 1: all modes keep the fixed 3 s of 1.7.1
  (before the §6.8 seed the simulation was 54 / 51 / 39 commands in 11 min
  of 30/30 at 3 / 6 / 12 s, so a longer tick barely helped; with the seed
  it is 53 / 24 / 21). Revision 1 had 3 / 6 / 12 s.

Changes after the first hardware tests (2026-11-09, maintainer's request):
- Cool-down setting −2..+2 for Smart and Hybrid sets `FALL_RATE` (§4).
- Sprint criterion in §8 rewritten: the fall limit, not the HR level,
  decides the fan after a sprint. Results in §8.

## 0. Decisions

1. **Three switchable fan modes** (heart rate always required):

   | mode | inputs | behaviour |
   | --- | --- | --- |
   | **Pure** | HR | as in 1.7.1, unchanged (regression reference) |
   | **Smart** | HR | HR curve plus the output shaping of §2: louder at once, quieter only slowly (fall-rate limit), minimum useful speed, OFF hysteresis. Addresses the finding that HR falls faster after an effort than body temperature. |
   | **Hybrid** | HR + power | Smart plus the power curve of §2 |

   - **Hybrid** is always **visible**, but **disabled** until an HR sensor and a
     power source are **configured** (set up, not necessarily connected) and
     the HR span is ≥ 30 bpm.
   - **Hybrid with the power source configured but not connected behaves like
     Smart**, and switches back to Hybrid behaviour as soon as power arrives.
   - **HR span later below 30 bpm** (min/max changed while Hybrid is stored):
     Hybrid behaves like Smart and the WebUI says why. Same rule as a removed
     power source.
   - There is no power-only mode.
2. **Settings value: FTP only.** `minW = 0.72 × FTP`, `maxW = 1.05 × FTP`, no
   separate minW/maxW. Everything above FTP runs at full speed anyway; the
   low range is covered by the HR curve.
   - Rationale: heat production and sweat loss follow **absolute** watts
     (core-temperature rise best predicted by W/kg, sweat loss by the required
     evaporation in W; fitness adds only 1–4 % of explained variance, Cramer
     & Jay 2015). For a single rider, FTP is a convenient rescaling of watts.
     It is not a "relative strain" measure for cooling.
   - **Later (stage 2):** learn the start point instead of the fixed 72 %:
     the typical power at which HR sits at the user's `minHR`.
3. **Update interval: fixed 3 s in all modes**, as in 1.7.1
   (`FANHR_UPDATE_INTERVAL_MS`). The selectable 3 / 6 / 12 s of revision 1
   is dropped for stage 1 (maintainer, revision 2.1); it comes back only if
   the hardware tests show too many fan commands.
4. ~~Decay coupled to heart rate (handover).~~ Removed in revision 1: the
   handover works backwards (fast decrease after a long block, slow after a
   sprint). HR falls quickly after an effort (delay ≈ 8 s, then τ ≈ 28 s,
   Storniolo 2020) while core temperature stays elevated for minutes (Kenny
   2006), so an HR-led fan slows down too early — the complaint users have
   about the Elite Aria HR mode (DC Rainmaker 2023).
5. **One power curve (Hybrid), no heat-load floor in stage 1.** Revision 1
   added a slow heat-load floor (≈ 4 min average power) on the
   `minW`–`maxW` window. With that window it never raises a fan command in
   the simulation: after a 4-min block at FTP the fall-rate limit holds the
   output above the floor, and HR stays elevated for minutes (§8, `--sweep`).
   Only a much lower window changes anything: τ 120 s on 0.4–0.8 × FTP raises
   the output 60 s after the block from 69 % to 81 %, but also runs the fan
   at 34 % from power alone during endurance riding with low HR. The floor
   stays a stage-2 option if the hardware tests show a gap.
6. **Power is clamped to `maxW` before filtering**, so a sprint far above FTP
   does not hold the fan at full speed for over a minute.
7. **Fan gets louder at once, quieter only slowly** (fall-rate limit).
8. **Minimum useful speed:** when the fan is on in Smart and Hybrid, at least
   25 %. Below roughly 10 km/h air speed no benefit on core temperature, HR,
   RPE or endurance was found (Otani 2018; Saunders 2005).
9. **HR fail-safe suspended while the rider pedals** (Hybrid only). Presence
   = a power sample > 30 W or cadence > 0 within the last 60 s, whatever the
   power state is now. Fresh 0 W does not count: some trainers keep sending
   0 W after the rider has left. A stale power value does not end presence
   either: crank power meters stop sending while the rider coasts.
10. **HR span ≥ 30 bpm** (`maxHR − minHR`) for Hybrid. The fan lowers HR at
    constant power; the loop gain is ≈ 0.1–0.2 at an 80 bpm span (no
    sustained oscillation), but approaches 1 with the currently allowed
    10 bpm minimum span.

## 1. Idea

- **Pure** (today): fan speed follows the smoothed heart rate between
  `minHR` and `maxHR`. Calm, but late: HR needs 30–90 s to show an effort
  change and about 3 min to settle (τ ≈ 60–70 s on a cycle ergometer,
  Spörri 2022).
- **Smart** (new): like Pure, but the fan gets quieter only slowly after an
  effort, has a minimum useful speed and does not chatter around `minHR`.
  For riders with only a chest strap.
- **Hybrid** (new): power raises the fan within seconds when the effort goes
  up. HR holds the fan up as long as it is the higher input, and remains the
  fallback.

Physical reason: cycling gross efficiency is about 18–23 % (Moseley 2004), so
roughly 80 % of metabolic power becomes heat — about 3.3–4.6 × the
mechanical power. Heat production follows power directly; HR follows it with
a delay and is itself influenced by the fan, hydration and drift. HR is a
useful proxy, not the heat state itself.

## 2. Control law

Target speeds as fractions of `HW_FAN_MAX_SPEED_PERCENT` (95), combined with
`max()`:

```
P_in  = clamp(P, 0, maxW)                        // §0.6
P_ff  : asymmetric filter on P_in

s_hr  = map(HR_smoothed, minHR, maxHR)           // existing FanHRAdapter curve
s_pw  = map(P_ff, minW, maxW)                    // Hybrid with power present
s_raw = s_hr                                     // Smart
s_raw = max(s_hr, s_pw)                          // Hybrid
```

Filter, stepped once per new power sample with the real `dt` since the
previous sample (not once per command tick):

```
if Hold of a level power had reached and P_in > P_eq:   // §6.8
  P_ff = max(P_ff, P_eq)
tau   = (P_in > P_ff) ? TAU_UP : TAU_DOWN        // TAU_UP 4 s, TAU_DOWN 30 s
alpha = 1 - exp(-dt / tau)
P_ff += (P_in - P_ff) * alpha
```

The exact step stays stable for any `dt`. The Euler step `dt / tau` of
revision 1 overshoots for `dt > tau` and diverges for `dt > 2·tau`
(simulation: 0 → 300 W at `dt` = 12 s gives 900, −900, 2700 … W). With
sample gaps of 1–12 s the exact step matches the continuous response.

A new sample is detected per packet, not from a changed value, so two equal
readings in a row both count. Implementation: `BLERelayManager::loop()`
already sets `_powerNew` / `_cadenceNew` from the BikeSensor callbacks (loop
task, inside `update()`); for each it reads `lastPower()` / `lastCadence()`
once and passes the value with its packet time `now − ageMs` to
`FanHRAdapter`. Comparing `now − ageMs` between loops instead would jitter
by a millisecond and report false new samples. With no fresh sample (power
state "none", §3) `P_ff` decays towards 0 with `TAU_DOWN` over the elapsed
time. While the last output was a Hold of a level that power had reached and
`P_in` is above that level, `P_eq = minW + s_prev · (maxW − minW)`, raise
`P_ff` to `P_eq` before the filter step (§6.8); a Hold from HR alone and
any non-Hold leave the filter unchanged.

An FTP change applies to new samples only; `P_ff` is not reset. If `P_ff`
then exceeds the new `maxW`, `s_pw` saturates and `FALL_RATE` handles the
lower target.

Output shaping (Smart and Hybrid; Pure stays as in 1.7.1), evaluated in
`loop()` with the elapsed time:

```
s_out = s_raw                               if s_raw >= s_prev   // louder: immediately
      = max(s_raw, s_prev - FALL_RATE * dt) otherwise            // quieter: 20 %/min
if fan ON:  command = max(round(s_out * 95), MIN_SPEED_PCT)      // §0.8
```

`FALL_RATE` is a fraction of full scale per minute: 20 %/min = 19 command
points per minute (full scale 95).

Then the existing command stage: deadband 3 %, per-fan ACTIVE/INACTIVE,
global power ON/OFF. The 1 % deadband near the bottom only matters for Pure;
Smart and Hybrid never send less than `MIN_SPEED_PCT`. Each command tick
(3 s) sends the current `s_out`; there is no mean over the interval, so a
rise goes out at the next tick. `FALL_RATE` already low-passes every decrease, which is what
the mean was meant to do. With 20 %/min and the 3 % deadband a decrease goes
out as a 3 % step about every 9 s.

Behaviour (simulation, FTP 250 W, HR model with 8 s delay, τ 60 s up / 120 s
down):
1. **Interval start**: `P_ff` follows the new power within ~3 × `TAU_UP`
   (11 s to 95 % for 150 → 300 W). 30 s into a 4-min block at FTP, Hybrid
   sends 78 %, Smart 25 %.
2. **After a 4-min block at FTP, then 0 W**: 84 % at the end, 69 % at +60 s,
   51 % at +120 s, then `MIN_SPEED_PCT` while HR is above `minHR`. The fan
   does not follow the fast early HR recovery.
3. **Sprint** (10 s at 800 W): clamped to `maxW`; `P_ff` brings the fan up,
   `s_pw` is back to 0 37 s after the sprint, `FALL_RATE` makes the rest
   gradual.
4. **30/30 at 1.4 / 0.4 × FTP**: after 3 min the commands stay in an 86–95 %
   band (with the §6.8 seed; 84–93 % without it). The same input without
   `FALL_RATE` swings between `MIN_SPEED_PCT` (25 %) and 93 % every 30 s.

## 3. Inputs, presence and fallbacks

Power has three states, read from `BLERelayManager::lastPower()` and the
`BikeMetrics` stale rule (`hw::kBikeStaleMs` = 3 s):

| power state | meaning | Hybrid uses |
| --- | --- | --- |
| **pedalling** | fresh sample, > 30 W or cadence > 0 | `s_pw`; presence restarts |
| **idle** | fresh sample, ≤ 30 W and no cadence (coasting, stopped trainer) | `s_pw` (decays with `TAU_DOWN`) |
| **none** | stale (> 3 s), not connected or not configured | `s_pw` = 0; `P_ff` decays with `TAU_DOWN` over the elapsed time |

**Presence** (Hybrid) = pedalling within the last 60 s, independent of the
current power state (§0.9).

| HR | presence | behaviour |
| --- | --- | --- |
| valid | – | Hybrid: `max(s_hr, s_pw)` with output shaping; with power "none" this is Smart |
| none | yes | Hybrid: power only; **fail-safe suspended** |
| none | no | **fail-safe** |

Smart and Pure have no presence: HR none = fail-safe, as in 1.7.1.

**Fail-safe as a derived state.** `failsafe = !hrValid && !(Hybrid && presence)`.
- Its rising edge sends power OFF at once and schedules one re-assert after
  10 s (today's behaviour, now keyed to this edge instead of the HR
  disconnect). In Hybrid the edge comes when presence ends, up to 60 s after
  the HR disconnect.
- It sends OFF if HR was connected since boot (today's `everHadHr`) **or**
  Homewind switched the fans on itself. Without the second condition a
  Hybrid ride without a working chest strap would leave the fans running
  after the rider leaves.
- Entering it clears the shaped output `s_prev` and the ON state; leaving it
  starts from 0, so the fan only comes back with a new target (louder at
  once).
- `onHeartRateDisconnected()` clears the HR buffer as today (`s_hr` = 0) but
  must not force the target OFF while Hybrid presence holds.

**ON, hold band, OFF** (Smart and Hybrid):
- ON when `HR_used ≥ minHR` or `s_pw > 0`.
- Hold (state unchanged) while `minHR − 5 ≤ HR_used < minHR`.
- OFF only when HR is below `minHR − 5 bpm` (or missing with Hybrid
  presence), `s_pw` is 0 and the shaped output is down at `MIN_SPEED_PCT`,
  all for 30 s. Avoids ON/OFF chatter around `minHR` (today the fan switches
  OFF as soon as HR < minHR) and a stop from a high speed. Consequence: in
  Hybrid without HR, pedalling below `minW` lets the fan run down and stop.

Other rules:
- Negative or implausible values (> 2500 W) count as no sample.
- All on the loop task; no new task, no new callback (sample hand-over: §2).
- **HR staleness** is not handled in stage 1: today an HR sensor that stays
  connected but stops sending keeps the last value. Changing that would
  change Pure, so it is an open point (§6).

## 4. Settings

- `fanMode`: `PURE` (default, = 1.7.1) | `SMART` | `HYBRID`.
  - `HYBRID` can be selected only while an HR sensor and a power source are
    configured and `maxHR − minHR ≥ 30`.
  - If a precondition is lost later (power source removed, span reduced), the
    stored mode stays `HYBRID`, the control behaves like Smart (§0.1) and
    resumes Hybrid behaviour on its own when the precondition is back.
  - **Mode switch while riding**: entering Smart or Hybrid seeds `s_prev`
    with the speed last sent, so the fan does not drop faster than
    `FALL_RATE`. Entering Pure applies the Pure law at the next tick; a stop
    is correct there when HR is below `minHR` (Pure = 1.7.1). Entering Pure
    without HR while Homewind had switched the fans on (Hybrid on power)
    sends OFF once; the Pure law alone would leave them running.
- `ftp` (W, Hybrid): only power setting. `minW = 0.72 × FTP`,
  `maxW = 1.05 × FTP`. Range 100–500 W, default 200 W.
- `cooldown` (Smart and Hybrid): −2..+2, default 0. Sets `FALL_RATE` to
  30 / 25 / 20 / 15 / 10 % of full scale per minute; +2 keeps the fan up
  twice as long as 0 after an effort. Added after the hardware tests: the
  maintainer found the fall after sprints good, possibly a little longer.
- Compile-time constants: `TAU_UP` 4 s, `TAU_DOWN` 30 s, `FALL_RATE` 20 %/min
  at cool-down 0 (5 %/min per step), `MIN_SPEED_PCT` 25, OFF hysteresis
  5 bpm / 30 s, presence 30 W / 60 s.
- Persistence: separate NVS blob `FanModeSettings` (mode, FTP, cool-down,
  version, CRC16); `HeartRateSettings` stays untouched. Missing or invalid
  blob → `PURE`, 200 W, cool-down 0. Cool-down uses a byte that was reserved
  and written as 0, so blobs from before load as cool-down 0 (same version).

## 5. UI, API, status

- WebUI fan section: mode selector **Pure / Smart / Hybrid**. Hybrid always
  visible, dimmed until HR sensor, power source (configured)
  and a 30 bpm HR span exist. FTP field (Hybrid only). While Hybrid runs as
  Smart, the WebUI says why (no power / span < 30 bpm). Implemented in the
  heart rate card: a three-part selector in the pink header, "FTP (W)" as a
  third field next to Min HR / Max HR that slides in only in Hybrid, FTP
  typed in a dialog. A tap on the blocked Hybrid opens the standard app
  overlay with the reason; "Running as Smart" stays as a line in the card.
  Cool-down (Smart and Hybrid): a light five-part selector XS · S · M · L · XL
  (= −2 … +2, M default) below the fields, styled like the mode selector.
- API: one action `POST /api/v1/action/fanmode/set` with `mode`, `ftp` and
  `cooldown` (all optional, applied together), next to
  `action/heartrate/setmin|setmax`. Routes under `action/fan/` stay reserved
  for fan devices. `mode=HYBRID` without its preconditions returns 409 with
  the reason in the body (409 is already the state-conflict code, e.g.
  "Maintenance active"). Values are read back through the WebSocket frame
  below. Writes are queued to `loop()`.
- New WebSocket frame `FAN_CONTROL` (`0x0E`), 10 bytes, little-endian:
  ```
  [mode:uint8]            // stored mode: 0 Pure, 1 Smart, 2 Hybrid
  [effective:uint8]       // mode in effect (Hybrid can run as 1 Smart)
  [reason:uint8]          // why effective != mode: 0 none, 1 no power, 2 span < 30 bpm
  [target_on:uint8]       // 0 OFF, 1 ON (FanHRAdapter's per-fan state)
  [target_pct:uint8]      // speed acknowledged by the fans, 0..95
  [dominant:uint8]        // 0 HR, 1 POWER, 2 HOLD (FALL_RATE holds the output), 3 fail-safe
  [power_state:uint8]     // 0 none, 1 idle, 2 pedalling (§3)
  [ftp_w:uint16]          // stored FTP
  [cooldown:int8]         // stored cool-down -2..+2
  ```
  Sent on change, at most once per command tick (a mode, FTP or cool-down
  change at once), and once per client after connect. The WebUI shows this target in all
  modes instead of its client-side `calculateFanSpeed()` (`app.js`), which
  only knows the Pure curve. Implemented deviation: both firmware variants
  embed the same WebUI, so `calculateFanSpeed()` stays as the fallback until
  a `FAN_CONTROL` frame arrives, and the heart rate card keeps its 1.7.1
  layout until then (release firmware unchanged). Source: `FanHRAdapter`'s per-fan state (ON and
  speed from the fans' acknowledgements, OFF assumed when sent) and the
  `FanBlend` output; encoder `src/core/FanControlFrame.h`.
- Touch display: optional mode indicator, not required for the first version.

## 6. Open points (decide in the hardware tests)

1. `TAU_UP` 4 s vs. 8–10 s against stroke-to-stroke power noise (slope
   ≈ 1.4 %/W at FTP 200). The `maxW` clamp removes most of the sprint
   argument; the simulation with ±6 % noise stays in the 86–95 % band at 4 s.
2. `MIN_SPEED_PCT` 25 % vs. 30 %; the Xiaomi speed-to-airflow relation is
   not measured — check with an anemometer if possible.
3. `minW`: 0.72 × FTP vs. 0.65 × FTP (a wider span halves the reaction to
   pedalling noise).
4. Should the HR span ≥ 30 bpm also apply to Smart (the fan → HR feedback is
   the same in every HR mode)? Today it is only a Hybrid precondition.
5. HR staleness rule (sensor connected, no samples): needed for Smart and
   Hybrid, but changes Pure. Decide together with a Pure change, not here.
6. Heat-load floor as stage 2, only if the 4 × 4 min hardware test shows
   the fan coming down too early.
7. Known 1.7.1 behaviour, kept in Pure: Pure sets its "HR seen since boot"
   flag only when it sees HR change from missing to valid, and it checks
   only every 5 s while HR is missing. If HR is already valid at that first
   check (about 5 s after the adapter starts), a later HR loss does not
   switch the fans off. Unlikely on the device because BLE starts after the
   fan connect sequence; fix together with any other Pure change (Smart and
   Hybrid use a flag set on every accepted HR sample).
8. Reaction while the fan is held (§8, both hardware rounds). While
   `FALL_RATE` holds the fan above `s_pw`, `P_ff` decays towards the
   recovery power with `TAU_DOWN`. At the next effort it first has to climb
   back to the held level, and the fan keeps falling meanwhile. Fix (in
   `onPowerSample()`): if the last output was a Hold of a level that power
   had reached (`max s_pw` outside Hold is > 0 and at least `s_prev − 0.02`)
   and `P_in` is above that level, `P_eq = minW + s_prev · (maxW − minW)`,
   raise `P_ff` to `P_eq` before the filter step. Outside Hold the peak ages
   at `FALL_RATE`, so an early surge does not seed a later HR Hold. A Hold
   from HR alone (e.g. 1 bpm less) and any non-Hold leave the filter
   unchanged. Using only "power led on the last non-Hold sample" missed the
   first sprint after a 4-min block when the HR model sat slightly above
   `s_pw`; clearing the peak on `s_pw = 0` missed a 0 W recovery.
   Replay of the recorded rides (1 Hz power and HR) through a host copy of
   `FanBlend` with the 3 s / 3-point command stage; seconds from the power
   rise to the first higher command. The replay of the previous law matched
   the hardware within the acknowledgement delay (round 2: 10–11 s against
   12–13 s measured; round 1 sprints: 4–8 s against 5–7 s).

   | Case | Before | Fix | `TAU_UP` 2 s |
   |---|---|---|---|
   | Round 2, 30/30 intervals 2–5, fan at 85–88 % | 10–11 s, drop −3 | 1–4 s, no drop | 7–8 s |
   | Round 1, sprints, fan at 46–53 % | 4–8 s | 1–2 s | 2–5 s |
   | Synthetic: 3 s at 400 W, fan at the HR level (25 %) | ignored | ignored | 59 %, 41 % at +60 s |
   | Synthetic: 3 s at 400 W during Hold at 77 % | ignored | 86 %, 68 % at +60 s (before 56 %) | ignored |

   Side effects: a short effort during Hold now raises the fan (the restart
   after the 10 s coasting in round 2: 55 → 63 %); after the 10 s sprints of
   round 1 the peak rises from 83–86 % to 91 % and the speed 60 s later from
   66–71 % to 73–76 %. Rejected: `TAU_UP` 2–3 s or a higher input clamp
   (1.25–1.5 × FTP) react faster too, but let a 3–5 s spike from the HR
   level drive the fan to 36–95 %, which `FALL_RATE` then holds for 1–2 min.
   Host test `testHoldReact`: 30/30 at 1.4 / 0.4 × FTP with the fan held,
   first higher output ≤ one tick after the rise in intervals 2–5 and no
   drop before it; a 3 s spike at the HR level leaves the output unchanged,
   including right after a 1 bpm HR drop; after a 4-min FTP block (recovery
   at 0.4 × FTP and at 0 W) the next sprint still seeds within one tick; an
   early surge does not seed a later HR Hold; near `minHR` with power never
   on the map the 0.02 margin does not seed. Fails without the seed or if
   every Hold is seeded. Hardware re-check of the 30/30: pass (third round);
   first-round sprints at ~50 % still open.

Decided and removed: mode names and count, Hybrid without power or with a
span < 30 bpm behaves like Smart, FTP vs. minW/maxW, power-only mode,
interval choice (fixed 3 s in stage 1), combination rule (`max()`; a weighted
sum would leak power noise into steady state), handover (removed), mean over
the interval (removed), `FALL_RATE` (20 %/min, simulation), filter step
(exact), heat-load floor (not in stage 1).

## 7. Stages

- **Stage 1** (this concept): §2–5.
- **Stage 2**: learned start point (`minW` from the power at `minHR`);
  heat-load floor if §6.6 asks for it.
- **Stage 3, optional**: CORE body-temperature sensor as an additional slow
  floor, not as primary input (CORE differed from rectal temperature by
  > 0.3 °C in about half of paired readings during cycling, Verdel 2021).
- **Not planned**: full heat-balance model or HR-based core-temperature
  estimator (ECTemp). Needs air speed at the skin, humidity and clothing,
  which are not measured; ECTemp inherits the fan → HR bias.

## 8. Implementation and tests

Implementation:
- Arduino-free filter, combiner, output shaping and presence logic in
  `src/core/FanBlend.h` (pattern of `BikeMetrics.h`, `BikeCard.h`),
  host-testable.
- `FanHRAdapter`: read mode, feed power samples, call `FanBlend`; Pure path
  unchanged.
- Settings, API, WebUI, status as in §4–5. Compile gate
  `HW_ENABLE_FAN_POWER_MODE`; Basic and Touch identical.
- Estimate: about 1.3–1.7k lines over ~12 files (WebUI included), internal
  RAM < 1 KB, flash +10–30 KB. Release gate as in `AGENT_FEATURES.md`.

Reference simulation `tools/fan_blend_sim.py` (Python, no Arduino):
`python3 tools/fan_blend_sim.py` runs the checks below and exits non-zero on
a failure; `--sweep` compares heat-floor variants and `FALL_RATE` values.
Pure uses the integer arithmetic of `FanHRAdapter` (5-sample mean, map,
3 s, deadband) and is compared command by command against an independent
1.7.1 reference; Smart/Hybrid follow §2–3, including the fail-safe state and
the mode switch. Each check was confirmed to fail under a matching injected
fault (no presence limit, cadence ignored, no `FALL_RATE`, Euler step, span rule ignored, no
seeding on mode switch, no reset on fail-safe, Pure off by one). Not
modelled: the two-step power ON / speed of the fan library and command
acknowledgements. The HR model (95 bpm rest, 165 bpm at FTP, 8 s delay, τ
60 s up / 120 s down) is a plausible stand-in, not a measurement. The host
tests port these checks to C++ against `FanBlend.h`.

Host tests (`tests/fan_blend_test.cpp`), all passing in the simulation
(FTP 250 W, minHR 110, maxHR 170):

| check | criterion | simulation |
| --- | --- | --- |
| step 150 → 300 W | `P_ff` reaches 95 % within ~3 × `TAU_UP` | 11 s |
| sample gaps 1–12 s | `P_ff` equals the continuous response | 0.00 W error |
| 10 s at 800 W on 150 W | input clamped; `s_pw` back to 0 within ~3 × `TAU_DOWN` | 37 s |
| 4 min at FTP, then 0 W, HR model | ≥ 55 % at +60 s | 69 % |
| same, HR below `minHR − 5` | no decrease faster than `FALL_RATE`, OFF only from `MIN_SPEED_PCT` | 81 → 63 → 42 % → OFF |
| same, no HR, fresh 0 W | OFF when presence ends (59–64 s) | +60 s |
| same, no HR, power stale | OFF when presence ends (59–64 s) | +60 s |
| same, no HR, power meter 0 W / power stale with CSC, cadence 120 s longer | cadence keeps presence; OFF 60 s after cadence stops (179–184 s) | +180 s / +180 s |
| 10 s coasting, no HR, fresh 0 W / stale | no OFF | none / none |
| 30/30 at 1.4 / 0.4 × FTP, ±6 % noise | band ≤ 20 % after 3 min | 86–95 % |
| 30/30 at 1.4 / 0.4 × FTP, HR below `minHR`, fan held | first higher command ≤ one tick after the rise in intervals 2–5, no drop before it; 3 s at 400 W at the HR level ignored (also after a 1 bpm HR drop) | host test only |
| Pure | identical to the 1.7.1 reference, ON < 25 % and OFF occur | 66 = 66 commands |
| Smart after the 4-min block | shaped ≥ unshaped + 10 at +120 s | 51 vs. 27 % |
| Smart, HR lost | OFF in the same second, one re-assert after 10 s | +0 s, +10 s |
| Hybrid, no HR, after fail-safe | pedalling 100 W does not restart from the old speed | no ON |
| Pure → Smart while HR falls | no step down beyond `FALL_RATE` | 69 → 66 % |
| Smart in the hold band → Pure | OFF at the next tick | +0 s |
| Hybrid with span < 30 bpm | commands identical to Smart | identical |
| cool-down −2..+2 after the 4-min block, HR below `minHR` | drop in the first 60 s = fall rate × 95 (± 2); longer cool-down switches OFF later | host test only |
| Hybrid at FTP, power meter disconnected for 5 min mid-ride (maxHR 190, power dominant) | no step down beyond `FALL_RATE`; ends within 3 of Smart; back: same commands as a meter connected at that moment, same level as before | host test only |
| Hybrid at FTP, HR sensor disconnected (BLE) mid-ride, pedalling stops 5 min later (trainer sends 0 W) | same commands as a ride without HR; OFF 59–64 s after pedalling stops, one fail-safe edge | host test only |
| Hybrid, dismount: HR sensor disconnected and pedalling stopped together, trainer sends 0 W / stops sending | OFF 59–64 s later, one fail-safe edge, stays OFF | host test only |
| same, HR sensor still connected | no fail-safe; OFF later than 200 s (`FALL_RATE`, then HR hold band) | host test only |

For information (§0.3): without the §6.8 seed 54 / 51 / 39 commands in
11 min of 30/30 at a 3 / 6 / 12 s tick (basis for dropping the interval
setting); with the seed 53 / 24 / 21.

Additional host tests: negative/implausible power, new-sample detection with
equal readings, FTP change mid-ride. `tests/fan_mode_blob_test.cpp` covers
the NVS entry, `tests/fan_control_frame_test.cpp` the `FAN_CONTROL` payload.

Hardware tests:
- 30/30 s intervals and 4 × 4 min: first speed command ≤ 6 s after interval
  start while the fan is ON, ≤ 9 s from OFF (power ON acknowledgement before
  the speed command); no stop during 10 s coasting; times from the power
  rise. If the fan is held at ≥ 80 % (within 15 points of `maxPct`) at the
  power rise, a higher speed is hardly noticeable; there the check is: no
  drop of more than one 3-point step and ≥ 90 % within the interval
  (changed after the second hardware test);
  after each 4-min block a gradual decrease over minutes, not following the
  early HR drop
- 3 × 10 s sprints with 2 min rest: fan up within one tick; afterwards a
  gradual decrease at `FALL_RATE` of the cool-down setting (cool-down 0:
  about 19 points per minute), not following the HR drop. Revision 2.1
  asked for "back near the HR level within ~30–60 s"; the fall limit rules
  that out once the fan was high, so the criterion was changed after the
  first hardware test.
- Pure unchanged (regression against 1.7.1, same session)
- Smart: gradual decrease after a 4-min block; no ON/OFF chatter around
  `minHR`
- Hybrid with power meter configured but switched off: behaves like Smart;
  switched on mid-ride: Hybrid behaviour without a jump
- power meter disconnect mid-ride: smooth transition to Smart behaviour
- HR sensor disconnect while pedalling: fan keeps running on power;
  fail-safe 60 s after pedalling stops
- Hybrid ride with the chest strap switched off from boot: fans run on
  power and go OFF 60 s after pedalling stops
- trainer in the PWR slot that keeps sending 0 W after the rider leaves:
  fail-safe after 60 s
- Hybrid visible but disabled/409 without configured power source or with HR
  span < 30 bpm; enabled when the power source is configured even if it is
  not connected; span reduced to < 30 bpm while Hybrid → Smart with hint
- WebUI shows the target from `FAN_CONTROL` in all three modes
- mode switch while riding: Pure → Smart/Hybrid without a drop faster than
  `FALL_RATE`; Smart/Hybrid → Pure follows the Pure law at the next tick
  (stop if HR < minHR); no command burst
- at most one speed command per 3 s tick in all modes
- NVS: settings survive reboot; missing/corrupt blob → defaults
- heap before/after (`HEAP_BENCHMARK.md`)

Hardware test results, first round (2026-11-08/09, Touch, test firmware
`20261108-210742-fanpower`, TICKR FIT, Assioma PRO, Tacx Neo in Zwift ERG;
FTP 200 W, min HR 110, max HR 155). Measured from the `FAN_CONTROL` frame
(acknowledged speed, at most one frame per 3 s), so times include the
fan's acknowledgement; no serial log.
- WebUI: selector, FTP, Hybrid blocked without a configured power source,
  "Running as Smart" with power not connected and with span < 30 bpm: pass.
- Pure (min HR 92 in this phase): commands follow `(HR − 92) / (155 − 92)
  × 95` exactly in 3 s steps, no chatter: pass. Idle 10.4 h: free heap 52.9 KB, largest
  block 31.7 KB, no leak.
- Smart: ON at HR 110 with 25 %, one ON and one OFF around `minHR`; after a
  hard block 53 % → OFF in about 2 min in 3-point steps every ~9 s; at
  +120 s 26 % against 0 % for Pure: pass.
- Hybrid: effective Hybrid throughout; 14 s coasting at 0 W and cadence 0
  kept the fan on; after the 4-min blocks 93 → 75 % and 94 → 85 % in 60 s,
  largest step −3: pass. Heap 51–53 KB free, largest block 31.7 KB.
- Reaction, from the power rise to the first higher speed: 30/30 interval
  1 3.1 s, block 2 5.0 s, sprints 5–7 s. The other 30/30 intervals and
  block 1 are not valid: Zwift ERG pulled the trainer back to about 105 W.
  The sprints miss "within one tick": the fan was held at 46–53 % by the
  fall limit, and `P_ff` (`TAU_UP` 4 s) first has to pass that level. Fixed
  in §6.8; 30/30 re-check pass (third round), sprints at ~50 % still open.
- After the sprints: 86 → 68–71 % at +60 s while the HR curve was about
  40 %; the maintainer found it good, possibly a little longer (cool-down
  setting added).
- Found: after a mode change the frame showed the new mode with the old
  `effective` until the next change; fixed (status sent after the update).
- Still open on hardware: power meter disconnect mid-ride, HR sensor
  disconnect while pedalling, dismount with the trainer sending 0 W, 30/30
  without ERG. The first three are host-tested (table above); each of those
  tests fails under a matching injected fault (stale power kept, HR loss ends
  Hybrid while pedalling, 0 W counts as presence, fall limit removed, power
  loss with valid HR treated as fail-safe). Not covered there: BLE
  disconnect detection and the 3 s `kBikeStaleMs` delay before power counts
  as missing, `powerOffAll()` and its re-assert in `FanHRAdapter`, and a
  strap taken off while the HR sensor stays connected (§6.5, not handled in
  stage 1).

Hardware test results, second round (2026-11-13, Touch, firmware
`20261109-174709-fanpower`, same sensors; FTP 200 W, Hybrid, cool-down 0;
min HR 110, max HR 150, changed in the WebUI from 94/140 one minute into
the warm-up, before the first interval). Plan (guide page with automatic
checks): 2 min 100 W, 5 × (30 s 280 W / 30 s 80 W), 10 s coasting, one
4-min block at 210 W, 3 min 100 W. Deviation from the plan: ridden as a
Zwift ERG workout with the same targets instead of free riding; the trainer
set the planned power (hard intervals 272–286 W on average), so this
replaces the open point "30/30 without ERG". Times measured from the power
rise (first power sample ≥ 210 W for the 30/30, ≥ 180 W for the block; 4–7 s
after the guide's step change) to the first higher acknowledged speed.
- 30/30 interval 1, fan OFF (HR 100–106 below min HR 110): ON acknowledged
  after 3 s, 68 % 6.0 s after the power rise: pass (limit 9 s).
- 30/30 intervals 2–5: fail against "≤ 6 s while ON". At the power rise the
  fall limit had the fan at 86–88 % and still lowered it to 83–86 %;
  `dominant` changed to power after 8–9 s, the first higher speed (89–91 %)
  came after 12–13 s, and 94–95 % after 18–24 s. Same cause as the sprints
  in the first round; here only 7–9 points of headroom were left. The
  maintainer did not notice a delay. Pass under the criterion changed after
  this round (largest drop −3, ≥ 90 % after 12–15 s); fixed in §6.8, hardware
  re-check in the third round.
- 10 s coasting at 0 W: fan stayed on (58 → 55 %): pass.
- 4-min block from 28 %: 60 % 5.8 s after the power rise: pass (limit 6 s,
  close).
- After the block: 92 → 77 % in the first 60 s (−15; guide limit −23 = 19 +
  4 tolerance), largest step −3, while the HR curve was at about 31 %; 92 →
  26 % in about 210 s. After the last 30/30 interval: 94 → 28 % in about
  210 s, until the block started. Both about 19 points per minute: pass.
- During the block `dominant` alternated between power and hold every 3–6 s
  while the speed was 92 % (95 % for one minute); ERG power varied by about
  ±20 W around 210 W. Display only; the speed changed just twice.
- Reboot before the ride: mode Hybrid kept (default Pure). FTP 200 and
  cool-down 0 equal the defaults, so this does not show that they are
  stored.
- Heap during the ride 47.8–53.2 KB free, largest block 31.7 KB.
- Dismount after the plan: pedalling stopped (0 W, 3 s later cadence 0)
  with HR 105 falling to 102; fan OFF 31 s after cadence 0, at HR 101, by
  the HR off rule (smoothed HR below min HR − 5 = 105 for 30 s; it fell
  below 105 as the cadence reached 0). The HR sensor stayed connected, so
  the 60 s presence fail-safe (only without HR) could not act and was not
  exercised.
- Afterwards, rider off the bike and fan already OFF: power meter
  disconnected → effective Smart, reason no_power, 3 s after the sensor
  list showed it disconnected; HR sensor disconnected just under 6 min
  later → fail-safe in the same second. Fan stayed OFF throughout
  (acknowledged state), so this shows the status only: not the fail-safe
  OFF command, and not the disconnects while pedalling.
- Still open on hardware: see third round.

Hardware test results, third round (2026-11-13 evening, Touch, firmware
`20261113-143847-fanpower` with the §6.8 seed; same sensors; FTP 200 W,
Hybrid, cool-down 0, min HR 110, max HR 150). Same Zwift ERG plan as the
second round; the 10 s coasting step ran long (see below).
- 30/30 interval 1, fan at 25 %: first higher speed 43 % after 5.1 s: pass
  (limit 6 s while ON).
- 30/30 intervals 2–5, fan at 86–90 % at the power rise: first higher speed
  after 4–5 s (86 → 92, 86 → 91, 88 → 90, 90 → 93). Intervals 2, 3 and 5
  were Holds; interval 4 was HR-led (so no §6.8 seed) and dipped 88 → 85
  (−3) before rising. Pass against "≤ 6 s while ON" and against the
  high-fan criterion. Second round was 12–13 s with a drop to 83–86 %.
- Coasting: planned 10 s, ridden about 46 s at 0 W; fan stayed on
  (72 → 57 %): pass. The restart (about 4 s at 255–342 W, HR 102–108 below
  min HR) raised the fan 57 → 76 % within 6 s — the §6.8 side effect
  (replay had predicted 55 → 63 %). That left the fan held at 64 % when
  the 4-min block started (28 % in round 2).
- 4-min block from 64 % (seeded Hold): 70 % 3.0 s after the power rise:
  pass. Not a re-check of round 2's 5.8 s from 28 %.
- After the block: for the first ~60 s HR was at or above max HR (guide
  "Puls-Kurve 93 %"), so the fan stayed at 94 % (dominant HR); that is not
  a cool-down check. From 19:56:03, when HR fell below max HR, 94 → 79 % in
  about 53 s (−15), largest step −3, then about 19 points per minute to
  40 % at the end of the plan: pass.
- Heap during the ride 52.9–53.1 KB free, largest block 31.7 KB.
- NVS after the ride: set Hybrid, FTP 215, cool-down 1; API restart; after
  reboot the frame showed Hybrid, FTP 215, cool-down 1 (defaults are Pure /
  200 / 0). Restored to Hybrid, FTP 200, cool-down 0. Pass.
- Still open on hardware: first-round sprints with the fan at ~50 % (the
  30/30 above covers the held-high case); power meter disconnect for several
  minutes while the fan is held high (phase 5 only caught a short disconnect
  at 25 %). No hardware result yet: mode switch while riding, missing or
  corrupt NVS blob, 4 × 4 min, cool-down other than 0.

Hardware test results, phase 5 (2026-11-13 evening, Touch, firmware
`20261113-143847-fanpower`, same sensors; Hybrid, FTP 200, cool-down 0,
min HR 110, max HR 150). Guide + logger `/tmp/hwlog/run8`. Mark buttons were
not used; step timers alone advanced the guide, so two guide CHECKs are
false fails — verdict from the sensor and `FAN_CONTROL` log.
- **A, power meter mid-ride:** only a short disconnect during the warm-up
  (Assioma `conn=0` 20:36:57–20:37:28, ~31 s) while the fan was at 25 %;
  effective Smart `no_power` within ~4 s of the sensor list, Hybrid again on
  reconnect with no jump. The later “A_off” window kept power connected
  (Hybrid throughout, 92 → 74 % under soft pedalling / `FALL_RATE`). Not a
  pass of the high-fan multi-minute case; status transition at low fan: ok.
  Practical note: Assioma pedals stay powered while you pedal, so a true
  battery-off mid-ride is awkward; the intended hardware check is WebUI
  **Disconnect** on the power row (`sensor/disconnect`) while pedalling, then
  Connect again — same BLE-loss path on Homewind.
- **B, HR disconnect while pedalling:** TICKR `conn=0` from 20:43:52; fan
  stayed on at 94 % on power while pedalling: pass. Stopped (0 W) at
  20:47:18; OFF with `dominant` fail-safe at 20:48:16 (**58 s**): pass
  (55–70 s tolerance).
- **C, dismount + HR off, trainer 0 W:** pedalling stopped 20:50:49; HR
  `conn=0` from 20:50:55; OFF fail-safe at 20:51:46 (**57 s** after stop):
  pass. Guide reported “OFF after 0 s” because step C started after the fan
  was already off.
- Heap during the session 47.4–53.9 KB free, largest block 31.7 KB.

Release readiness (2026-11-13): ship as **Homewind 1.8.0** with
`HW_ENABLE_FAN_POWER_MODE` on by default. Default mode stays Pure (= 1.7.1).
Accepted residual risk: power disconnect while the fan is held high not
fully ridden; ~50 % sprint re-check and cool-down ≠ 0 not re-ridden; Basic
board not smoke-flashed with this feature (same gate as Touch).

## 9. Sources (review 2026-11-08)

- Cramer & Jay 2015, heat production vs. core temperature and sweat — https://pmc.ncbi.nlm.nih.gov/articles/PMC4628991
- Moseley et al. 2004, gross efficiency in cycling — https://www.usada.org/wp-content/uploads/R060.pdf
- Saunders et al. 2005, air velocity and heat storage — https://read.qxmd.com/read/15743384/
- Otani et al. 2018, airflow and endurance in cycling — https://vufind.lboro.ac.uk/PrimoRecord/cdi_proquest_miscellaneous_1948760709
- Fernandez et al. 2023, fan cooling at fixed HR (~15 % more workload) — https://scholars.georgiasouthern.edu/en/publications/fan-cooling-improves-submaximal-exercise-capacity-in-an-indoor-th/
- Storniolo et al. 2020, HR recovery kinetics after sprints — https://www.frontiersin.org/articles/10.3389/fpsyg.2019.02950/full
- Spörri et al. 2022, HR kinetics on a cycle ergometer — https://www.frontiersin.org/journals/control-engineering/articles/10.3389/fcteg.2022.894180/full
- Kenny, Carter, Journeay 2006, thermoregulation after exercise — https://nutrition-evidence.com/article/92420/thermoregulatory-control-following-dynamic-exercise
- Coyle & González-Alonso 2001, cardiovascular drift — https://scilit.com/publications/b2c41c04695672f811b81cfb09bf4f36
- Verdel et al. 2021, CORE sensor validity — https://research.birmingham.ac.uk/en/publications/reliability-and-validity-of-the-core-sensor-to-assess-core-body-t
- USARIEM ECTemp algorithm — https://usariem.health.mil/index.cfm/research/products/cbt_algorithm
- DC Rainmaker, Elite Aria review — https://dcrainmaker.com/2023/10/elite-aria-smart-fan-trainer-review.html
- DC Rainmaker, Wahoo Headwind review — https://dcrainmaker.com/2018/07/wahoo-headwind-review.html
