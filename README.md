# Homewind

[![License](https://img.shields.io/badge/license-PU%2BCU-blue.svg)](LICENSE)
[![Website](https://img.shields.io/badge/web-homewind.io-f00f66.svg)](https://homewind.io)

**Homewind is free ESP32 firmware that sets your Xiaomi Smart Fan's speed from your heart rate and links your sensors to Zwift and Rouvy on Apple TV.**

<!-- Demo GIF: rider pedals, heart rate rises, fan speeds up. Replace the path once the clip exists. -->
<!-- ![Homewind demo](docs/media/homewind-demo.gif) -->

- **Heart-rate fan control.** You set the heart rate where the fan starts and where it reaches full speed. In between, fan speed follows your pulse.
- **Bluetooth sensor hub for Apple TV.** Pair heart rate, power, speed and cadence sensors once with Homewind. Zwift or Rouvy then see a single Bluetooth device.
- **No soldering, no programming.** Flash the firmware in your browser, set it up on your phone, update over the air.
- **Local only.** Homewind talks to the fan over your home Wi-Fi.

→ **Website, setup guide and one-click install: [homewind.io](https://homewind.io)**

## Get started (no coding)

1. Get a supported Xiaomi fan and one of the two boards below.
2. Open [homewind.io](https://homewind.io) in Chrome or Edge on a desktop, connect the board via USB and click **Flash**.
3. Connect to the `Homewind` Wi-Fi, choose your 2.4 GHz network and follow the setup page.
4. Read your fan's token with the [Xiaomi Cloud Token Extractor](https://github.com/mLihs/GUI-Miio-Tokenextrator-All-Plattforms/releases) (desktop app for macOS, Windows and Linux; log in with your Xiaomi account or by QR code) and paste the 32-character token into the **Fans** section.
5. Optional: add your Bluetooth sensors in the **BLE** section.

Full guide: [homewind.io/setup](https://homewind.io/setup/) · German: [homewind.io/de/anleitung](https://homewind.io/de/anleitung/)

### Hardware

| Version | Board | Notes |
| --- | --- | --- |
| Homewind Touch | Waveshare ESP32-S3-Touch-AMOLED-1.64 | 1.64″ touch display on the bars |
| Homewind Basic | Seeed Studio XIAO ESP32-S3 | Smallest build, set up in the browser |

Housing and stand: free 3D-print files on [MakerWorld](https://makerworld.com/en/@hires2k/).

### Supported fans

- Xiaomi Smart Standing Fan 2 Pro
- Xiaomi Smart Standing Fan 2
- Smartmi Standing Fan 3
- Xiaomi Smart Standing Air Circulation Fan

Several fans can be controlled at once. Your fan is missing? [Open an issue](../../issues) with the model name.

### Sensors

Bluetooth Low Energy heart rate, power, speed and cadence sensors. ANT+ is not supported.

Homewind remembers a sensor by its Bluetooth address. If a sensor is not found after sleep, wait up to 5 minutes; only then search for it again in the **BLE** section. Some sensors use a random address (for example CAD-BLE cadence sensors) and can change it after deep sleep; Homewind then no longer finds them until they are searched again.

Garmin Cadence Sensor 2: after 2–3 minutes without movement the sensor ends the connection and then stays silent for up to about 5 minutes, even if the crank turns. A blinking LED shows that the sensor is awake again. Homewind connects again on its own at most about 35 seconds after the sensor is back. Removing the battery skips the pause.

## Build from source

Only needed if you want to change the firmware. Everyone else uses the browser install above.

Detailed board settings and background: [ARDUINO_SETUP.md](ARDUINO_SETUP.md).

### 1. ESP32 core

Boards Manager → **esp32 by Espressif Systems**, version **3.3.12** (or 3.3.7). **Do not use 3.3.11:** its precompiled libraries keep Wi-Fi and lwIP buffers out of PSRAM, only about 11 KB of internal heap remain, WebSocket frames get lost and the firmware update in the web UI hangs at "Searching Update…".

Before a core update, check that the setting is on:

```bash
grep SPIRAM_TRY_ALLOCATE_WIFI_LWIP \
  ~/Library/Arduino15/packages/esp32/tools/esp32s3-libs/<VERSION>/sdkconfig
# expected: CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
```

After flashing, `free_heap` at `http://<IP>/api/v1/system/info` should be well above 11 KB.

### 2. Board settings (Homewind Touch)

| Option | Value |
| --- | --- |
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.64 |
| **PSRAM** | **Enabled** |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Flash Mode | QIO 80MHz |
| CPU Frequency | 240MHz (WiFi) |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| Upload Speed | 921600 |
| Arduino Runs On / Events Run On | Core 1 |

PSRAM is mandatory: LVGL allocates all its memory there. Without it the firmware stops right after `[App] Initializing core...`, with no error and no reboot. The Arduino IDE tends to reset PSRAM to Disabled after a core update, so check it every time.

FQBN for `arduino-cli`:

```
esp32:esp32:waveshare_esp32_s3_touch_amoled_164:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none
```

### Board settings (Homewind Basic)

| Option | Value |
| --- | --- |
| Board | XIAO_ESP32S3 |
| PSRAM | Disabled |
| Flash Size | 8MB |
| Partition Scheme | Default (8MB) |
| Flash Mode | QIO 80MHz |
| CPU Frequency | 240MHz (WiFi) |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Default (enabled on this board) |
| Upload Speed | 921600 |
| Arduino Runs On / Events Run On | Core 1 |

Set `HW_ENABLE_DISPLAY=0` for Basic builds in the Arduino IDE. `tools/build.sh` sets it automatically.

FQBN:

```
esp32:esp32:XIAO_ESP32S3:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=default,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,FlashSize=8M,PartitionScheme=default_8MB,DebugLevel=none,PSRAM=disabled,LoopCore=1,EventsCore=1,EraseFlash=none
```

### 3. Libraries

All in `~/Documents/Arduino/libraries/` (or next to the Homewind folder when you use `tools/build.sh`).

Homewind libraries (by the author):

| Library | Version | Purpose |
| --- | --- | --- |
| [BluetoothBikeSensorServer](https://github.com/mLihs/BluetoothBikeSensorServer) | 3.0.0 (required: ≥ 3.0.0, API `BikeSensor.h`) | Relays heart rate, speed/cadence and power from BLE sensors to a local GATT server |
| [SmartMiFanAsync](https://github.com/mLihs/SmartMiFanAsync) | 2.0.0 (required: ≥ 2.0.0) | Non-blocking Xiaomi / Smartmi fan control over miIO |
| [GitFirmwareUpdate](https://github.com/mLihs/GitFirmwareUpdate) | 1.0.4 | OTA firmware updates |
| [wifiMangerLite](https://github.com/mLihs/wifiMangerLite) | 3.0.0 | Wi-Fi setup via captive portal, NVS storage |
| [NVSUtilityLibrary](https://github.com/mLihs/NVSUtilityLibrary) | 1.0.1 | NVS settings storage (JSON) |
| [HomeWindWSAmoled](https://github.com/mLihs/HomeWindWSAmoled) | 1.5.10 | AMOLED display UI with LVGL (Touch only) |
| DebugLog | 1.1.0 | Logging |

Third-party libraries (Library Manager):

| Library | Version | Purpose |
| --- | --- | --- |
| ESP Async WebServer | 3.12.0 | Web server and `AsyncWebSocket` (web UI, API, telemetry) |
| Async TCP | 3.5.0 | TCP layer for the web server |
| ArduinoJson | 7.4.3 | JSON for API and settings |
| NimBLE-Arduino | 2.5.2+hw1 (workspace copy, patched) | Bluetooth LE stack. The workspace copy fixes a crash in 2.3.0–2.5.2 when a central (e.g. Zwift) disconnects; see `NimBLE-Arduino/PATCHES.md` |
| lvgl | 8.4.0 | Display graphics (Touch only). Use the preconfigured copy from `Amoled_LIB_Config/` (see below) |

Homewind uses `AsyncWebSocket` from ESP Async WebServer, not `arduinoWebSockets`.

**LVGL for the Touch display:** `Amoled_LIB_Config/LVGL_Waveshare.zip` contains LVGL 8.4.0 and a `lv_conf.h` set up for the Waveshare AMOLED display. Unzip it into `~/Documents/Arduino/libraries/`. That puts the `lvgl` folder and `lv_conf.h` side by side, exactly where they need to be:

```
~/Documents/Arduino/libraries/
├── lvgl/
└── lv_conf.h      # next to lvgl/, not inside it
```

The included `lv_conf.h` already has the required settings: `LV_MEM_CUSTOM 1` with PSRAM allocation, `LV_COLOR_DEPTH 16`, `LV_COLOR_16_SWAP 1`. Not needed for Homewind Basic.

### 4. Build and flash

**Option A – Arduino IDE:** open `Homewind.ino`, select board and port, check the settings above, then Sketch → Upload. The IDE uses the libraries in `~/Documents/Arduino/libraries/`: replace the `NimBLE-Arduino` folder there with the patched workspace copy first, otherwise the build uses the unpatched NimBLE-Arduino and the disconnect crash comes back.

**Option B – terminal, without the Arduino IDE:** `tools/build.sh` builds both variants with the board settings above, checks the result and flashes. It uses `arduino-cli`.

Requirements:

- `arduino-cli` with the ESP32 core 3.3.12 installed (`arduino-cli core install esp32:esp32@3.3.12`)
- `python3`
- These libraries as folders **next to** the Homewind folder (same parent directory): HomeWindWSAmoled, BluetoothBikeSensorServer, NimBLE-Arduino (patched 2.5.2), SmartMiFanAsync, wifiMangerLite, GitFirmwareUpdate, NVSUtilityLibrary, DebugLog. The script compiles these workspace copies and stops if it picks up a copy from `~/Documents/Arduino/libraries/` instead.
- The other libraries (lvgl, ESP Async WebServer, Async TCP, ArduinoJson) in the Arduino libraries folder, plus `lv_conf.h` as described above.
- No `build_opt.h` in the Homewind folder; it would override the variant flags.

```
workspace/
├── Homewind/                    # this repository
├── BluetoothBikeSensorServer/
├── DebugLog/
├── GitFirmwareUpdate/
├── HomeWindWSAmoled/
├── NVSUtilityLibrary/
├── SmartMiFanAsync/
└── wifiMangerLite/
```

Usage:

```bash
./tools/build.sh                          # build touch + basic
./tools/build.sh touch --flash            # build Touch and flash it
./tools/build.sh basic --flash --port /dev/cu.usbmodem1101
./tools/build.sh flash basic              # flash an existing build, no compile
./tools/build.sh ports                    # list serial ports
./tools/build.sh --clean                  # delete the object cache first
./tools/build.sh --no-deploy              # do not copy firmware into the website tree
```

What it does per variant:

1. Reads the version from `library.properties` and creates a build ID (date and time).
2. Sets `HW_ENABLE_DISPLAY` (Touch 1, Basic 0), `FW_VERSION` and `FW_BUILD_ID`.
3. Compiles with `arduino-cli`. The log goes to `build/.cache/<variant>.log`; on error the last 40 lines are shown.
4. Checks the binary: correct version and build ID, correct update URL for its variant, no URL of the other variant.
5. Copies the flash files to `build/<board>/`.
6. Writes the ESP Web Tools `manifest.json` via `tools/update_manifest.py` and, unless `--no-deploy` is set, copies the firmware into the website tree.

`all` cannot be combined with flashing. If several serial ports are found, pass `--port`. Details: [tools/README.md](tools/README.md).

On a successful start, the serial monitor shows the boot sequence, starting with:

```
[App] Boot orchestrator started (Homewind v1.x.x)
[App] Initializing core...
```

### Libraries per build option

Some libraries from the table above are only needed depending on `src/app/Config.h`:

| Library | Needed when |
| --- | --- |
| wifiMangerLite | `HW_WIFI_BACKEND_WML=1` (captive portal; default when `HW_DEV_MODE=0`) |
| NVSUtilityLibrary | with wifiMangerLite (stores Wi-Fi credentials and paired BLE sensors) |
| HomeWindWSAmoled, lvgl | `HW_ENABLE_DISPLAY=1` (Touch board). Set `HW_ENABLE_DISPLAY=0` for Basic builds |

Place libraries that are not in the Library Manager in your `libraries/` folder.

### Feature gates

Features are switched in `src/app/Config.h`:

```cpp
#define HOMEWIND_ENABLE_WEBSERVER    1  // web server (ESPAsyncWebServer)
#define HOMEWIND_ENABLE_WEBSOCKET    1  // WebSocket telemetry (AsyncWebSocket)
#define HOMEWIND_ENABLE_SETTINGS     1  // settings store (NVS)
#define HOMEWIND_ENABLE_API_ACTIONS  1
#define HOMEWIND_ENABLE_API_SETTINGS 1
```

Define them before `Config.h` is included, or edit `Config.h` directly.

### Project structure

```
Homewind/
├── Homewind.ino              # Main sketch (wiring only)
├── library.properties        # Arduino library metadata
├── Amoled_LIB_Config/        # LVGL 8.4.0 + lv_conf.h for the Waveshare display (zip)
├── webui_src/                # Web UI sources (HTML, CSS, JS)
├── tools/
│   ├── build.sh              # Builds Touch and Basic, flashes
│   └── build_webui.py        # Builds PROGMEM headers from webui_src/
├── generated/                # Generated web assets (do not edit)
└── src/
    ├── app/                  # App coordinator, build info, Config.h
    ├── core/                 # System state, non-blocking timing
    ├── settings/             # Settings store
    ├── wifi/                 # Wi-Fi connection management
    └── web/                  # Web server, WebSocket telemetry, HTTP API
```

### Web UI build and caching

Run `tools/build_webui.py` after changing anything in `webui_src/`. The script embeds all assets in flash (no filesystem needed) and computes a build hash from their content.

- The hash is appended to asset URLs (`app.js?v=HASH`), so browsers, including Safari, load fresh files after every firmware update.
- The server sends `ETag` and `Cache-Control` headers and answers `304 Not Modified` for unchanged files.

| Asset | Cache |
| --- | --- |
| `index.html` | `no-cache` (revalidated by ETag) |
| `app.css`, `app.js` | 1 day (hash in URL forces updates) |

### Troubleshooting

- **Serial output stops after `[App] Initializing core...`:** PSRAM is disabled. Set Tools → PSRAM → Enabled.
- **Firmware update in the web UI hangs at "Searching Update…":** ESP32 core 3.3.11. Use 3.3.12 or 3.3.7.
- **No output in the serial monitor:** set USB CDC On Boot → Enabled.
- **ESP32 platform not found:** install the ESP32 Arduino core via Tools → Board → Boards Manager.
- **`ESPAsyncWebServer.h` not found:** install ESPAsyncWebServer, or disable the web server in `Config.h`.
- **All subsystems show `DISABLED`:** the feature gates in `Config.h` are off.
- **Flash button greyed out on homewind.io:** use Chrome or Edge on a desktop. Safari and Firefox do not support Web Serial.
- **Board not found when flashing:** try another USB cable. Many are charge-only.

## Related projects

| Project | What it is |
| --- | --- |
| [Xiaomi Cloud Token Extractor](https://github.com/mLihs/GUI-Miio-Tokenextrator-All-Plattforms) | Desktop GUI to read Xiaomi device tokens, needed once to add a fan |
| [BluetoothBikeSensorServer](https://github.com/mLihs/BluetoothBikeSensorServer) | BLE sensor relay used by Homewind |
| [SmartMiFanAsync](https://github.com/mLihs/SmartMiFanAsync) | Fan control library used by Homewind |
| [Homewind on MakerWorld](https://makerworld.com/en/@hires2k/) | Housing and stand to print |

## Support the project

Homewind is free for private use. If it makes your training better, you can [support development via PayPal](https://www.paypal.com/donate/?hosted_button_id=9WSJYGER8UHHE). A star on GitHub helps others find the project.

## Contributing

Bug reports, fan models and pull requests are welcome. Please open an [issue](../../issues) first for larger changes.

## License

Free for private, non-commercial use. Commercial use requires a separate license. See [LICENSE](LICENSE).

Homewind is an independent open-source project and is not affiliated with Xiaomi, Smartmi, Apple, Zwift, Rouvy or Wahoo.

Contact: [hello@homewind.io](mailto:hello@homewind.io) · [homewind.io](https://homewind.io)
