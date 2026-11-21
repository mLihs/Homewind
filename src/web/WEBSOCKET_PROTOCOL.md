# WebSocket Binary Telemetry Protocol v1

## Overview

Binary WebSocket protocol for server → client telemetry (observation only). Designed for low memory usage, deterministic behavior, and reconnect safety.

## Frame Format

All frames follow the format: `[type:uint8][payload:variable]`

- **type**: Frame type identifier (1 byte)
- **payload**: Frame-specific binary data (variable length)

## Frame Types

### Snapshot Frames (Full State)

Sent on client connect and periodically (every 30 seconds) to ensure reconnect safety.

#### SYSTEM_STATUS (0x01) - System Health

**Payload:**
```
[uptime_ms:uint32][free_heap:uint32][largest_free_block:uint32]
```
Total: 12 bytes

- `uptime_ms`: System uptime in milliseconds (little-endian)
- `free_heap`: Free heap memory in bytes (little-endian)
- `largest_free_block`: Largest free heap block in bytes (little-endian)

**Total size:** 12 bytes

#### SENSORS_SNAPSHOT (0x02) - All Configured Sensors

**Payload:**
```
[count:uint8][sensor1:84]...[sensorN:84][status1:uint8]...[statusN:uint8]
```

**Sensor entry (fixed 84 bytes):**
```
[name:64][type:1][mac:17][connected:1][battery:1]
```

- `count`: Number of sensors (0-3, only `count` entries sent, each entry is fixed 84 bytes)
- `name`: Sensor name (UTF-8, fixed 64 bytes, null-terminated, null-padded)
- `type`: Sensor type (0 = HR, 1 = CSC, 2 = PWR)
- `mac`: MAC address string (fixed 17 bytes, null-terminated, null-padded, format "XX:XX:XX:XX:XX:XX")
- `connected`: Connection status (0 = disconnected, 1 = connected)
- `battery`: Battery level in percent (0-100, 255 = unknown)
- `status` (since 1.6.0, one byte per entry, after all entries): 0 = ok, 1 = duplicate (another sensor type already uses this MAC; the sensor is saved but not active). Decoders that stop after `count` entries ignore it.

**Note:** Fixed-size encoding prevents heap fragmentation from variable WebSocket allocations. Each sensor entry is exactly 84 bytes, so frame size is predictable: 1 + count×85 bytes (max 256 bytes for 3 sensors). A decoder should skip a record with an unknown `type` (the WebUI does since 1.7.0).

#### FANS_SNAPSHOT (0x07) - All Configured Fans

**Payload:**
```
[count:uint8][fan1:35][fan2:35][fan3:35][fan4:35]
```

**Fan entry (fixed 35 bytes):**
```
[token:32][connected:1][control_active:1][recovery:1]
```

- `count`: Number of fans (0-4, only `count` entries sent, each entry is fixed 35 bytes)
- `token`: Fan token string (hex, fixed 32 bytes, null-terminated, null-padded)
- `connected`: Connection status (0 = disconnected, 1 = connected)
- `control_active`: Control active status (0 = inactive, 1 = active)
- `recovery`: Recovery UI state (0 = normal, 1 = recovering, 2 = exhausted)

**Note:** Fixed-size encoding prevents heap fragmentation from variable WebSocket allocations. Each fan entry is exactly 35 bytes, so frame size is predictable: 1 + count×35 bytes (max 141 bytes for 4 fans).

#### HEART_RATE_SETTINGS (0x0B) - Heart Rate Min/Max Settings

**Payload:**
```
[min:uint16][max:uint16]
```
Total: 4 bytes

- `min`: Minimum heart rate (BPM, little-endian)
- `max`: Maximum heart rate (BPM, little-endian)

Sent when the range changes: `heartrate/setmin`, `heartrate/setmax` or plus/minus on the Touch display (shifts min and max by 5 bpm). The new range applies at once; it is saved to NVS 3 s after the last change (since 2.1.0), so a restart within those 3 s keeps the previous range.

### Control Frames (WebUI Internal)

Control frames are WebUI helpers (not application telemetry). They should be ignored by other consumers.

#### CLIENT_ID (0x0C) - WebSocket Client ID Announcement

Sent by the server to the connecting client. The WebUI can then call:

- `POST /api/v1/action/telemetry/refresh?client_id=<id>`

to request **unicast** delivery of heavy snapshots (`SENSORS_SNAPSHOT`, `FANS_SNAPSHOT`, plus the small `BIKE_METRICS`) without triggering broadcast bursts. The handler only records the request; the frames follow from the main loop.

**Payload:**
```
[client_id:uint32]
```
Total: 4 bytes

### Delta Frames (Incremental Updates)

Sent only when state changes (marked dirty).

#### SENSOR_CONN_STATE (0x03) - Sensor Connection State Change

**Payload:**
```
[name_len:uint8][name:bytes][connected:uint8]
```

- `name_len`: Sensor name length
- `name`: Sensor name (UTF-8)
- `connected`: New connection status (0 = disconnected, 1 = connected)

#### HEART_RATE (0x06) - Heart Rate Value Update

**Payload:**
```
[value:uint16]
```

- `value`: Heart rate in BPM (little-endian)

#### BIKE_METRICS (0x0D) - Power and Cadence (since 1.7.0)

**Payload:**
```
[power_w:int16][cadence_rpm:uint16][flags:uint8]
```
Total: 5 bytes

- `power_w`: Instantaneous power in watts (signed, little-endian); 0 when bit 0 is clear or the last power packet is older than 3 s
- `cadence_rpm`: Cadence in rpm (little-endian); 0 when bit 1 is clear or the value is older than 3 s
- `flags`:
  - bit 0: power valid. The power meter is subscribed and has sent data since it connected (BikeSensor `lastPower()`). Stays set while it is connected; without a packet for more than 3 s, `power_w` is 0.
  - bit 1: cadence valid. The cadence source has delivered (BikeSensor `lastCadence()`): the CSC sensor while it is subscribed, otherwise a subscribed power meter once it has sent crank data. `cadence_rpm` is 0 when the last crank event (power meter) or CSC packet is older than 3 s; a CSC sensor also reports 0 itself when the crank stops.
  - bit 2: cadence comes from the power meter (no CSC sensor is subscribed). Can be set while bit 1 is clear (power meter without crank data yet).
  - bit 3 (`HW_ENABLE_TRAINER_MIRROR`, on in release builds since 2.0): `power_w` is the trainer's (Cycling Power read along by the trainer mirror). Set with bit 0 while no power meter is connected (D2 in `docs/TRAINER_PROXY_DESIGN.md` §3: power meter first, then the trainer); a connected power meter without a sample yet leaves bit 0 clear instead. Cadence keeps BikeSensor's order (CSC sensor, power meter, trainer) without its own flag. Clients that ignore bit 3 show the trainer's power as power.
  - bits 4–7: reserved, 0

Sent when the encoded value changes (new power or cadence sample, or a validity change, checked at least once per second), coalesced by the dirty flag like `HEART_RATE`. Sent once per client after connect (staged connect of the first client, and with the unicast snapshots requested by `telemetry/refresh?client_id=`). Smart and Hybrid also use power; see `FAN_CONTROL`.

#### FAN_CONTROL (0x0E) - Fan Mode and Target

In firmware built with `HW_ENABLE_FAN_POWER_MODE=1`, the default in every release build since 1.8.0 (`Config.h`); concept `docs/FAN_POWER_HR_CONCEPT.md` §5. Encoded by `hw::encodeFanControl()` (`src/core/FanControlFrame.h`).

**Payload:**
```
[mode:uint8][effective:uint8][reason:uint8][target_on:uint8][target_pct:uint8][dominant:uint8][power_state:uint8][ftp_w:uint16][cooldown:int8]
```
Total: 10 bytes

- `mode`: stored mode (0 = Pure, 1 = Smart, 2 = Hybrid)
- `effective`: mode in effect; Hybrid runs as 1 = Smart without a configured power source or a fresh power sample, or with an HR span < 30 bpm
- `reason`: why `effective` differs from `mode`: 0 = none, 1 = no power, 2 = HR span < 30 bpm
- `target_on`, `target_pct`: FanHRAdapter's per-fan state (ON and speed from the fans' acknowledgements, OFF assumed when sent), highest speed over the fans that are ON (0..95); `target_pct` is 0 while off and until the first speed acknowledgement after ON
- `dominant`: 0 = HR, 1 = power, 2 = hold (the fall-rate limit holds the output), 3 = fail-safe
- `power_state`: 0 = none (stale, not connected or not configured), 1 = idle, 2 = pedalling (> 30 W or cadence)
- `ftp_w`: stored FTP in watts (little-endian)
- `cooldown`: stored cool-down, signed −2..+2 (0 = default); sets the fall rate in Smart and Hybrid to 30 / 25 / 20 / 15 / 10 % of full scale per minute

Sent when the status changes, at most once per 3 s (a mode, FTP or cool-down change at once), on client connect and on `telemetry/refresh`. Before fan control runs (fans and BLE still starting), only `mode`, `effective` = `mode`, `ftp_w` and `cooldown` are set; receiving the frame at all means the firmware supports the fan modes. The WebUI shows the mode selector and FTP in the heart rate card from the first frame on and animates the fans from `target_on`/`target_pct`; without the frame it keeps the client-side Pure curve.

#### TRAINER_STATUS (0x0F) - Trainer Mirror Status (trainer firmware only)

Only in firmware built with `HW_ENABLE_TRAINER_MIRROR=1` (every release build since 2.0); design `docs/TRAINER_PROXY_DESIGN.md`, spec `docs/AGENT_TRAINER_MIRROR.md` Task 4. Same data as `GET /api/v1/action/trainer/status`.

**Payload:**
```
[state:uint8][flags:uint8][setup:uint8][name_len:uint8][name:bytes]
```
Total: 4 + name_len bytes (name ≤ 31 bytes)

- `state`: BikeSensor `TrainerState`: 0 = off, 1 = waiting, 2 = connecting, 3 = discovering, 4 = ready, 5 = changed (GATT changed, not mirrored), 6 = setup
- `flags`:
  - bit 0: an app is connected to the mirror
  - bit 1: mirror: this boot mirrors the trainer
  - bit 2: enabled: NVS `trn_on`, the mirror runs after the next restart
  - bit 3: set up: a trainer is stored (NVS `trn_dev`; also a released one, which still counts for the device rule)
  - bits 4–7: reserved, 0
- `setup`: result of the last trainer setup this boot: 0..8 = BikeSensor `TrainerCheck` (0 ok, 1 not found, 2 link failed, 3 too large, 4 empty, 5 empty service, 6 no CCCD, 7 missing advertised, 8 store failed), 0xFE = running, 0xFD = busy, 0xFC = device rule, 0xFF = idle (none; also after enable/forget)
- `name`: stored trainer name (UTF-8, may be empty)

Sent when the status changes (setup start and result, enable/forget, link or app changes, checked at least once per second), once per client after connect (staged connect and the unicast snapshots of `telemetry/refresh?client_id=`) and on `telemetry/refresh`. Receiving the frame at all means the firmware supports the trainer mirror: the WebUI shows the Trainer row in "Setup Sensors" only then.

#### APP_STATUS (0x10) - Connected Apps

In every firmware with BLE (`HW_ENABLE_BLE`), not only trainer builds.

**Payload:**
```
[apps:uint8]
```
Total: 1 byte

- `apps`: number of apps (BLE centrals, e.g. Zwift) connected to Homewind, to the plain relay or to the trainer mirror (BikeSensor `centralCount()`)

Polled in every `loopFlush()` (main loop task) while a WebSocket client is connected; sent when the number changes, on WebSocket client connect and with the unicast snapshots of `telemetry/refresh?client_id=`. Not sent before the BLE relay is ready. The WebUI skips its automatic restart after a trainer change while `apps > 0`; without this frame (older firmware) it falls back to bit 0 of `TRAINER_STATUS`.

#### DISCOVERY_STATUS (0x04) - Discovery State Change

**Payload:**
```
[active:uint8][type:uint8][error:uint8]
```

- `active`: Discovery active status (0 = stopped, 1 = active)
- `type`: Sensor type being discovered (0 = HR, 1 = CSC, 2 = PWR, 255 = all/unknown)
- `error` (since 1.6.0, optional for decoders): 0 = none, 1 = busy (the last start was refused because a discovery was still open). `discovery/start` answers 200 once the request is queued; a refusal is only reported here.

#### DISCOVERY_RESULTS (0x05) - Discovery Results

**Payload:**
```
[count:uint8][result1...resultN][generation:uint32]
```

**Result entry:**
```
[index:uint8][name_len:uint8][name:bytes]
```

- `count`: Number of entries (at most 15: Homewind keeps 15 discovery results, which always fit into one 512-byte frame)
- `generation` (appended in 1.6.0, little-endian, optional for decoders): identifies the discovery run. Pre-1.6.0 decoders ignore it.
- `index`: Discovery snapshot index
- `name_len`: Device name length
- `name`: Device name (UTF-8, for UI display only). A device without a name is shown with its MAC (`aa:bb:cc:dd:ee:ff`), the same text that is stored when it is selected.

**Note:** This frame format is optimized to reduce WebSocket bandwidth. The UI only receives index + name. When the user selects a sensor, the UI posts `sensor/set?generation=<generation>&index=<index>`; the main loop resolves type, MAC and address type from the same discovery snapshot. A generation or index that no longer matches is rejected with 409.

#### FIRMWARE_VERSION (0x08) - Firmware Version Update

**Payload:**
```
[version_len:uint8][version:bytes][board_len:uint8][board:bytes]
```

- `version_len`: Version string length
- `version`: Version string (UTF-8, e.g., "1.2.3")
- `board_len`, `board`: Optional. Detected board revision ("V1", "V1.1"), only sent on display builds. Clients that read only the version ignore it.

#### FIRMWARE_PROGRESS (0x09) - Firmware Update Progress

**Payload:**
```
[percent:uint8]
```

- `percent`: Update progress percentage (0-100)

#### FIRMWARE_SEARCH_RESULT (0x0A) - Firmware Search Result

**Payload:**
```
[has_update:uint8][version_len:uint8][version:bytes][notes_len:uint16][notes:bytes]
```

- `has_update`: Whether update is available (0 = false, 1 = true)
- `version_len`: Version string length (0-255, only present if has_update=1)
- `version`: Version string (UTF-8, e.g., "1.2.3", only present if has_update=1)
- `notes_len`: Release notes length (uint16, little-endian, only present if has_update=1)
- `notes`: Release notes text (UTF-8, only present if has_update=1)

**Note:** If `has_update=0`, only the first byte is sent. If `has_update=1`, version and notes follow.

## Architecture

### Dirty-Flag + Loop-Flush Model

1. **Mark Dirty**: Call `markDirty(frameType)` from async contexts (BLE callbacks, etc.)
2. **Loop Flush**: Call `loopFlush()` from main `loop()` to send all dirty frames
3. **No Async Sending**: Never send frames directly from async callbacks

### Reconnect Safety

- On client connect, snapshot frames are automatically marked dirty
- `loopFlush()` sends full snapshot on next loop iteration
- Ensures clients always receive complete state after reconnection

### Memory Usage

- Static buffers (512B frame buffer)
- No heap allocation in frame builders
- Chunked frame building to minimize stack usage

## Endianness

All multi-byte integers are **little-endian** (LSB first).

## String Encoding

All strings are **UTF-8** encoded, **not null-terminated**. Length prefix indicates exact byte count.

## Example Frame

**SYSTEM_STATUS frame:**
```
[0x01][0x34 0x12 0x00 0x00][0x00 0x10 0x00 0x00][0x00 0x08 0x00 0x00]
  ^      ^uptime: 4660ms      ^free_heap: 4096     ^largest: 2048
 type
```

## Integration Notes

- Protocol is implemented by `WebSocketTelemetry` (see `src/web/WebSocketTelemetry.*`)
- Frames are built from `SystemState`, `BLERelayManager`, `FanController`, and firmware update state
- All frame builders return 0 if buffer is too small or data unavailable


