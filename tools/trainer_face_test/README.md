# trainer_face_test v0.2 (gate G5)

Tests whether Zwift accepts an ESP32 as a **controllable trainer**, and which
face works under which name. No trainer needed: a loopback model turns ERG
targets and grades into simulated power (4 Hz) and 85 rpm cadence.

| Variant | Services | Name | Purpose |
| --- | --- | --- | --- |
| F | FTMS 0x1826 (R1 layout: power target + simulation only) + Cycling Power + Heart Rate | `homewind-test` | planned release-1 face |
| P1 | Cycling Power with Wahoo control point `a026e005` + Heart Rate | `homewind-test` | pass-through face, own name |
| P2 | like P1, DIS manufacturer "Wahoo Fitness" | `KICKR TEST` | comparison only, never ship |
| T1 | Tacx FE-C over BLE `6E40FEC1` (FEC2 notify, FEC3 write) + Cycling Power + Heart Rate | `homewind-test` | pass-through face for Tacx, own name |
| T2 | like T1, DIS manufacturer "Tacx" | `Tacx Neo TEST` | comparison only, never ship |

## Build and flash

Any ESP32-S3 (XIAO ESP32-S3 tested for compile), NimBLE-Arduino 2.5.2+hw
(workspace copy), core 3.3.12:

```
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3 \
  --library <path>/NimBLE-Arduino tools/trainer_face_test
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32S3 -p <port> tools/trainer_face_test
```

Every variant uses its own static random BLE address, so Apple TV / iPad /
Zwift caches from one run cannot leak into the next. Still: before each
variant, remove ("forget") the previous test device in Zwift and in the
Bluetooth settings of the iPad.

Serial monitor 115200. Commands: `f`, `p`, `k`, `t`, `n` select F / P1 / P2 / T1 / T2 (stored,
reboots), `s` prints status, `d` cycles a response delay (0 / 2 / 6 s), `x`
drops responses, `l` sends FTMS status 0xFF (control lost) — use these after
the basic run to see how Zwift reacts to slow, missing or revoked control.
Responses are sent as indications only to subscribed connections, one at a
time per connection (next after the confirmation or the 30 s timeout); the
log shows `RSP-FAIL`, `RSP-NOSUB`, `RSP-DROP` and `IND-STATUS` honestly.
A second app may connect; FTMS control requests from it get 0x05. Every write from the app is logged as hex
(`FTMS-CP`, `WAHOO-CP`, `FEC-TX`) with the response sent (`FTMS-RSP`, `WAHOO-RSP`).
For FE-C the log also says whether the frame checksum matches XOR or sum, and
decodes pages 49 (ERG), 51 (grade), 50, 55 and 70.
Save the serial log of every run.

## Procedure (per variant, on Apple TV and on iPad)

1. Real trainer off or out of range; Homewind off. Only the test ESP32 powered.
2. Zwift → Pair devices. Note under which tiles the device appears:
   Power, Cadence, **Controllable**.
3. Pair it as Controllable (and Power). Start a free ride: change of grade
   should log `SIM` / `0x46`; power in Zwift should follow the model.
4. Start a workout in ERG: log should show target power (`0x05` / `0x42`);
   Zwift power should ramp to the target.
5. Zwift reconnect: quit and restart Zwift once; disconnect/reconnect 5×.
6. Note the Apple TV Bluetooth slot count (how many other devices still pair).

## Result table (fill in)

| Check | F ATV | F iPad | P1 ATV | P1 iPad | P2 ATV | P2 iPad | T1 ATV | T1 iPad | T2 ATV | T2 iPad |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| appears as Controllable | | | | | | | | | | |
| ERG target received | | | | | | | | | | |
| SIM grade received | | | | | | | | | | |
| power shown follows model | | | | | | | | | | |
| reconnect 5× ok | | | | | | | | | | |
| Zwift version / tvOS / iPadOS | | | | | | | | | | |

Decision rule:
- F works on Apple TV → FTMS face stays the release-1 plan.
- P1 works and F does not → pass-through face for the KICKR becomes the
  release-1 candidate.
- Only P2 works → Zwift keys on the name; the pass-through face is not
  shippable (no brand impersonation); stay with F or rethink.
- Same rules for T1 / T2 (Tacx). A T1/P1 failure is weaker evidence than an
  F pass: the tool only emulates part of each vendor protocol.
- All variants need no trainer, so all of them can be run locally. The remote
  KICKR owner is only needed for a capture of the real KICKR (gate G12).

## Remote test (KICKR owner without a dev setup)

1. Send a flashed or blank ESP32-S3 (e.g. XIAO) and the file
   `trainer_face_test_xiao_s3_merged.bin` (flash at offset 0x0).
2. Flash in Chrome with an ESP web flasher (Connect → offset 0x0 → bin →
   Program), or skip if pre-flashed.
3. Open a Web Serial monitor in Chrome at 115200, type `p`, then `k` for the
   second run; follow the procedure above; save the log as text.
4. Return the log and the filled table. If a Mac (PacketLogger + iOS logging
   profile) or Android (HCI snoop log) is available, also capture Zwift
   controlling the real KICKR directly (gate G12).

## Limits of this sketch

- Wahoo response format (`[0x01, opcode]`) is a guess; 0x46 grade is printed
  in both reported encodings. The logged bytes are the point, not the model.
- FE-C: pages 54 (capabilities) and 71 (command status) are answered on a
  page-70 request with reported layouts; other requests are logged only.
- No encryption/bonding on the control points (FTMS 1.0 asks for it; the
  product will document this deviation).
- Heart rate is simulated (110 + power/10 bpm) only to test "one device".
