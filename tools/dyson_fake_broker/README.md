# fake_dyson_broker

Minimal MQTT broker that approximates a Dyson fan for `tools/dyson_probe` P0.

```bash
python3 fake_dyson_broker.py --serial NK6-EU-P0PROBE01 --product 438 --password probe
python3 fake_dyson_broker.py --auth-fail
python3 fake_dyson_broker.py --pool 2          # max concurrent sessions (dead keep a slot)
python3 fake_dyson_broker.py --raw-password --password '<88-char mqtt password>'
```

Optional: `pip install zeroconf` for `_dyson_mqtt._tcp` advertisement.
