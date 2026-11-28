# miio_fake (P1)

UDP stand-in for a Xiaomi fan on port **54321**.

```bash
cd Homewind/tools/miio_fake
python3 fake_miio.py
# with token (needs PyCryptodome for encrypted replies):
python3 fake_miio.py --token 00112233445566778899aabbccddeeff --model zhimi.fan.za5

python3 -m unittest test_fake_miio.py -v
```

HELLO-only mode needs no extras. Encrypted `miIO.info` / `set_properties` need `pip install pycryptodome`.
