# Trainer mirror — design note, gate, plan (rev 8)

Date 2026-11-15. Base: Homewind 1.8.2 (workspace), BluetoothBikeSensorServer (BBS) 3.2.2,
NimBLE-Arduino 2.5.2+hw1, HomeWindWSAmoled 1.6.1. Trainer: Tacx NEO 2T (maintainer,
local; GATT dump `neo2t_nrfconnect.txt`). Wahoo KICKR 2017: acquaintance, remote only.

Rev 6 replaced the FTMS translation design (rev 3–5, archived as
`TRAINER_PROXY_DESIGN_rev5.md`) with the maintainer's concept: **mirror the trainer's
GATT and relay everything unchanged.** Rev 7 worked in the two independent rev 6 reviews
(disposition in Appendix A). Rev 8 adopts three further dispositions from the
2026-11-15 independent review (Opus) after the G5 prototype: BLE address from table
hash is mandatory; CCCD reference counting for app + internal read-along; power-meter
values are internal by default until G5 `cps` proves Zwift reads CPS (Appendix B).

Tags: CONFIRMED (code `file:line` or official source), REPORTED, UNKNOWN, DECIDED
(maintainer decision), ESTIMATE.

---

## 0. Summary

1. **Goal.** Zwift (Apple TV, iPad) sees one Bluetooth device "homewind-…" with trainer
   control (ERG, simulation; virtual shifting if the trainer offers it), power, cadence,
   speed and heart rate, and uses one Bluetooth slot. Fan (Hybrid), WebUI and Touch get
   the trainer's power (and, if configured, the power meter's).
2. **Mirror everything, unchanged (DECIDED)** — all services of the trainer, including
   Zwift's protocol FC82 ("Zwift Ready"), FE-C over BLE, CP Control Point, DIS and private
   services. **Exception: DFU is not mirrored (DECIDED)**; trainer firmware updates are done
   with Homewind switched off (UI: "release trainer").
3. No protocol knowledge, no filters, no Homewind-side limits. Trainer firmware and app own
   control and safety.
4. **Transparent link behaviour (DECIDED):** Homewind advertises the mirror only while the
   trainer is connected, and disconnects the app when the trainer drops — the same as a
   direct connection.
5. **Read-along:** Homewind decodes the trainer's CPS and CSC measurements for fan, WebUI
   and Touch. Subscriptions to the trainer stay up while Homewind needs them, independent
   of whether an app is connected (CCCD reference count, § 2.2.3).
6. **Priorities (DECIDED, rev 8):** cadence CSC sensor → power meter → trainer; speed only
   from the trainer; HR only from the strap. **Power toward the app:** keep the mirrored
   trainer CPS/FE-C/FC82 byte-identical by default. The power meter feeds fan / WebUI /
   Touch. Rewriting mirrored CPS for the app (and thus M2b CPS encoder work) is built only
   if G5 `cps` shows Zwift takes power from CPS rather than FE-C/FC82.
7. **Gate first.** G5 (does Zwift accept the mirror under Homewind's own name, with the
   real NEO 2T behind a throw-away relay sketch) is the go/no-go. If it fails, stop and
   re-decide; no prepared fallback.

---

## 1. Facts

| # | Fact | Tag |
| --- | --- | --- |
| F1 | NEO 2T services: CSC 0x1816, CPS 0x1818 (CP Vector, CP Control Point), Tacx FE-C over BLE `6E40FEC1` (FEC2, FEC3), Secure DFU, DIS, `CF285001…`, `FE03A000…`, FC82 (Zwift protocol), `FA1C030E…`; no FTMS; 9 services, 26 characteristics, 12 CCCDs. The dump has no properties, values, MTU or advertising data | CONFIRMED (`neo2t_nrfconnect.txt:6-50`) |
| F2 | NEO 2T is "Zwift Ready" (virtual shifting via FC82) since a Tacx firmware of June 2025; Zwift may control it via FC82 rather than FE-C | CONFIRMED for iPad and Apple TV: control and power via FC82 only (G5 2026-11-15) |
| F3 | NEO 2T accepts one or more BLE centrals | UNKNOWN → G2 |
| F4 | Zwift chooses the control path from the GATT, not from the advertised name | UNKNOWN → G5 (core question) |
| F5 | NimBLE-Arduino acknowledges a write synchronously in the server access callback; no ATT error after `onWrite` | CONFIRMED (`NimBLEServer.cpp:749-771`) |
| F6 | The server side cannot see whether the app sent Write Request or Write Command | CONFIRMED (`ble_gatt.h:1104-1144`, `NimBLEServer.cpp:749-771`) |
| F7 | Reads are answered synchronously from the attribute value | CONFIRMED (`NimBLECharacteristic.h:295`, `NimBLEServer.cpp:723`) |
| F8 | One unconfirmed indication per connection; a second one is caught only by a debug assert; unconfirmed indications end the link after 30 s | CONFIRMED (`ble_gattc.c:4931`, `:83`, `:1306`) |
| F9 | NimBLE-Arduino client calls `writeValue`, `subscribe`, `readValue` block until done; `writeValue` silently turns an over-long write without response into a long write | CONFIRMED (Opus review, NimBLE-Arduino client source) |
| F10 | GATT client procedures: 4 for all links together | CONFIRMED (`nimconfig.h:312`) |
| F11 | A characteristic's maximum length cannot be discovered; notifications longer than the app's MTU−3 are truncated silently | CONFIRMED (`ble_att_cmd.c:89`) |
| F12 | No fixed attribute limit; the server can be built from a table before `start()`; the GATT cannot change while any link exists | CONFIRMED (`ble_gatts.c:3235-3251`) |
| F13 | BBS owns server and advertising (`BBSNimBLEPort.h:184`, `:233-251`), creates its own CPS/CSC, keeps advertising with a central connected (`BBSCore.h:1233-1235`) | CONFIRMED |
| F14 | BBS has CPS/CSC parsers and `stripPowerCrank`, no CPS/CSC encoder, no wheel decoder; `PowerSample` has no crank counters | CONFIRMED (`BBSCodec.h`, both reviews) |
| F15 | `CONFIG_BT_NIMBLE_MAX_CONNECTIONS` = 4 | CONFIRMED (`Homewind/sdkconfig.h:6-7`) |
| F16 | BBS `src/` 2,591 counted lines of a 2,600 budget | REPORTED (rev 5/6 reviews) |
| F17 | Fan-mode NVS blob is fixed 8 B; new settings need their own key | CONFIRMED (`FanModeBlob.h:28-39`) |
| F18 | Touch baseline (1.7.1+): free internal 47.8–52.8 KB, lowest 38.9 KB, largest block 31.7 KB | CONFIRMED (`HEAP_BENCHMARK.md`) |
| F19 | Apple caches GATT per peripheral address; Service Changed after reboot without bonds does not reach a recipient | CONFIRMED (rev 6 reviews; Nordic / Apple behaviour) → address from hash |

### Accepted consequences
- **Writes** are acknowledged by Homewind at once (F5) and forwarded. Write type upstream
  follows the trainer's properties (F6): Write Request if the trainer's characteristic
  allows it, otherwise Write Command. ATT errors from the trainer cannot reach the app;
  the result of a command travels in the trainer's own protocol (FE-C page 71, FC82 and
  CP Control Point indications), which is relayed back. Upstream errors are logged;
  ENOMEM / procedure-pool failures retry — control writes must not be dropped silently.
- **Reads** are served from a cache refreshed on every trainer connect (DIS and other
  readable values). Whether Zwift reads a characteristic whose value changes without
  notification is UNKNOWN → G3/G5 logs.

---

## 2. Architecture

### 2.1 Placement
Module inside BBS (DECIDED by both reviews' recommendation, maintainer confirmed
2026-11-16, Q1),
own files, behind `BBS_COMPILE_TRAINER=1`. Cadence priority / crank stripping live in BBS
as today. Homewind keeps the WebUI, NVS settings and the fan. ESTIMATE: +800–1,200 BBS
lines → budget decision. CPS/CSC *app* composition (M2b) is optional until G5 `cps`.

### 2.2 Components
1. **Discovery (setup).** Connect to the trainer, discover all services, characteristics,
   descriptors; read every readable characteristic once; record MTU; build the table;
   store; reboot. Non-blocking `ble_gattc_*` calls only (F9).
2. **Mirror build (boot).** In BBS `serverStart()`, before `server->start()`: create the
   mirrored services from the table (same UUIDs, properties); skip GAP 0x1800, GATT 0x1801
   and DFU (Nordic Secure DFU 0xFE59 and legacy DFU UUIDs). In trainer mode BBS does not
   create its own CPS/CSC; the HR service stays.
3. **Relay (runtime).** One ordered FIFO per trainer link for everything app → trainer:
   CCCD changes and writes in arrival order, so a subscription always reaches the trainer
   before the next control write (Opus C2). Executed with non-blocking `ble_gattc_*` from
   the loop task; at most 2 of the 4 GATT procedures (F10) in use by the relay; retry on
   ENOMEM. Trainer → app: notifications forwarded at once; indications FIFO of 8 per app
   link, one outstanding (F8); overflow → log and disconnect the app (transparent failure).
   Characteristics are mapped by UUID (+ instance index), handles resolved after every
   discovery.
   **CCCD reference count (DECIDED, rev 8):** each mirrored characteristic that Homewind
   also read-alongs has holders `{app, internal}`. Upstream subscribe while any holder is
   set; unsubscribe only when both clear. The G5 throw-away sketch unsubscribes when the
   app drops — that must **not** ship. Fan / WebUI / Touch keep receiving trainer CPS/CSC
   with no app connected.
4. **Lengths and MTU.** Attribute value cap 64 B (ESTIMATE; covers FE-C 13 B and the
   reported FC82 message sizes); request MTU 247 upstream, offer 247 to the app; log both.
   A message larger than the app's MTU−3 is logged as an error (F11). G5 records actual
   sizes; cap adjusted from the log.
5. **Read-along and composition** (section 3).
6. **Link coupling.** Advertising only while the trainer is connected and the mirror is
   built. Trainer disconnect → disconnect the app at once. App connected → stop
   advertising (no second app). One link stays reserved for the trainer.

### 2.3 Table, storage, change detection, address
- Per characteristic: UUID, instance index, properties, cached value (≤ 64 B), CCCD flag.
  NEO 2T ESTIMATE 1.0–1.5 KB. NVS blob `trn_gatt`: version, length, CRC32, structure hash
  (UUIDs + properties).
- On each trainer reconnect: rediscover, compare hash. Mismatch (e.g. trainer firmware
  update) → mirror not advertised, WebUI notice "Trainer changed, set up again".
- **BLE address from table hash (DECIDED, rev 8):** when the mirror GATT is active, the
  advertised static random address is derived from the structure hash (both MSBs set).
  Hub mode (no trainer mirror) keeps the existing Homewind identity. Hub → mirror and
  trainer-GATT change therefore present a new address to Apple, so the GATT cache cannot
  serve a stale database. Service Changed is not used. Users still remove the old device
  in Zwift / Bluetooth settings after a mode change; the address change makes that
  sufficient. M1 delivers this — not a post-release fallback.

### 2.4 Advertising (31 B)
Flags 3 B + 16-bit list HR 0x180D, CPS 0x1818, CSC 0x1816, FC82 (10 B) + 128-bit FE-C
`6E40FEC1` (18 B) = 31 B. Name in the scan response. G5 compares with the trainer's own
advertising (captured raw) and copies what fits.

---

## 3. Source priorities

| Measurement | Toward app (default) | Internal (fan / WebUI / Touch) |
| --- | --- | --- |
| Power | trainer mirror unchanged (CPS + FE-C + FC82) | trainer → power meter |
| Cadence | trainer CSC crank unless a higher cadence source is active (then compose) | CSC sensor → power meter → trainer |
| Speed | trainer (fixed) | trainer |
| Heart rate | HR strap (fixed) | HR strap |

- Only configured devices take part (device rule, section 4). Active *internal* source =
  first in order that is connected and delivered within 3 s (`hw::kBikeStaleMs`).
- **App power (DECIDED, rev 8):** do not rewrite mirrored CPS for a power meter unless
  G5 test `cps` proves Zwift displays the replaced CPS watts. Until then FE-C/FC82 and
  CPS stay trainer truth toward the app; a power meter is still useful for Hybrid fan and
  the WebUI when the trainer is the control path.
- If G5 `cps` shows Zwift uses CPS: enable optional "power meter first" that encodes CPS
  to match the mirrored CP Feature bits; document that FE-C/FC82 may still show trainer
  watts (conflicting tiles possible).
- Cadence: CSC wheel from the trainer; crank from the active cadence source; compose when
  needed. Counter continuity: per-source offsets for revolutions and event times after
  the first switch (packets then no longer byte-identical).
- FE-C, FC82 and other trainer services are always relayed unchanged.

---

## 4. Connections and RAM
- **Device rule (DECIDED, permanent):** besides the HR strap at most **two** devices:
  trainer + power meter, trainer + CSC sensor, power meter + CSC sensor, or one of them.
  Never trainer + power meter + CSC sensor. Rationale: power meters report cadence as
  crank data in CPS (pedals, crank/spider meters; old hub meters are the rare exception),
  so a third device adds nothing for almost all riders. Links stay at 4 (F15:
  HR + 2 devices + app); no 5-link work, no `MAX_CONNECTIONS` change.
- RAM ESTIMATE (Touch): 11–17 KB internal for the mirrored attribute table, values,
  queues and the extra link. G7 uses absolute limits (section 6).

---

## 5. WebUI and display (R1)
- Sensor setup: row **Trainer** — search, select, "Reading trainer…", "Restart to
  activate", reboot. Shows manufacturer/model (DIS) and the control services found
  (informational). Notices: "Update the trainer's firmware with Homewind switched off";
  button **Release trainer** (stop mirror / disconnect so Tacx/Garmin apps can update).
- Priorities: cadence list (up/down, Auto); power list for *internal* use; speed and HR
  fixed. If G5 unlocks app CPS rewrite, expose "power toward Zwift" separately and warn
  about FE-C/FC82 mismatch.
- Device rule in the UI: with two devices besides HR configured, the third row is
  disabled with the hint "Besides the heart-rate strap, at most two devices. A power
  meter also provides cadence." The API rejects a third device (`sensor/set`).
- Status: trainer connected, app connected, mirror active, GATT-changed notice.
- NVS: priorities in key `src_prio` (F17).
- NVS devices: the v3 sensor blob holds exactly three entries (HR, CSC, PWR). With
  trainer + power meter a fourth device type is needed, so the trainer cannot live in
  the power slot. Store the trainer in its own key (`trn_dev`: address, type, name,
  table hash) next to `trn_gatt`; the v3 blob stays unchanged, so a downgrade keeps HR,
  CSC and power meter. M2a task.
- Touch display: unchanged.

---

## 6. Gate

Order: **G0 → G1 → G2 → G4 → G3 → G5 → G7.**

| ID | Check | Method | Pass |
| --- | --- | --- | --- |
| G0 | Cheap pre-check of advertising/pairing | `tools/trainer_face_test` variant T1 (loopback, no trainer) on iPad and Apple TV | result recorded (informational) |
| G1 | Baseline 1.8.1+ Touch: heap, loop period, GATT pass duration | `HEAP_BENCHMARK.md` method | numbers recorded |
| G2 | NEO 2T accepts two centrals? | nRF Connect while Zwift is connected to the NEO | recorded |
| G4 | Full NEO 2T GATT with properties and values; raw advertising | nRF Connect (Mac/iPhone): read all, export complete log | table complete |
| G3 | Reference: Zwift controls the NEO 2T directly | iOS Bluetooth logging profile on iPad + PacketLogger (Mac): pairing, ERG, grade, reconnect | which services Zwift uses (FE-C vs FC82), message sizes, MTU, reads |
| **G5** | **Zwift accepts the mirror** | relay sketch `tools/trainer_mirror_relay` v0.3 (section 7) on a spare ESP32-S3, NEO 2T behind it, `homewind-test`; Zwift on Apple TV and iPad; serial log via `tools/serial_log.py` | pairs as controllable trainer + power + cadence + HR from one device; ERG and simulation follow; trainer power cycle and Zwift restart 5× each recover; relay delay per direction median ≤ 30 ms; `cps` result recorded (drives M2b) |
| G7 | Heap with mirror on Touch (interim check; final check = release check, section 8) | after M2a, 60 min ride, `HEAP_BENCHMARK.md` method | lowest free internal ≥ 20 KB, largest block ≥ 16 KB, no downward trend (proposal) |

**G5 results (2026-11-14, `trainer_mirror_relay` v0.3, NEO 2T, iPad, no serial file):**
Zwift pairs `homewind-test` as Power Source, Resistance, Cadence and Heart Rate from one
device (Zwift-Ready icon; mirror copies the trainer's FC82 service data and appearance).
ERG works through the mirror.

**G5 results (2026-11-15, v0.3, NEO 2T FW 2.7.0, iPad, log `logs/g5_ipad_20261115-115718_v3.log`):**
- Table must be complete: a discovery that stored 8 services / 11 characteristics with
  FC82 empty left Zwift at "Connecting…" with 0 W and cadence only. Re-read with
  25 characteristics (FC82 `…0002` notify, `…0003` write, `…0004` indicate) → pairs.
- Control path = FC82 only. Zwift sends `RideOn` to `…0003`, then protobuf commands:
  ERG 100 → 200 → 100 W (`3F 08 01 10 nn 22 02 10 64` / `… 10 C8 01`), simulation
  parameters after the workout. No write to FE-C `FEC3` or CPS control point.
- `cps`: mirror sent constant 200 W on CPS 0x2A63 (712 notifications) while FC82
  `…0002` carried the real power (0–411 W); Zwift displayed the real power. Zwift takes
  power from FC82, not CPS → **M2b stays locked**; CPS rewrite would not reach Zwift.
- Stable: drop 0, internal heap free ~225 KB / largest 184 KB, ~2000 notifications in
  the ERG run.
- Message size (found in the M1 final review, 2026-11-17): the FC82 `…0004` device-info
  indication (`3C 08 00 12 41 …`) is ~70 B; the prototype's 64 B cap truncated it (48×)
  and Zwift re-requested it (`00 08 00`) 3× per connect, then went on. M1 cap → 104 B
  (trainer MTU 107 − 3).
- NEO ran without mains power until 12:28 (plug not in; self-powered by pedalling).
  After pedalling stopped (12:04:15) it switched off ~3 min later (12:07:28) and came
  back when pedalled (12:09:03); while powering up it dropped the link 1–2× within
  ~30 s (12:07:32–38, 12:09:03–11, 12:20:57–12:21:47). Mirror reconnected within ~5 s
  each time and Zwift re-paired by itself. Not a mirror fault; WebUI should show
  "trainer off", not an error.
- Trainer power cycle 5× on Apple TV (12:28:14, then log
  `logs/g5_reconnect_20261115-201048.log` 20:16:21, 20:18:30, 20:21:25, 20:22:53):
  each time trainer lost → mirror reconnects the trainer within 0.4 s (12:28: two
  connects) → Apple TV re-paired by itself after 11 s, then 5 s × 4, `RideOn` follows.
  The NEO stays up for a while when unplugged; the link drops when mains returns.
- Zwift restart 5× on Apple TV (20:13:08, then quit 20:24:09 / 20:25:19 / 20:26:04 /
  20:26:53 with reason 0x13): Zwift re-pairs `homewind-test` without re-selection each
  time (20:24:42, 20:25:40, 20:26:24, 20:27:23), `RideOn` follows.
- Reconnect log 20:10–20:27: drop 0, no NTF/IND/WR failures, heap free 226 240 →
  225 660 B, largest block 184 308 B unchanged.
- Apple TV (same log, 12:21:51–12:26): pairs `homewind-test`; ERG 100 → 200 → 100 W
  (12:23:15 / 12:24:07 / 12:24:52) and simulation after the workout, all via FC82;
  rider confirms ERG, correct watts and grade feel; drop 0, heap unchanged.

- Relay delay (ESP-internal: BLE callback → notify / write call; air time not included):
  trainer → app n = 12 696, median 0 ms, p99 0 ms; app → trainer n = 143, median 0 ms,
  p95 1 ms. All 18 values > 30 ms (max 253 ms) fall in the first ~3 s after an app
  connect, while `loop` blocks on the upstream `subscribe` calls (F9); the first
  `RideOn` write waits behind them. Criterion "median ≤ 30 ms" met.

- Apple TV, Assioma as power source, `homewind-test` as controllable (20:32–20:36, same
  log): Zwift power-matches via FC82, a new ERG target every 0.2–1.5 s (206 writes in
  3 min): 100 W phase 87–103 W, 200 W phase 182–197 W; NEO's own power 82–114 /
  179–208 W. All writes relayed (down +206), drop 0. Assioma pairs as second device next
  to the mirror on Apple TV. Rider: Zwift shows ~100 / 200 W after a few seconds; ERG
  feels less immediate than with the trainer as power source (Zwift's outer power-match
  loop; mirror adds ~0 ms). Supports the default "trainer power unchanged to the app".

- 60 min ride (2026-11-16, Apple TV, `logs/g5_60min_20261116-204437.log`; real HR strap
  paired directly to Apple TV, `homewind-test` for power, resistance, cadence): one app
  connection 20:49:03–21:47 (~58 min, ride 20:52–21:46) without disconnect or trainer
  loss; 31 871 notifications, drop 0, no NTF/IND/WR failures, no reset. Heap internal free
  225 412–225 532 B, largest block 184 308 B constant — no downward trend. (The app
  disconnect 20:48:40, reason 0x13, was Zwift during pairing before the ride.)

G5 criteria met on iPad and Apple TV except WLAN load, which needs the mirror inside
the Homewind firmware (M2a, with G7). Not run from the agent checklist: third device
(Zwift Click) on Apple TV. **Maintainer confirmed the G5 results on 2026-11-16.**

**Remaining gates (maintainer decision 2026-11-16):**
- G3, G4: covered by G5. Control path = FC82 only on iPad and Apple TV; full NEO table
  with properties, values and MTU in `logs/g5_ipad_20261115-115718_v3.log` (`use 0`),
  raw advertising in its scan lines. No PacketLogger capture planned.
- G2: short nRF Connect check, not blocking M1 — does the NEO stay visible/connectable
  while the mirror holds it (risk: two entries in Zwift's pairing list)? Result → UI /
  manual text. **Passed 2026-11-17** (M1 bench below): not listed in Zwift.
- G1: measure the Touch baseline before M2a (needed for G7), not before M1.
  **Measured 2026-11-18** (1.8.2 diagnostic build, HR strap, Zwift Apple TV, WebUI,
  28 min; `logs/g1_touch_ride_20261118-071132.log`): free internal 50.1–50.5 KB, largest
  block 31.7 KB, lowest since boot 44.5 KB, DMA lowest 36.7 KB; lowest stack reserves
  IDLE0 236 B, LVGL 876 B. Loop period and GATT pass duration are not instrumented in
  Homewind — not measured. With M1's +19.9 KB static RAM the lowest would be ~24.6 KB:
  above G7's 20 KB but tight, before the second link's runtime heap. At 28 min the
  diagnostic serial `t` command (task list) stopped output mid-list and the loop task
  (display/WebUI values) froze while HTTP still answered; diagnostic builds only.
  **Decision 2026-11-18:** `BBS_TRAINER_PSRAM=1` in the Touch build (behind the display
  gate) puts the ~19.5 KB Trainer in PSRAM; expected Touch lowest ~36 KB with the mirror's
  runtime GATT (estimate), confirmed by G7.
- M1 starts in a fresh agent session with `docs/AGENT_TRAINER_MIRROR.md` as handover.

**M1 bench (2026-11-17, BBS `examples/Trainer` on XIAO, NEO 2T, Zwift Apple TV, real HR
strap; logs `logs/m1_bench_*.log`):**
- Setup found a defect: `Trainer::update()` cleared `seen_` every loop, also during the
  setup scan, so setup ended NOT_FOUND. Fixed (kept while capturing) with a host test
  that fails on the old code. After the fix: 8 services / 25 characteristics, hash
  `685cb09e`, trainer connected 5 s after the reboot.
- ERG workout, power and cadence in Zwift: OK (rider). up 2,859 / down 131, dropped,
  write errors and oversize 0.
- Trainer power cycle ×3: the NEO bridged unplug/replug while pedalled, no link loss.
- Forced trainer-side drop (unplugged, crank still, NEO off after ~6–8 min, then plugged
  in): 3 drops (supervision timeout, reason 520) between 20:18 and 20:20, each with the
  app dropped by link coupling (534); trainer back after 20/20/57 s, Zwift re-paired
  0.2–0.6 s later; then stable, power and cadence OK.
- G2: while the mirror holds the NEO, Zwift (Apple TV) does not list the NEO — no double
  entry.
- Relay reset ×5 (esptool): trainer connected 7.2–7.9 s and Zwift re-paired 7.5–9.3 s
  after boot, every time without user action.
- Zwift restart ×5: clean disconnect (reason 531), re-paired after 25–46 s including the
  app start; trainer link unaffected; no errors.
- Second defect: trainer-side MTU stayed 23. The NEO starts the MTU exchange itself
  (107); M1's `ble_gattc_exchange_mtu` then returned `BLE_HS_EALREADY` and the phase was
  skipped, so app writes > 20 B would have been refused. Fixed in `BBSTrainerNimBLE.h`
  (EALREADY → MTU event with `ble_att_mtu()`); on hardware mtu = 107/247. ERG ride after
  the fix: 32 app writes, write errors / dropped / oversize 0.

G5 decision: pass → plan. Control missing → compare with G3 (advertising, reads, FC82
handshake), fix the sketch, retest once; still failing → stop and re-decide. Works only
with a vendor name → not shippable; stop and re-decide.

---

## 7. G5 relay sketch (requirements)
- Standalone, NimBLE-Arduino 2.x, ESP32-S3, no Homewind code; non-blocking `ble_gattc_*`
  preferred (v0.3 may still block — note in log).
- `scan`, `use N`: discover the trainer's full GATT, store, reboot; boot builds the mirror
  (all services except DFU) + an HR service (simulated or relayed strap), advertises as
  `homewind-test` (own static random address) only while the trainer is connected.
- Relay per 2.2.3 (one ordered FIFO incl. CCCD, UUID mapping, indication pacing, write
  type from trainer properties); link coupling per 2.2.6. CCCD refcount is **product**
  only — the sketch may unsubscribe on app drop.
- Log with `millis()` and host timestamps (`tools/serial_log.py`): direction, UUID, hex,
  size; MTU both sides; raw advertising of the trainer; upstream write results;
  indication confirmations; free heap every 10 s; `+Nms` relay delay.
- Command to switch CPS between relay and a replaced value (`cps`) — decides M2b.
- Delay per direction (upstream notify → downstream notify; downstream write → upstream
  write).

---

## 8. Plan

| M | Content | Tests |
| --- | --- | --- |
| M0 | Gate G0–G5 (incl. logged iPad + Apple TV) — **passed 2026-11-16** (G3/G4 via G5; G2 open, non-blocking; G1 before M2a) | gate table; `cps` decision; go/no-go |
| M1 | BBS module: table codec + NVS + hash; **address from hash**; mirror build in `serverStart()`; relay FIFO, indication queue, **CCCD refcount**, link coupling, DFU skip, ENOMEM retry | host: table encode/decode, CRC, hash, UUID mapping, FIFO order (CCCD before write), indication pacing and overflow, length cap, refcount subscribe/unsubscribe, address derivation |
| M2a | Homewind: Trainer row, setup with reboot, status, notices, **Release trainer**; read-along to fan Hybrid, `BIKE_METRICS`, Touch | NEO 2T: 60 min ERG + simulation ride, trainer power cycle, Zwift restart; heap G7; WLAN + real HR |
| M2b | **DROPPED (G5 `cps` 2026-11-15: Zwift takes power from FC82).** Was: CPS encoder toward app, composition, counter offsets, WebUI "power toward Zwift" | host + NEO 2T + power meter; document FE-C/FC82 mismatch |
| M2b′ | Cadence composition (CSC/PM crank into mirrored CSC) without CPS rewrite | host + hardware drop-out/return |
| M3 | Hardening: GATT change detection, second-app attempt, link reservation, hub↔mirror address | hardware: table mismatch, second app, Touch heap 60 min |
| M4 | Remote KICKR 2017 (acquaintance): setup + 30 min ERG, serial log | reviewed; "tested by a third party" |

R1 = M1–M3 on NEO 2T (without M2b); KICKR listed after M4.

**Release check (DECIDED):** RAM saving is not part of this plan for now. Before the
final R1 release, measure on the release builds of Basic and Touch, 60 min ride with
trainer, HR and Zwift: free internal heap (min/avg), largest free block, PSRAM use, task
stack high-water marks (serial `t`). Thresholds as G7. Only if they fail, RAM tuning
(`AGENT_RAM_TUNING.md` C/D) is decided. R1 ships a **tested-trainer list** (start: NEO 2T),
not "any trainer".

---

## 9. Out of scope / documented limits
- Translation (FTMS etc.), Homewind-side limits, control logic.
- DFU through the mirror (excluded; update the trainer with Homewind off).
- ATT errors from the trainer are not visible to the app (F5).
- DIRCON: separate feature, not combined in R1.
- Product/legal: FC82 / "Zwift Ready" branding — clarify before commercial claims.

## 10. Open questions
1. ~~Confirm: mirror as a BBS module with budget raise (~3,400–3,800 lines)?~~
   **DECIDED 2026-11-16:** BBS module behind `BBS_COMPILE_TRAINER=1`, budget raise
   accepted; M2b dropped by G5 `cps`, so expect the lower end. **2026-11-17:** M1 came in
   at ~4,000 counted lines (2,591 before); budget set to 4,000, no guard macro; M2a is
   budgeted separately. After the bench fixes 4,004; budget raised to 4,050.
2. G7 thresholds (lowest ≥ 20 KB, block ≥ 16 KB) acceptable?

---

## Appendix A — rev 6 review disposition

| Item (reviewer) | Decision |
| --- | --- |
| Write type invisible (both) | adopted: type from trainer properties (1, F6) |
| CCCD/write ordering race (Opus) | adopted: one ordered FIFO (2.2.3) |
| Blocking client calls (Opus) | adopted: `ble_gattc_*` only (F9, 2.2) |
| MTU, max length, truncation (both) | adopted: cap 64 B, MTU 247, logging, G3/G5 sizes (2.2.4) |
| Trainer down / setup writes lost (both) | adopted as DECIDED link coupling (2.2.6) |
| FC82 relevant (Fable) | adopted: mirrored like everything (DECIDED), advertised (2.4), G3 |
| DFU through mirror (Opus) | DECIDED: not mirrored |
| No BBS encoder; counter rebasing (both) | adopted for cadence path (F14, section 3); CPS app rewrite gated by G5 `cps` (rev 8) |
| BBS skips own CPS/CSC; build in `serverStart()` (Opus) | adopted (2.2.2) |
| Map by UUID, not stored handle (Opus) | adopted (2.2.3, 2.3) |
| Procedure pool 4 (Opus) | adopted (F10, 2.2.3) |
| Indication queue size/overflow (Opus) | adopted: 8, overflow disconnects (2.2.3) |
| Service Changed useless (both) | adopted: not used; **address from hash mandatory in rev 8** (was note-only in rev 7) |
| G7 absolute threshold (both) | adopted (6), values proposed |
| Gate order G3 before G5, G0 first (both) | adopted (6) |
| Priority/composition into BBS (Opus) | adopted (2.1) |
| Split M2 (Fable) | adopted (M2a/M2b); M2b gated (rev 8) |
| Budget +800–1,200 (both) | adopted (2.1) |
| F7 debug assert, 12 CCCDs, dump w/o properties (both) | corrected (F1, F8) |
| Stale-read claim unsupported (Opus) | corrected: UNKNOWN (1); refresh on connect (rev 8) |

---

## Appendix B — rev 8 disposition (2026-11-15 independent review)

Source: independent Opus review of rev 7 + G5 prototype v0.3; maintainer accepted.

| Item | Decision |
| --- | --- |
| Apple GATT cache / hub→mirror | **DECIDED:** BLE static random address derived from GATT structure hash whenever the mirror is active (2.3). No longer a fallback note. |
| CCCD vs internal read-along | **DECIDED:** reference count `{app, internal}`; product must not unsubscribe when the app alone drops (2.2.3). |
| Power trainer → PM toward Zwift | **DECIDED:** default = trainer bytes unchanged to the app; PM for fan/WebUI/Touch. App CPS rewrite only if G5 `cps` proves Zwift uses CPS (0.6, 3, M2b). |
| Apple TV / WLAN+4 links / reconnect | stay on M0 G5 checklist; not design changes |
| FC82 / Zwift Ready branding | document before commercial release (9); no code change |
| Tested-trainer list for R1 | adopted (8) |
