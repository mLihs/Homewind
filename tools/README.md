# Homewind Build Tools

## build.sh

Compiles both firmware variants with `arduino-cli`, flashes them from the terminal, and refreshes the ESP Web Tools files.

| Variant | Board | `HW_ENABLE_DISPLAY` | Website directory |
|---------|-------|---------------------|-------------------|
| `touch` | Waveshare ESP32-S3-Touch-AMOLED-1.64 | 1 | `homewind-touch` |
| `basic` | XIAO ESP32-S3 | 0 | `homewind-basic` |

Touch uses the FQBN from `ARDUINO_SETUP.md` (PSRAM enabled, 16MB partition, USB CDC on boot). Basic uses the XIAO defaults: 8MB flash, 3MB APP partition, PSRAM off, USB CDC on boot.

Workspace copies of `HomeWindWSAmoled`, `NimBLE-Arduino` (patched 2.5.2), `SmartMiFanAsync`, `wifiMangerLite`, `GitFirmwareUpdate`, `NVSUtilityLibrary` and `DebugLog` override the copies in `~/Documents/Arduino/libraries`. Other libraries (LVGL, ESPAsyncWebServer, …) still come from the Arduino libraries folder.

```bash
# Build both variants, write local manifest.json, copy bins to the website tree
./tools/build.sh

# One variant
./tools/build.sh touch
./tools/build.sh basic

# Build without copying to the website (local manifest.json is still updated)
./tools/build.sh all --no-deploy

# Build and flash (port is auto-detected when exactly one board is connected)
./tools/build.sh touch --flash
./tools/build.sh basic --flash --port /dev/cu.usbmodem1101

# Flash an existing build without compiling
./tools/build.sh flash touch

# List serial ports
./tools/build.sh ports
```

Every successful build refreshes `build/<board>/manifest.json` for ESP Web Tools. `--no-deploy` skips only the website copy. `--clean` deletes the object cache before compiling.

From 1.8.0, release builds enable fan modes Pure / Smart / Hybrid by default (`HW_ENABLE_FAN_POWER_MODE=1` in `Config.h`). `--fan-power` still forces the gate on for a test stamp (build id ends in `-fanpower`) and requires `--no-deploy`. Concept: `docs/FAN_POWER_HR_CONCEPT.md`.

Outputs:

- `build/esp32.esp32.waveshare_esp32_s3_touch_amoled_164/` and `build/esp32.esp32.XIAO_ESP32S3/` — bins, `flash_args`, `manifest.json` (what ESP Web Tools needs)
- `build/.cache/` — object files (`<variant>/`) and build stamp/`build_opt.h` (`<variant>.opt/`), gitignored
- `~/Documents/Works/Homewind/Website/src/firmware/homewind-touch/` and `homewind-basic/` — the same bins plus `manifest.json` and `latest.json`. `notes` must be at most 160 UTF-8 bytes, the same limit the ESP uses for the web UI. Longer text stops the deploy. Umlauts and emoji count as multiple bytes.

Each run writes a stamp header (`FW_VERSION` from `library.properties`, `FW_BUILD_ID` as `YYYYMMDD-HHMMSS`, and `HW_ENABLE_DISPLAY`) and a `build_opt.h` into `build/.cache/<variant>.opt/`, outside arduino-cli's build path, and passes it with `--build-property build.opt.path=…` (arduino-cli wipes the build path whenever its build options change, e.g. after a new sketch file, and the core then leaves an empty `build_opt.h` there). `Config.h` and `BuildInfo.h` pick the stamp up when it is on the include path. The generated `build_opt.h` also sets `-DBBS_MAX_DISCOVERED=15` and `-DBBS_COMPILE_POWER=1`; builds without it (e.g. the Arduino IDE) stop at a `static_assert`, and the script fails if the compile log does not show its `build_opt.h`. When `build_opt.h` or the sketch-folder `sdkconfig.h` changes (or `sdkconfig.h` appears or disappears), the variant's object cache is deleted, because arduino-cli would keep stale library objects. After linking, the script checks that the Touch binary contains the touch OTA URL and the Basic binary the basic URL.

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
