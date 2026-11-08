# Arduino Setup – Homewind (Display-Variante)

Board-Einstellungen und Abhängigkeiten, mit denen Homewind auf dem
**Waveshare ESP32-S3-Touch-AMOLED-1.64** baut und startet.

> **Wichtig:** Nach jedem Update des ESP32-Cores die Einstellungen unter
> `Tools` kontrollieren. Die Arduino IDE setzt dabei gerne Optionen auf die
> Standardwerte zurück – vor allem **PSRAM → Disabled**.

---

## 1. Board-Paket

- **Boards Manager:** `esp32 by Espressif Systems`
- **Version:** `3.3.12` (oder `3.3.7`) – **nicht** `3.3.11`

### Warum nicht 3.3.11

In Core 3.3.11 ist `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` in den
vorkompilierten Libraries deaktiviert. WiFi- und lwIP-Puffer landen dann im
internen RAM statt im PSRAM. Im Betrieb bleiben nur noch ~11 KB interner
Heap übrig (größter Block ~7 KB). Folge: WebSocket-Frames gehen verloren,
z. B. bleibt das Firmware-Update in der Web-UI bei „Searching Update…“
hängen, obwohl das Gerät das Update gefunden hat.

3.3.12 hat die Einstellung wieder aktiviert und entspricht bei allen
speicherrelevanten Optionen 3.3.7.

Diese Einstellung lässt sich in der Arduino IDE nicht ändern. Vor einem
Core-Update deshalb prüfen, ob sie gesetzt ist:

```bash
grep SPIRAM_TRY_ALLOCATE_WIFI_LWIP \
  ~/Library/Arduino15/packages/esp32/tools/esp32s3-libs/<VERSION>/sdkconfig
```

Erwartet: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y`. Nach dem Flashen sollte
`free_heap` unter `http://<IP>/api/v1/system/info` deutlich über 11 KB liegen.

## 2. Tools-Einstellungen

| Option | Wert |
|---|---|
| Board | Waveshare ESP32-S3-Touch-AMOLED-1.64 |
| **PSRAM** | **Enabled** |
| Partition Scheme | 16M Flash (3MB APP/9.9MB FATFS) |
| Flash Mode | QIO 80MHz |
| CPU Frequency | 240MHz (WiFi) |
| USB Mode | Hardware CDC and JTAG |
| USB CDC On Boot | Enabled |
| USB Firmware MSC On Boot | Disabled |
| USB DFU On Boot | Disabled |
| Upload Mode | UART0 / Hardware CDC |
| Upload Speed | 921600 |
| Arduino Runs On | Core 1 |
| Events Run On | Core 1 |
| Core Debug Level | None |
| Erase All Flash Before Sketch Upload | Disabled |

Vollständiger FQBN (z. B. für `arduino-cli`):

```
esp32:esp32:waveshare_esp32_s3_touch_amoled_164:UploadSpeed=921600,USBMode=hwcdc,CDCOnBoot=cdc,MSCOnBoot=default,DFUOnBoot=default,UploadMode=default,CPUFreq=240,FlashMode=qio,PartitionScheme=app3M_fat9M_16MB,DebugLevel=none,PSRAM=enabled,LoopCore=1,EventsCore=1,EraseFlash=none
```

### Warum PSRAM zwingend ist

LVGL holt seinen gesamten Speicher aus dem PSRAM (`lv_conf.h`:
`LV_MEM_CUSTOM_ALLOC → heap_caps_malloc(size, MALLOC_CAP_SPIRAM)`).
Ohne PSRAM schlägt jede Allokation fehl, und `LV_ASSERT_HANDLER` hält
die Firmware in einer Endlosschleife an – **ohne Fehlermeldung und ohne Reboot**.

Symptom im Serial Monitor – danach kommt nichts mehr:

```
[Heap] BOOT_RAW @00:00:00: free=..., largest=...
[App] Boot orchestrator started (Homewind v1.4.50)
[App] Initializing core...
```

## 3. Bibliotheken

Alle unter `~/Documents/Arduino/libraries/`. Für NimBLE-Arduino muss dort die gepatchte Workspace-Kopie liegen (Ordner `NimBLE-Arduino/` ersetzen); mit der Library-Manager-Version 2.5.2 kommt der Absturz beim Trennen zurück. `tools/build.sh` bindet die Workspace-Kopie selbst ein. Getestete Versionen:

| Bibliothek | Version |
|---|---|
| lvgl | 8.4.0 |
| HomeWindWSAmoled | 1.6.1 |
| NimBLE-Arduino | 2.5.2+hw1 – gepatchte Workspace-Kopie `NimBLE-Arduino/` (2.3.0–2.5.2 stürzen ab, wenn sich ein Central wie Zwift trennt; siehe `NimBLE-Arduino/PATCHES.md`) |
| ESP Async WebServer | 3.12.0 |
| Async TCP | 3.5.0 |
| ArduinoJson | 7.4.3 |
| BluetoothBikeSensorServer | 3.2.1 (≥ 3.2.1, API `BikeSensor.h`) |
| SmartMiFanAsync | 2.0.0 |
| wifiMangerLite | 3.1.0 (≥ 3.1.0) |
| NVSUtilityLibrary | 1.0.1 |
| DebugLog | 1.1.0 |
| GitFirmwareUpdate | 1.0.4 |

### LVGL-Konfiguration

Die Datei `lv_conf.h` muss **direkt** in `~/Documents/Arduino/libraries/`
liegen (neben dem Ordner `lvgl`, nicht darin). Relevante Einstellungen:

- `LV_MEM_CUSTOM 1` mit PSRAM-Allokation (siehe oben)
- `LV_COLOR_DEPTH 16`
- `LV_COLOR_16_SWAP 1`

## 4. Checkliste vor dem Upload

- [ ] Richtiges Board gewählt (Waveshare ESP32-S3-Touch-AMOLED-1.64)
- [ ] **PSRAM: Enabled**
- [ ] Partition Scheme: 16M Flash (3MB APP/9.9MB FATFS)
- [ ] USB CDC On Boot: Enabled (sonst keine Ausgabe im Serial Monitor)
- [ ] Richtiger Port gewählt

## 5. Build und Flash im Terminal

> **Bauen nur mit `tools/build.sh`.** Das Skript erzeugt `build_opt.h` mit
> `-DBBS_MAX_DISCOVERED=15` und `-DBBS_COMPILE_POWER=1`; ohne diese Flags
> brechen `static_assert`s den Build ab – Sketch → Upload in der Arduino IDE
> kompiliert daher nicht. Die `sdkconfig.h` im Homewind-Ordner erhöht die
> BLE-Verbindungen auf 4 (HR, CSC, Power + ein Central); greift sie nicht,
> bricht der Build ebenfalls ab.

`tools/build.sh` baut beide Varianten mit den Einstellungen aus diesem
Dokument (Touch) bzw. für das XIAO ESP32-S3 ohne Display (Basic).
Lokale Libraries aus dem Workspace haben Vorrang vor `~/Documents/Arduino/libraries/`.

```bash
./tools/build.sh                  # beide Varianten, Ablage für ESP Web Tools
./tools/build.sh touch --flash    # Touch bauen und flashen
./tools/build.sh flash basic      # schon gebaute Basic-Firmware flashen
```

Details: `tools/README.md`.
