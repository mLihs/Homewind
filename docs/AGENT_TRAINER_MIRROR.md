# Agent task: trainer mirror — finish gate G5, then build

Read first: `docs/TRAINER_PROXY_DESIGN.md` (rev 8) and `docs/AGENT_FEATURES.md` § 2.
Website / support copy (EN+DE, publish at Trainer GA): `Website/docs/WEBSITE_TRAINER_MIRROR.md`.
Hardware: Tacx NEO 2T (local), spare ESP32-S3 (XIAO) with `tools/trainer_mirror_relay`,
iPad and Apple TV with Zwift, Assioma pedals, HR strap. Mac for builds and captures.

## Binding decisions (do not reopen)
- Mirror the trainer's complete GATT and relay everything unchanged in both directions;
  DFU is not mirrored. No translation (FTMS etc.), no Homewind-side limits, no control
  logic. Advertised name stays `homewind-…`; the trainer's DIS, service data and
  appearance are copied (truthful, not imitation).
- Trainer lost → disconnect the app. Advertise only while the trainer is connected.
- Priorities: cadence CSC → power meter → trainer; speed trainer; HR strap. Power toward
  the app = trainer mirror unchanged by default; power meter for fan/WebUI/Touch. App CPS
  rewrite only if G5 `cps` proves Zwift uses CPS. Besides HR at most two devices. 4 BLE links.
- Apple GATT cache: no Service Changed; **BLE address derived from GATT structure hash
  when the mirror is active (mandatory, rev 8).**
- CCCD reference count `{app, internal}` so read-along survives app disconnect (rev 8).
- RAM: no tuning now; release check (heap, largest block, RAM) before the final R1.
- Clean room: no code from GPL/AGPL projects (see `TRAINER_PROXY_REFERENCE_SURVEY.md`).

## State (2026-11-15)
- Design rev 8 (Opus dispositions: address, CCCD refcount, power internal by default).
- `tools/trainer_mirror_relay` v0.3 (standalone, NimBLE-Arduino 2.5.2+hw, XIAO ESP32-S3):
  reads the NEO 2T GATT (`scan`, `use N`), stores it, reboots, mirrors it as
  `homewind-test` with a simulated HR (120 bpm). Lessons so far:
  - start the GATT server before any link exists (NimBLE resets GATT on `start()`;
    otherwise assert `ble_svc_gap_init`);
  - use `discoverAttributes()` for a full discovery;
  - copy the trainer's advertising service data (FC82 `01`) and appearance (0x0480).
- iPad result (2026-11-14, no serial file): Zwift pairs `homewind-test` as Power Source,
  Resistance (Zwift-Ready icon), Cadence, Heart Rate from one device. ERG 100 → 200 W
  followed correctly and at once.

## State (2026-11-16) — G5 passed, maintainer confirmed
Details and logs: design § 6 "G5 results". Key findings for M1:
- Zwift (iPad, Apple TV) controls the NEO 2T **only via FC82** and takes power from FC82;
  CPS replacement (`cps`) has no effect → **M2b dropped**.
- The stored table must be complete; a discovery without FC82 characteristics leaves Zwift
  at "Connecting…" with 0 W. Validate the table (expected service set) before storing.
- Link coupling works: trainer power cycle 5× and Zwift restart 5× recover without user
  action (5–11 s). NEO stays up for minutes without mains; it switches off ~3 min after
  pedalling stops — show "trainer off", not an error.
- Delay inside the ESP: median 0 ms both ways; outliers up to 253 ms only during the
  blocking upstream `subscribe` calls right after an app connect (v0.3) → M1 non-blocking.
- 60 min Apple TV ride: no disconnect, drop 0, heap flat. Power match with Assioma works.
- ESP32-S3 HWCDC: `Serial.printf` blocks without a host reading; always run
  `tools/g5_logger_daemon.sh` (FIFO commands: `echo s > /tmp/g5_cmd_fifo`) when testing.

Decisions 2026-11-16: BBS module behind `BBS_COMPILE_TRAINER=1`, budget raise accepted
(Q1); G3/G4 covered by G5; G2 short check, non-blocking; G1 before M2a.

## State (2026-11-17) — M1 implemented, three independent Opus reviews, not on hardware
- BBS (uncommitted, no version bump): `src/internal/BBSTrainer.h`, `BBSTrainerNimBLE.h`,
  `BBSTrainerTable.h`, hooks in Core/NimBLEPort/Types/Config, API in `BikeSensor.h`,
  `tests/trainer_test.cpp`, `examples/Trainer`. Host tests 16/16; flag off code-identical
  to the previous release; `src/` 3,999 of 4,000 counted lines (budget confirmed).
- Value cap 104 B (FC82 `…0004` is ~70 B); stored table layout v3 — re-run setup.
- **Open risk for M2a / G7:** flag on adds ~19.9 KB static RAM (55,220 vs 35,300 B on
  the XIAO example), mostly the 48-event queue, write/indication FIFOs and two tables at
  104 B per value. Touch baseline lowest free internal was 38.9 KB (F18), G7 wants ≥ 20 KB.
  Measure G1 first; if tight, shrink the queue entries (variable-length values) or move
  the buffers to PSRAM on Touch before M2a.
- arduino-cli with `--output-dir` makes core 3.3.12 copy binaries into
  `examples/*/build`; add `--build-property recipe.hooks.savehex.fqbn_to_dir=false`.

## State (2026-11-17 evening) — M1 bench passed on NEO 2T + Zwift Apple TV
- Setup fix in `BBSTrainer.h` (`seen_` kept during the setup scan) + regression in
  `trainer_test.cpp`; host tests 16/16; not yet independently reviewed.
- ERG, power, cadence OK; relay reset ×5 and Zwift restart ×5 re-paired automatically.
  Details in TRAINER_PROXY_DESIGN § 6 "M1 bench".
- Forced trainer-side drop (NEO switched off) recovered automatically; G2 passed (NEO not
  listed in Zwift while mirrored).
- MTU fix in `BBSTrainerNimBLE.h` (NEO exchanges first → `BLE_HS_EALREADY`; take
  `ble_att_mtu()`); trainer mtu 107 on hardware.
- Both fixes independently reviewed (Opus, no blocker); review's `ble_att_mtu() == 0`
  guard and comment fixes applied; host 16/16; hardware mtu 107/247.
- `src/` now 4,004 counted lines; budget raised to 4,050 (maintainer, 2026-11-17).
- G1 measured 2026-11-18 (TRAINER_PROXY_DESIGN § 6): lowest free internal 44.5 KB
  without mirror → ~24.6 KB expected with M1 static RAM.
- RAM decision (maintainer 2026-11-18, independently reviewed): `BBS_TRAINER_PSRAM`
  (default 0) allocates the Trainer (~19.5 KB) once in PSRAM, else internal RAM. M2a sets
  it only in the Touch build behind the display gate (workspace PSRAM rule). XIAO with
  PSRAM=opi: static 55,220 → 36,328 B, 21,972 B PSRAM used, setup store/reload and NEO
  connect OK; trainer link held 10.5 h, Zwift auto-reconnected, ERG ride 56 writes, errors
  0 (`logs/m1_psram_20261118-084113.log`). Still internal: the mirrored GATT server at runtime (est. 6–10 KB) and the
  extra link — G7 measures it. M2a diagnostics should log whether the Trainer is external.

## Task 1 — finish G5 with logs
Setup: flash the relay (`arduino-cli compile/upload`, FQBN `esp32:esp32:XIAO_ESP32S3`,
`--library <NimBLE-Arduino workspace copy>`). Log every run with
`python3 tools/serial_log.py <port> logs/g5_<device>_<test>.log` (host timestamps; type
commands into it). Remove `homewind-test` in Zwift and Bluetooth settings before
switching devices.

| # | Test | Record |
| --- | --- | --- |
| 1 | iPad ERG 100 → 200 → 100 W, `homewind-test` as power source | control characteristic Zwift writes (FC82 `00000003-19ca-…` or FE-C `6e40fec3-…`), bytes of each target, relay delay `+ms` (median/max), power in Zwift vs NEO |
| 2 | iPad ERG with Assioma as power source (power match) | same; does Zwift regulate the NEO to target |
| 3 | iPad free ride, grade changes | messages for grade; felt resistance |
| 4 | `cps` ON (CPS power replaced by 200 W), `homewind-test` as power source | does Zwift show 200 W (power from CPS) or real power (from FC82/FE-C) |
| 5 | NEO power cycle mid-ride; Zwift restart ×5 | app disconnected on trainer loss, mirror back, Zwift re-pairs automatically |
| 6 | Apple TV: tests 1, 3, 5 | as above + Bluetooth slots: does a second device (e.g. Zwift Click) still pair |
| 7 | 60 min ride | heap lines (`heap internal free … largest …`), drops (`drop`), no reset |

Deliver: `logs/` files, filled table in `docs/TRAINER_PROXY_DESIGN.md` § 6 "G5 results",
short findings (control path FC82 vs FE-C, delay, power-source behaviour, anything that
broke). If something fails: fix the sketch (it is throw-away code), retest, document.

## Task 2 — reference capture (G3) and full dump (G4)
- G4: nRF Connect on Mac/iPhone: read all NEO characteristics, export the full log.
- G3: iOS Bluetooth logging profile on the iPad + PacketLogger on the Mac while Zwift
  controls the NEO directly (no relay): pairing, ERG step, grade change. Compare with the
  relay log from Task 1 (same characteristics, same bytes?).

## Task 3 — after G5 passes: M1 (BBS module)
G5 confirmed 2026-11-16 — **this is the next task.** Follow design § 2 and § 8 (M1):
table codec + NVS + hash, mirror build in `serverStart()`, relay FIFO (CCCD and writes in
order), indication queue (8, one outstanding), link coupling, DFU skip, non-blocking
`ble_gattc_*` calls, write type from the trainer's properties. Host tests first.
Module inside BBS with budget raise: DECIDED 2026-11-16 (design Q1).

## Task 4 — M2a plan (rev 2, 2026-11-18, reviewed once, approved)
**DECIDED 2026-11-18 (maintainer):** D1 in BBS with a budget raise; D2 power meter →
trainer, fix the § 3 table; D3/D6 `trn_dev` lite + `trn_on`, setup OK turns the mirror on;
D4 AGENT_FEATURES gate as target, G7 as hard minimum, maintainer decides if only G7
passes; D8 setup removes the power slot with a notice. D5 and D7 as recommended. Next:
maintainer commits M1 + pending changes, then the D4 re-baseline, then implementation
with independent review.
Goal (design § 8): Homewind uses the M1 mirror. Trainer row in the WebUI with setup,
reboot, status and notices; mirror on/off ("Release trainer"); trainer power/cadence for
fan Hybrid, `BIKE_METRICS`, WebUI and Touch; device rule; Touch with the Trainer in PSRAM.
Map of the touch points: Explore report 2026-11-18 (`BLERelayManager`, `BikeMetrics`,
`FanHRAdapter`, `DisplayManager`/`BikeCard`, `app.js` `BLESensorManager`, `ApiActions`,
`build.sh` `write_build_stamp`).

### Decisions needed before code (recommendation first)
- **D1 Decoding (rule: BLE protocol work in BBS).** M1 hands Homewind raw bytes
  (`setTrainerDataCallback`). Recommended: BBS reads along 0x2A63 only (NEO CPS flags
  0x0030 carry power and crank; G5 log). Power via a new `trainerPower()` getter +
  callback (no change to `PowerSample` and its consumers); trainer crank as the third
  source in BBS's existing cadence selection (`BBSCore.h:1236-1267`, design § 2.1), so
  `lastCadence()` stays. Parsers exist (`BBSCodec.h`). Needs a BBS budget raise for M2a
  (4,020 / 4,050 now; ESTIMATE +40–80).
- **D2 Internal power priority — contradiction in the design.** § 0.6 and the binding
  summary above say "power meter feeds fan / WebUI / Touch"; the § 3 table says "trainer →
  power meter". Recommended: power meter → trainer (the § 0.6 decision text; a power
  meter is the rider's chosen reference), fix the § 3 table. Cadence stays CSC → power
  meter → trainer. Fixed in R1, no `src_prio` UI.
- **D3 Trainer storage.** Rev 1's "no `trn_dev`" does not hold: `begin(false)` clears the
  stored table (`BBSTrainer.h:194-198`) and `TrainerInfo` has no name, so with the mirror
  off Homewind cannot show or count the stored trainer. Recommended: `trn_dev` lite in
  namespace `homewind` (MAC + name from discovery), plus `trn_on`; both cleared by factory
  reset.
- **D4 RAM gate.** `AGENT_FEATURES.md` gate (steady ≥ 40 KB, lowest ≥ 30 KB, block
  ≥ 25 KB) vs. G7 (≥ 20 / ≥ 16 KB). The rev 1 estimate (~42 / ~36) came from the G1
  baseline with HR only; it misses the trainer link and a power meter/CSC link. The gate's
  own config (HR + CSC + power) is impossible in mirror mode. Recommended: worst case
  config = HR + trainer + power meter + Zwift + WebUI on Touch; re-baseline 1.8.2 with
  HR + power meter (no mirror) first; AGENT_FEATURES gate as target, G7 as hard minimum;
  if only G7 passes, the maintainer decides (ship default-off vs. reduce first).
  Re-baseline measured 2026-11-18 (`logs/d4_base_pm_20261118-202146.log`, HEAP_BENCHMARK):
  steady 50.6 KB (range 49.96–50.64), lowest since boot 42.4 KB (at the first WebUI
  connect), largest block 31.7 KB. ESTIMATE with the mirror (−1 KB static, −6–10 KB GATT,
  −1–2 KB trainer link): steady ~38–43, lowest ~29–34 KB — both around the target;
  the early smoke test (work package 5) decides.
- **D5 Gate and variants.** Recommended: `HW_ENABLE_TRAINER_MIRROR` default 0; build.sh
  option `--trainer` (test builds, like `--fan-power`) writes `-DBBS_COMPILE_TRAINER=1`
  into `build_opt.h`, and on Touch (`display == 1`) `-DBBS_TRAINER_PSRAM=1`;
  `static_assert(!BBS_TRAINER_PSRAM || HW_ENABLE_DISPLAY)`. Basic gets the mirror with the
  Trainer as a static object.
- **D6 Setup OK → mirror on?** Recommended: yes, setup OK sets `trn_on=1` and offers the
  restart; Release trainer sets `trn_on=0`, keeps `trn_dev`.
- **D7 Trainer status transport.** Recommended: new frame `TRAINER_STATUS` 0x0F, small
  (state, app connected, mirror on, changed, name); service/characteristic counts only in
  serial diagnostics. Alternative: 4th entry in `SENSORS_SNAPSHOT` (fits, 341/512 B) but
  every 3-sensor assumption in WebUI/Touch changes.
- **D8 Trainer already in the power slot.** Today users keep the NEO as power sensor; it
  stops advertising while connected (G2), so setup would fail NOT_FOUND, and slot +
  mirror would target the same MAC. Recommended: setup removes the power slot first if it
  holds the selected MAC (with a WebUI notice); `sensor/set` rejects the `trn_dev` MAC; at
  boot with `trn_on=1` a slot holding that MAC is not started (logged, shown).
- **Notice in mirror mode:** separate CSC sensor/power meter feed only Homewind, not Zwift
  (no local CSC/Power services, `BBSCore.h:1317`). Show in the Trainer row.

### Preconditions
- Maintainer commits the BBS M1 work and the pending Homewind changes (1.8.2 bump,
  whitespace in `BLERelayManager.cpp`, `HEAP_BENCHMARK.md` row) so M2a reviews alone.
- D1–D8 decided; D4 re-baseline measured (HR + power meter, 1.8.2, Touch). Done
  2026-11-18 (commits pushed, baseline in D4).

### Work packages (in this order; host tests where logic is testable, both variants build)
1. **Build + gate:** `Config.h` flag + `#error` checks, `build.sh` option and per-variant
   flags in `build_opt.h`, `static_assert`s (D5) in `BLERelayManager.h`.
2. **BBS (D1):** read-along 0x2A63 → `trainerPower()` + callback, trainer crank as third
   cadence source. Host tests in BBS. Maintenance/OTA: already covered — `quiesce()` stops
   the trainer and `quiesced()` waits for it (`BBSCore.h:578-580, 604-607`,
   `trainer_test.cpp:995-998`); only a hardware check that maintenance finishes in time.
3. **Device rule + power-slot conflict (D8):** pure helper (host test in
   `ble_command_state_test`), used by `applySetFromDiscovery`/`sensor/set` and setup.
4. **BLERelayManager:** load `trn_on`/`trn_dev`; `setTrainerMirror(trn_on)` before
   `begin()`; handle `TRAINER_CONNECTED/DISCONNECTED/SETUP_DONE`; setup state (refuse while
   a Homewind discovery is open; a flag marks the next `DISCOVERY_DONE` as the setup's so
   the WebUI snapshot stays); boot rule for a slot with the trainer MAC; factory reset
   calls `trainerForget()` on the loop task in `runFactoryReset` with its own failure bit.
5. **Early Touch heap smoke test:** setup via curl, mirror on, Zwift + HR + power meter,
   15 min, heap lines. Stop and report if D4 is clearly missed.
6. **Data:** source selection as a pure helper in `BikeMetrics.h` (D2), used by
   `updateBikeMetrics()`; `BIKE_METRICS` flag bit 3 "from trainer". Fan feed: trainer power
   reaches `FanHRAdapter::onPowerSample` on each new sample via the BBS callback, like the
   power meter path (`BLERelayManager.cpp:218-230`), not via the 1 s metrics poll.
   `FanHRAdapter` power-configured, Hybrid precondition (API + `app.js:188/202`), bike tile
   visibility (`app.js:171`) and `selectBikeCard` (Touch + WebUI) accept the trainer. Touch:
   only the data source changes, no HomeWindWSAmoled API change. Host tests:
   `bike_metrics_test`, `bike_card_test`, `fan_blend_test`.
7. **API + WebSocket:** `trainer/setup {mac,name}`, `trainer/enable {on}`, `trainer/forget`
   (restart via existing `system/restart`); frame 0x0F (D7) incl. `markDirty` bound
   (`WebSocketTelemetry.cpp:352-354`), `kFrameNames` (`:279`), connect/snapshot emits
   (`:205`, `:230`). Encoder, decoder, `WEBSOCKET_PROTOCOL.md`, `ENDPOINT.md`,
   `docs/05_APIS.md` together.
8. **WebUI** (`webui_src`, then `tools/build_webui.py`): Trainer row — select from a PWR
   discovery (NEO advertises CPS), "Reading trainer…", result (check codes), "Restart to
   activate"; status; notices (trainer firmware update with Homewind off; mirror mode:
   CSC/power meter not sent to Zwift; power slot removed); mirror switch (= Release
   trainer); third device row disabled per § 5. Opening the sensor setup pauses
   advertising (`sensor/server/pause`): mirror not advertised while the page is open, a
   connected app stays. English strings.
9. **Diagnostics:** diagnostic builds log at boot whether the Trainer lies in PSRAM; trainer
   counters (services, characteristics, drops) in serial `s`.
10. **Docs:** README, CHANGELOG (both projects), design § 3 (D2 fix), § 5, § 8, this file.

### Hardware tests (design § 8 M2a + gates)
Touch and Basic, NEO 2T, Zwift Apple TV: setup from the WebUI incl. NEO previously in the
power slot; ERG 100→200→100 W and a simulation ride; trainer power cycle and Zwift
restart 5× each; mirror off/on; maintenance/OTA with the trainer connected; fan Hybrid
from trainer power and from the power meter; WLAN + real HR; 60 min ride in the D4 worst
case config with heap sampling. Not with the serial `t` command during rides.

### Out of scope for M2a
M2b′ cadence composition toward the app, M3 hardening, priority UI (if D2 as
recommended), version bumps/releases (maintainer).

### M2a progress (2026-11-18, not on hardware)
Done: work packages 1–4, the API subset of 7 (`trainer/setup|enable|forget`, `GET
trainer/status`; no frame 0x0F) and the PSRAM boot line of 9 (serial-command builds).
BBS: `begin()` reads along 0x2A63 with the mirror; `trainerPower()` + callback; trainer
crank = third cadence source (so trainer cadence already reaches `BIKE_METRICS` and the
fan cadence path, flagged as not from Power; package 6 refines). Factory reset bit 2 =
`trainerForget()` failed. Open: 5, 6, frame + docs of 7, 8, counters of 9, 10.
Decided 2026-11-18 (maintainer): a released trainer (`trn_on=0`, `trn_dev` stored) keeps
counting for the device rule and its MAC stays blocked as a sensor; `forget` frees it.
BBS budget 4,100 (4,078 counted). Review fixes: (1) after an event FIFO overflow, a setup
still `running` while the trainer left SETUP ends as `link_failed`; (2) setup OK with
maintenance admission closed reports `store_failed`; (3) `sensor/set` is refused (busy)
while a trainer setup runs.

Package 5 (smoke test) PASSED 2026-11-18, Touch diagnostic build 20261118-220448-trainer
(`logs/m2a_smoke_20261118-220740.log`): setup via curl first refused by the device rule
(HR + CSC + power meter configured — correct), after removing CSC `ok` in ~15 s; restart →
mirror on, state `ready`, "Trainer runtime in PSRAM"; Zwift Apple TV on the mirror the
whole ride. Heap with HR + NEO mirror + power meter + fan + Zwift + WebUI, 16 min ride:
steady 41.1–41.6 KB (target ≥ 40), lowest since boot 35.3 KB (≥ 30), largest block
30.7 KB (≥ 25) — D4 target met (cost vs. the re-baseline: ~9 KB steady, ~7 KB lowest).
Open: after the restarts the power meter (Assioma) stayed disconnected until removed
and added again; it was already disconnected before the setup (mirror off). Not seen in
this build's log (no sensor events) — check in G7 with sensor-event logging.

Part 2 (packages 6–10) implemented 2026-11-18, not on hardware; BBS unchanged.
6: `hw::bikePowerFromTrainer()` (D2, `BikeMetrics.h`) in `updateBikeMetrics()` and the fan
feed (trainer power callback → `onPowerSample` only while no power meter is connected);
`BIKE_METRICS` bit 3 = power from the trainer; Hybrid accepts the trainer (FanHRAdapter:
`mirror`; API/WebUI: mirrored, or enabled and set up); `selectBikeCard()` takes trainer
configured/ready (Touch shows its power and name). 7: frame `TRAINER_STATUS` 0x0F
`[state][flags][setup][name]` (flags: app, mirror, enabled, set up); `trainer/setup` also
takes `generation` + `index` (the WebUI has no MAC); docs. 8: Trainer row as § 5; the
power-slot notice compares the sensor list before and after the setup (the slot removal
at setup end now also marks `SENSORS_SNAPSHOT` while the dialog has the server paused).
9: serial `s` trainer counters; sensor connected/disconnected/failed at error level in
serial-command builds. Assioma: no code change; see the report (stale address with name
fallback not matching is the most plausible cause; the new logs show it).
Part 2 independent review 2026-11-18: one blocker fixed (the WebUI offers the configured
power sensor as trainer, D8, and the trainer Add button does not count that slot), plus:
setup watch before the request, setup result reset on Release/Forget, Forget confirm,
configured sensors hidden from the trainer search, `_trainerSetupScan` cleared after an
event overflow without a running setup, and in BBS `releaseCapacity()` when a trainer or
setup link ends (host test; BBS 4,086 / 4,100). Assioma still open: a capacity wait retries
after `BBS_CAPACITY_RETRY_MS` (60 s) anyway, so it alone cannot explain the ~10 min
outage; candidates: repeated capacity failures, stale address/name fallback, or another
central holding the pedals. Check with the sensor-event logs (serial-command build).
Slot diagnostics 2026-11-18: `SensorInfo` carries `failCount`, `nameFallback`, `capWait`,
`capReason` (BBS 4,091 / 4,100); serial `s` prints one line per slot (state, MAC, address
type, fails, capacity wait, name fallback, last disconnect, name) and the BLE counters.
First reading (diagnostic build 20261118-234429-trainer, pedals idle): PWR `state=2`
(WAIT_SCAN), random address, name fallback on, no fails, no capacity wait.

Part 2 hardware test 2026-11-19 (Touch, NEO 2T, Assioma): passed. WebUI setup of a NEO in
the power slot (listed first with the power-slot hint, slot removed, notices shown);
restart → mirror on; the device rule disabled the CSC Add; D2 both ways (Assioma
asleep/disconnected → Touch shows Tacx and the fans follow trainer power; Assioma back →
Assioma wins); Release → restart → mirror off, trainer kept; Assioma reconnected 6 s after
reboot. UX follow-ups 2026-11-19 (WebUI only, API unchanged): automatic restart after a
successful setup, Release/Mirror and Forget once `TRAINER_STATUS` shows the stored change,
unless an app is connected (then the Restart button stays; frame `APP_STATUS` 0x10 counts
apps on the mirror and on the plain relay, closing the gap of the 0x0F flag, which covers only
the mirror and remains the fallback for firmware without 0x10); advertising is paused
only while a search runs, no longer while "Setup Sensors" is open (the overview re-renders
only when its content changed, since `SENSORS_SNAPSHOT` then arrives every second); rows the
device rule blocks are greyed; short trainer button labels (Restart, Release/Mirror,
Forget; "Restart Homewind to apply the change." in the row); trainer list subtitle "Select
your trainer (not a power meter)".
Review fixes 2026-11-19 (WebUI only): a 20 s search watchdog and a resume on every WebSocket
(re)connect outside a search keep advertising from staying paused; no overview re-render over
the restart or error dialogs; a Release/Forget restart unconfirmed after 10 s shows "not
saved"; no auto-restart while a setup runs.
Re-enable after Release (Mirror → restart) passed 2026-11-19: `state=ready`, mirror on,
app connected, "Trainer runtime in PSRAM".
Re-test 2026-11-19 evening (diagnostic Touch `--trainer` with icons, badges, bike card,
`APP_STATUS`; log `m2a_g7_20261119-193737`): passed — auto-restart after Release; no
auto-restart after Mirror with Zwift on the plain relay only (Restart button, manual restart);
Forget → Cancel (no change); Forget → auto-restart; new setup with advertising on (ok in
~10 s, auto-restart, mirror ready); Zwift finds Homewind with "Setup Sensors" open; live
dialog; device-rule greying; subtitle. Not checked: Homewind hidden from Zwift during a search.
G7 2026-11-19 (61 min, HR + NEO mirror + Assioma + fan + Zwift on the mirror + WebUI on one
device): passed — steady 41.2–43.3 KB (≥ 40), min since boot 30.3 KB (≥ 30, tight),
largest block 29.7 KB (≥ 25); no link loss, no restart. Idle day run: largest block fell
to 22.5 KB while the WebUI was opened from several devices; a controlled test showed WebUI
loads on 2–3 devices cost ~12 KB transiently (min since boot 42.0 → 29.7 KB). Watch before
release: WebUI multi-device fragmentation. Also seen in G7 (~21:15): opening the WebUI on the
phone (2nd client, limit `HW_WEBSOCKET_MAX_CLIENTS` 2) froze the laptop's values while its
indicator still said "Connected"; a reload fixed it. Likely a stalled WebSocket (send queue
during the phone's page load) the WebUI does not detect. Proposed: WebUI reconnects when no
frame arrives for a few seconds; reproduce with WebSocket event logging first.
Reproduction 2026-11-20 without a ride (log `wsfreeze_20261120-072240`, per-client queue
logging): laptop + phone, 8 fast phone reloads, phone locked (iOS closed cleanly), third
client rejected — no freeze, queues always 0; reloads dipped the largest block to 22.5 KB
(recovers to 31.7 KB). Fix anyway (WebUI only): no frame for 45 s (`SYSTEM_STATUS` every
30 s) drops the socket, shows "Disconnected" and reconnects.
XIAO without PSRAM 2026-11-20 (diagnostic Basic `--trainer`, log `xiao_trainer2_20261120-081903`,
`HEAP_BENCHMARK.md`): erase, setup HR + Assioma + NEO, ~20 min ride with Zwift on the mirror
passed functionally (no restart, ERG ok, dropped=6 at the Assioma connect); steady 54–56 KB
free. But WebUI page loads drained internal RAM to < 1 KB (cold load, second device) and a
laptop that stopped acking held ~25 KB until it reconnected: not release-ready until page
serving is cheaper (next task). Factory reset passed (everything erased, AP mode); its report
was gone after the WiFi setup reboot — fixed: kept for two boots. Re-test passed: after the
WiFi setup `maintenance/status` shows `factory_reset {attempted:1, failed_mask:0}`, and the reset
dialog left open on the Mac switched to "Factory Reset Complete" once the device was back.
Page-serving cost found the same day (`HEAP_BENCHMARK.md`): not the assets but
`telemetry/refresh` — with Nagle off (AsyncWebServer default) every small WS frame took its own
TCP segment, up to ~43 KB per refresh with two clients. Fixed: `setNoDelay(false)` on WS
connections (2–12 KB), plus no refresh at WebUI script start and no favicon request. Control run
with all fixes (log `xiao_fix_20261120-095221`): laptop reload + iPhone private-tab cold load with
two clients → min since boot 24.3 KB (before: 0.3–2 KB).
Trainer `dropped` 2026-11-19/20 (same log): 7 after the 20:23 restart while HR, Assioma, NEO
and Zwift reconnected; 0 during G7; 18 between 01:39 and 01:54, matching the Assioma
wake-up at 01:45 (connect, disconnect after 5 s, failed reconnect). Pattern: losses while
another sensor connects (~2 s of NEO data at ~10/s). The counter aggregates the trainer
NOTIFY queue, app writes not forwarded, indication timeout or buffer full, and failed app
notifies/indications (`BBSTrainer.h`), so the cause is not attributable. M3 diagnostic: split
`dropped` per cause (BBS contract + budget, 4,091 / 4,100). Likely mechanism (code, not yet
measured): a sensor's GATT pass (`BBSCore::gattStep` → `gattPass`, blocking NimBLE calls, budget
`BBS_GATT_BUDGET_MS` 8 s) runs on the `update()` task that drains the trainer queue; NOTIFYs may use
40 of 48 slots, so a pass longer than ~2 s at the NEO's rate drops them. Confirmed again on the XIAO
(dropped=6 at the Assioma connect). M3 options: time the pass first; then a non-blocking pass or
relaying trainer notifications without `update()`.
Later feature (maintainer, 2026-11-19): trainer detection in the setup search. The BBS scan
decoder only knows 0x180D/0x1816/0x1818, so a power meter and a trainer look alike. Plan:
BBS reports "likely trainer" per scan result when ADV or SCAN_RSP lists FTMS 0x1826, the
Tacx FE-C UUID or a Wahoo UUID; the WebUI lists those first with a trainer icon and the
rest greyed ("probably a power meter") but still selectable — no hard filter (the KICKR
2017 has no FTMS, only 0x1818 + Wahoo services). Needs a BBS→Homewind contract field and
a small BBS budget raise (4,091 / 4,100). Bundle with the WebUI layout pass (new icons).

## Report format
What was done, logs and numbers, open points, proposed next step. No release or
version bump without the maintainer.
