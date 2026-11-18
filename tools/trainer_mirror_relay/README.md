# trainer_mirror_relay v0.3 (gate G5 prototype)

Homewind-independent test: a spare ESP32-S3 mirrors the real trainer (NEO 2T) and
offers it to Zwift as `homewind-test`. Answers the go/no-go question of the trainer
mirror: does Zwift accept the mirror as a controllable trainer under Homewind's name?

## Flash
`trainer_mirror_relay_xiao_s3_merged.bin` at offset 0x0 (XIAO ESP32-S3), or build with
arduino-cli + NimBLE-Arduino 2.5.2+hw. Serial monitor 115200, line ending "newline".

## Setup (once)
1. Trainer on, Zwift/other apps not connected to it.
2. `scan` → list with index, address, name, service UUIDs, raw advertising (save it).
3. `use N` (N = the trainer) → reads the full GATT incl. values, prints it, stores it,
   reboots. DFU is skipped.

## Run
- After boot the ESP32 connects to the trainer and only then advertises
  `homewind-test` (+ simulated HR 120 bpm).
- Zwift (iPad first, then Apple TV): Pair → note which tiles offer `homewind-test`
  (Power, Cadence, Controllable, Heart rate). Pair all from it.
- Free ride (grade changes) and an ERG workout (target changes). Watch the log:
  `APP>TR` app writes forwarded (`+Nms` = relay delay), `TR>APP`/`TR>APPi` trainer
  notifications/indications forwarded, `upstream notify/indicate …` subscriptions.
- `cps` toggles replacing the trainer's CPS power by 200 W → does Zwift show 200 W
  (power from CPS) or the real power (power from FE-C/FC82)?
- Trainer power off → app is disconnected; trainer on → mirror comes back.
- `dump` prints the stored GATT table. `forget` resets. Before switching between iPad and Apple TV remove `homewind-test`
  in Zwift and in Bluetooth settings.

## Record
Save the full serial log of every run and fill in:

| Check | iPad | Apple TV |
| --- | --- | --- |
| `homewind-test` offered as Controllable | | |
| ERG target follows (trainer resistance) | | |
| Simulation follows (grade felt) | | |
| Power / cadence / HR shown | | |
| `cps` ON: Zwift shows 200 W? | | |
| Trainer power cycle: recovers | | |
| Zwift restart ×5: recovers | | |
| Relay delay median (`+ms`) | | |
| Services Zwift subscribes / writes (FE-C, FC82, CPS) | | |
| Zwift / iPadOS / tvOS versions | | |

## Prototype limits
Blocking NimBLE client calls in `loop()`; value cap 64 B; one app; no NVS hash/change
detection; HR simulated. Not product code.
