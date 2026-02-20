# Homewind Build Tools

## update_manifest.py

Generates [ESP Web Tools](https://esphome.github.io/esp-web-tools/) `manifest.json` files from Arduino IDE build output.

### How it works

1. Reads the firmware **version** from `library.properties`
2. Parses the **`flash_args`** file in each build directory for exact filenames and flash offsets (source of truth from the Arduino toolchain)
3. Writes a valid `manifest.json` into the build directory
4. With `--deploy`: copies `.bin` files + manifest to the website firmware directory

### Usage

Run from the Homewind library root:

```bash
# Update manifests for all boards
python3 tools/update_manifest.py

# Update only a specific board
python3 tools/update_manifest.py --board touch
python3 tools/update_manifest.py --board basic

# Update + copy firmware to website directory
python3 tools/update_manifest.py --deploy

# Combine: single board + deploy
python3 tools/update_manifest.py --board touch --deploy
```

### Board mapping

| Alias   | Build directory                                      | Manifest name    | Chip       | Website dir      |
|---------|------------------------------------------------------|------------------|------------|------------------|
| `touch` | `esp32.esp32.waveshare_esp32_s3_touch_amoled_164`    | Homewind Touch   | ESP32-S3   | `homewind-touch` |
| `basic` | `esp32.esp32.XIAO_ESP32S3`                           | Homewind Basic   | ESP32-S3   | `homewind-basic` |

To add a new board, add an entry to the `BOARD_CONFIG` dict in `update_manifest.py`.

### Directory structure

```
Homewind/
├── library.properties          ← version source
├── build/
│   ├── esp32.esp32.waveshare_.../
│   │   ├── flash_args          ← parsed for offsets + filenames
│   │   ├── Homewind.ino.bin
│   │   ├── Homewind.ino.bootloader.bin
│   │   ├── Homewind.ino.partitions.bin
│   │   ├── boot_app0.bin
│   │   └── manifest.json       ← generated
│   └── esp32.esp32.XIAO_.../
│       └── ...
└── tools/
    └── update_manifest.py
```

With `--deploy`, files are also copied to:

```
~/Documents/Works/Homewind/Website/src/firmware/
├── homewind-touch/
│   ├── manifest.json
│   ├── Homewind.ino.bootloader.bin
│   ├── Homewind.ino.partitions.bin
│   ├── boot_app0.bin
│   └── Homewind.ino.bin
└── homewind-basic/
    └── ...
```

## build_webui.py

Generates PROGMEM headers from `webui_src/` for the embedded web interface (GZIP-compressed).

## watch_webui.py

File watcher for `build_webui.py` during development.
