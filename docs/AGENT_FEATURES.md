# AGENT_FEATURES.md — Homewind feature backlog

Feature list for an agent working on Homewind and the BikeSensor library
(BBS). Ordered by priority. Every feature starts with a design note that the
maintainer approves before code is written. Facts marked **verify** are not
yet confirmed on hardware or against a primary source.

Rules
- **RAM gate** per feature, measured on the Touch with HR + CSC + PWR, Zwift
  and the WebUI connected (`HEAP_BENCHMARK.md`): steady free internal heap
  ≥ 40 KB, minimum since boot ≥ 30 KB, largest free block ≥ 25 KB.
  Baseline: 47.8–52.8 KB free, minimum 38.9 KB, largest block 31.7 KB,
  measured with a 1.7.0 diagnostic build + HomeWindWSAmoled 1.6.1 (the
  content of 1.7.1); re-measure on the 1.7.1 release build before the first
  feature.
  Each new BLE link or GATT service costs internal RAM; measure it.
- BLE protocol work goes into BBS; Homewind only wires, stores settings and
  shows UI.
- Every feature ships behind a compile-time gate (default off until validated)
  and must not change the Basic build unless intended.
- Measure heap (`t`/`h`, `HEAP_BENCHMARK.md`) before and after each feature.

## Current state (verified 2026-11-14, Homewind 1.8.1, BikeSensor 3.2.2)

- BBS relays HR (0x180D), CSC (0x1816) and Cycling Power (0x1818) as
  **notify-only** characteristics. There is no FTMS service and no writable
  characteristic, so Zwift sees Homewind as a power source, never as a
  controllable trainer. On HR disconnect the relay notifies centrals once
  with 0 bpm (3.2.2).
- Homewind has three sensor slots (HR, CSC, PWR); `tools/build.sh` sets
  `-DBBS_COMPILE_POWER=1`, `static_assert`s enforce it. Cadence: CSC while
  connected, else the power meter. Fan modes Pure / Smart / Hybrid shipped
  in 1.8.0 (default Pure).
- BLE links: sketch-folder `sdkconfig.h` raises
  `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` from 3 to **4** = three sensors + one
  central (Zwift/Rouvy). All four are in use when HR + CSC + power are set up.
- Multi-type sensors: a trainer advertising CSC + Cycling Power is stored for
  the slot it was discovered with (`bleResolveDiscoveryType()`), so it can
  sit in the PWR slot today as a read-only power source. A trainer that
  advertises only FTMS (0x1826) is not found by the PWR discovery.
- NimBLE-Arduino 2.5.2+hw1 fixes the crash when a connection handle exceeds
  `MAX_CONNECTIONS`. Any further increase must be re-tested against this fix.
- Touch RAM: 1.7.1 halved the display draw buffers (about 32 KB more internal
  RAM); the figures in the RAM gate above are the current baseline.
- BBS `src/` line budget: 2,600 non-blank, non-`//` lines, set in the BBS v3
  design (`Refactoring/BBS_V3_DESIGN.md` rev 53, outside the repositories);
  2,587 used on 2026-11-08.
  An FTMS client and server do not fit without a budget decision.

---

## 0. Power relay — done in 1.7.0

Kept here as the base for 1 and 2. Nothing to do.

## 1. Fan modes Pure / Smart / Hybrid — done in 1.8.0

Concept: `docs/FAN_POWER_HR_CONCEPT.md`. Shipped with `HW_ENABLE_FAN_POWER_MODE`
on by default; default mode Pure (= 1.7.1). Residual hardware gaps listed in
the concept §8 release readiness note.

## 2. Smart-trainer support — FTMS trainer proxy

- **Goal**: Homewind connects to the trainer as a client and offers Zwift one
  device that is HR + cadence + power **and controllable** (ERG, simulation).
- **Why a proxy**: some trainers accept only one BLE central (Tacx NEO
  series, Van Rysel D100 — **verify** per model). Once Homewind holds that
  link, Zwift can only reach the trainer through Homewind, so Homewind must
  forward everything, including control. Newer Wahoo KICKR models often
  accept several BLE connections (**verify** per FW); there Zwift can also
  connect directly, and the proxy is a convenience, not a necessity.
- **Open architecture decision (before any design note):**
  - Where FTMS lives: BBS (needs a line-budget raise, see current state) or
    a separate library that Homewind wires next to BBS.
  - How an FTMS-only trainer is discovered and stored (new sensor type or
    role, see below); today the PWR slot discovers by Cycling Power.
- **Design question: roles instead of slots (sensor setup / WebUI).**
  Today the setup has fixed slots (HR, CSC, PWR). With a trainer, one device
  can deliver several things, and riders combine devices, e.g. power meter +
  CSC, or power meter + CSC + trainer. Proposal to work out:
  - **Heart rate**: always required (fan modes depend on it).
  - **Power source**: selectable — power meter or trainer.
  - **Cadence source**: selectable — CSC sensor, power meter or trainer.
  - **Control**: from the trainer, if one is set up.
  - **Duplicate data rule**: when power meter and trainer both send watts,
    the user picks the source; default the power meter on the bike (many
    riders prefer it). Same for cadence.
  - Consequences to design: NVS layout (devices + role assignment instead of
    fixed slots), discovery per device rather than per type, WebUI setup
    flow, migration from the 1.7 v3 blob, connection budget (below).
- **Connection budget**: the trainer takes the PWR link and gains the FTMS
  client role on top — no 5th BLE link. Trainer **and** separate power meter
  at the same time would need 5 links: out of scope (would mean HR + CSC +
  power meter + trainer + Zwift).
- **Covers (FTMS 0x1826, indoor bike)**: in principle any trainer that
  exposes Fitness Machine as an **indoor bike** with Indoor Bike Data +
  Control Point (ERG and/or simulation). Examples: Tacx **Flux** S/2 (not
  NEO — no FTMS); newer Wahoo KICKR/Core/Snap **with FTMS in FW** (KICKR
  ’17 FW 3.4.68 dump: **no** `0x1826`, only CPS + Wahoo proprietary — see
  `BluetoothBikeSensorServer/docs/trainers/Wahoo_KICKR_7D0B_2017.md`);
  Elite (Suito, Direto, Zumo, Justo …); Saris H3/H4; Kinetic R1; JetBlack
  Volt; Magene T300; Zwift Hub; Decathlon Van Rysel D100/D500/D900 (D100:
  FTMS only, often one connection — **verify**). Feature bits and quirks
  differ per model; the proxy mirrors what the trainer reports, it does not
  invent capabilities.
- **Cadence:** Wahoo KICKR does **not** provide cadence (maintainer). With
  KICKR as the only bike source, pair a CSC sensor (or a power meter that
  reports cadence). Indoor Bike Data from an FTMS KICKR may still omit
  instantaneous cadence — treat cadence as optional from the trainer.
- **Server side (Homewind → Zwift)**: Fitness Machine Feature 0x2ACC,
  Indoor Bike Data 0x2AD2 (notify), Training Status 0x2AD3, Supported
  Resistance Level Range 0x2AD6, Supported Power Range 0x2AD8, Control Point
  0x2AD9 (write + indicate), Fitness Machine Status 0x2ADA (notify).
- **Client side (Homewind → trainer)**: discover 0x1826, read Feature and
  ranges and mirror them, subscribe Indoor Bike Data, Status and Control
  Point indications.
- **Control Point forwarding**: Request Control 0x00, Reset 0x01, Set Target
  Resistance 0x04, Set Target Power 0x05, Start/Resume 0x07, Stop/Pause
  0x08, Set Indoor Bike Simulation 0x11. Forward each write 1:1 and return
  the trainer's response (0x80, opcode, result) to Zwift. Never answer on the
  trainer's behalf except "control not permitted" when the trainer link is
  down.
- **State machine (part of the design note, before code)**: control
  ownership (Zwift asked / trainer granted), one outstanding Control Point
  request at a time with a timeout, trainer disconnect mid-workout
  (resistance falls back to trainer default), Zwift disconnect (send Reset or
  leave as is?), reconnect of either side.
- **Known issues (verify)**: Tacx NEO 2T firmware 2.6.0 has user reports of
  BLE problems; record the trainer firmware in every test report.
- **First step, no code**: nRF Connect dump of the NEO 2T — done 2026-11-14
  (`BluetoothBikeSensorServer/docs/trainers/Tacx_Neo_2T_14189.md`). Discovery
  only (no Feature/FW value reads yet). Exposes CSC, Cycling Power and Tacx
  FE-C over BLE (`6E40FEC1-…`), **not** FTMS `0x1826`.
- **NEO vs FTMS (confirmed 2026-11-14):** the Tacx **NEO series has no native
  Bluetooth FTMS**; Garmin/Tacx use proprietary BLE and ANT+/FE-C. Reason
  given by Garmin/Tacx: FTMS does not carry the virtual-flywheel mass /
  algorithms the NEO needs. **Flux** (and similar) Tacx lines do support
  FTMS. So §2 (FTMS proxy) covers Flux / FTMS-capable Wahoo / Elite / …;
  controllable NEO support is §3 (FE-C), not a firmware toggle on the NEO.
- **KICKR ’17 dump (2026-11-14):** `Wahoo_KICKR_7D0B_2017.md` — CPS + Wahoo
  proprietary services, **no FTMS**, **no CSC**. Control on this FW is not
  §2 until a FTMS-capable Wahoo dump exists.
- **Tests**: ERG step 150 → 300 → 150 W (trainer follows within the time it
  takes without Homewind); simulation grade 0 → 8 → −3 %; Zwift reconnect
  5×; trainer power cycle mid-ride; 1 h ride with HR + trainer; fan control
  from trainer power; heap delta against the RAM gate. Prefer an FTMS
  trainer (e.g. Flux) for §2 hardware tests; NEO for §3.
- **Priority**: High (after the architecture decision)

## 3. Smart-trainer support — Tacx "FE-C over BLE" (NEO and older Tacx)

- **Goal**: same Zwift-facing proxy as 2 for Tacx trainers **without** FTMS
  (entire NEO series, plus older FE-C-over-BLE models).
- **Covers**: Tacx NEO (Gen 1), NEO 2, NEO 2T, NEO 3M (**no FTMS by design**);
  Flow Smart, Vortex Smart, Bushido Smart, Genius Smart (Garmin groups these
  as one protocol class on developer.garmin.com/tacx-trainer). Flux and
  other FTMS Tacx stay under §2.
- **Protocol**: proprietary Tacx service carrying ANT+ FE-C pages over BLE
  (service `6E40FEC1-…`, RX/TX characteristics — seen on the NEO 2T dump).
  Full specification only via Garmin's "Request Access". Data pages e.g.
  0x10 general, 0x19 trainer data; control pages e.g. 0x30 resistance, 0x31
  target power, 0x33 track resistance (**page map still verify** against the
  Garmin docs once access is granted).
- **Design**: translate FTMS Control Point requests from Zwift into FE-C
  control pages, and FE-C data pages into Indoor Bike Data. Reuses the server
  side of feature 2.
- **Limits**: NEO Gen 1 has slower Bluetooth (1 Hz vs. 4 Hz on NEO 2 series).
- **Tests**: as feature 2, on one FE-C-only trainer.
- **Priority**: Low (proprietary, shrinking user base)

## 4. Side note — treadmills and rowers

Idea only, not planned; not yet listed in `docs/08_OPEN_TOPICS.md`.
- **Treadmill**: read-only relay (FTMS Treadmill Data 0x2ACD or RSC 0x1814)
  for Zwift Run. No control point. Domyos treadmills need FTMS enabled once
  in the E-Connected app.
- **Rower**: Zwift does not accept rower data; output would be Cycling Power.
  Concept2 PM5 (firmware > 150) and Domyos rowers offer FTMS.
- **Priority**: Lowest

## 5. Network — DHCP hostname, mDNS identity, ARP check — done in 2.2.0

Plan: `docs/AGENT_NETWORK_REACHABILITY.md`. Client counterpart:
`Homewind-Bike-Computer/docs/HOMEWIND_DISCOVERY.md` (Karoo extension 0.4.1).
Shipped: hostname before `WiFi.mode(STA)`, mDNS once with TXT + `_homewind._tcp`,
`device_id`/`hostname` in `system/info`. Task 0 measured on Touch (BLE + app):
ARP loss refuted; Task 4 not shipped.

---

## Report format per feature

Design note (approved), commit hashes, compile gate name, heap before/after
against the RAM gate, test ids with pass/fail, devices and firmware versions
used, open questions.
