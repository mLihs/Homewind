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
  - bits 3–7: reserved, 0

Sent when the encoded value changes (new power or cadence sample, or a validity change, checked at least once per second), coalesced by the dirty flag like `HEART_RATE`. Sent once per client after connect (staged connect of the first client, and with the unicast snapshots requested by `telemetry/refresh?client_id=`). Fan control uses heart rate only.

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


