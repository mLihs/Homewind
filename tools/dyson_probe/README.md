# dyson_probe (P0)

Homewind-independent sketch: talk to a Dyson fan (or the fake broker) over local
MQTT on an ESP32-S3. Answers go/no-go before FanBus / SmartMiFanAsync 3.0.

See `Refactoring/AGENT_DYSON_P0.md` and `Refactoring/FAN_DRIVER_CONCEPT.md` §5 / §7 P0.

## Fake broker (Mac)

```bash
cd Homewind/tools/dyson_fake_broker
python3 fake_dyson_broker.py --serial NK6-EU-P0PROBE01 --product 438 --password probe
# optional: pip install zeroconf   # for _dyson_mqtt._tcp
```

Prints the derived 88-char MQTT password (SHA-512 + Base64 of the sticker password).

## Flash

Arduino IDE / arduino-cli, board ESP32-S3 (XIAO recommended). Needs **AsyncTCP**
(same as Homewind). Open `dyson_probe.ino`, upload, serial 115200.

## Run against fake broker

1. Note Mac LAN IP (broker listens on `:1883`).
2. On the probe:

```
wifi <ssid> <pass>
host <mac-ip> 1883
serial NK6-EU-P0PROBE01
product 438
sticker probe
connect
stage 5
off
heap
disc
```

Expect: `CONNACK 0`, then `PUBACK` for each publish. Broker log shows `fnsp`.

## Run against a real Dyson

```
wifi …
host <fan-ip> 1883          # or discover via mDNS later
serial <SN from sticker>
product <e.g. 438E>         # type from the account (opendyson), NOT the mDNS prefix
sticker <sticker password>  # or: cred <88-char local password>
client hw-deadbeef-0        # keep stable across reboots (≤23 chars)
connect
stage 1
…
```

Link-era models may need `fmod` instead of `fpwr` — note what works in the P0 log.

TP09 (2026-11-28): mDNS says `438`, but only `438E/…` works. A wrong product
type still gets PUBACK — check the fan display, not the log. The fan also
needs the clean-session flag in CONNECT (`CONNACK 3` without).

## Pass checklist

| Check | Result |
| --- | --- |
| CONNACK 0 with sticker→SHA512 password | |
| Stages 1–10 + off, PUBACK each time | |
| `pct 0` / `pct 95` / `pct 100` mapping | |
| Keepalive / `ping` → PINGRESP | |
| `disc` then reconnect same client id | |
| 10× probe reboot (fake `--pool 3` without replace would fail; with replace should live) | |
| `heap` before/after 1 connection | |
| Real TP07 / PH (if available) | TP09 OK 2026-11-28 (`cred`, `438E`); PH open |

## Host tests (no board)

```bash
cd Homewind
SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk   # if default SDK is broken
xcrun clang++ -std=c++17 -Wall -Wextra -Werror -isysroot "$SDK" \
  -Itools/dyson_probe tests/dyson_mqtt_min_test.cpp \
  -o /tmp/dyson_mqtt_min_test && /tmp/dyson_mqtt_min_test

cd tools/dyson_fake_broker && python3 -m unittest test_sticker_password.py -v
```

Covers: remaining-length > 127 (88-char CONNECT password), PUBLISH/PUBACK/CONNACK poll,
percent→stage, skip-same-stage, sticker→SHA-512/Base64 password.

## Not in P0

FanBus, NVS, WebUI, Homewind link, Tuya, SUBSCRIBE/status JSON.
