# AGENT_NETWORK_REACHABILITY.md — DHCP hostname and mDNS discovery

Status 2026-11-22: **shipped in Homewind 2.2.0**. Task 0 BLE measurement
(Touch 192.168.178.40, HR + Neo mirror + app, no WebUI): 10× `arp -d` + 20
pings, **0% loss**, first reply always icmp_seq 0 → ARP loss under BLE
**refuted**. Task 4 not shipped. Idle BLE comparison optional.

Client-side counterpart (Karoo extension):
`Homewind-Bike-Computer/docs/HOMEWIND_DISCOVERY.md`.

## Observations

- FRITZ!Box lists Homewind as `esp32s3-F3B3E4` ("Erstverbindung als" the
  same, 2026-11-07), mDNS answers as `homewind-xxxxxxxx.local`.
- Karoo 3 extension: the mDNS search (`_http._tcp`, `homewind-…`) finds
  Homewind only sometimes; sometimes an immediate `ConnectException` to the
  IP. Browsers on several devices reach Homewind at the same time without
  problems.

## Findings (verified in code, core 3.3.12)

### 1. Router shows the core default name

- `App::initWiFi()` calls `WiFi.mode(WIFI_STA)` before any hostname is set
  (`src/app/App.cpp:758`).
- The core copies the hostname into the STA netif **only** when STA is
  switched on (`WiFiGeneric.cpp:642-643`). The default is `esp32s3-` + base
  MAC bytes 3–5 (`NetworkManager.cpp:159-165`) = `esp32s3-F3B3E4`.
- wifiMangerLite later calls `WiFi.mode()` (no-op, mode unchanged) and
  `WiFi.setHostname(cfg.deviceName)` (`WiFiManagerLite.cpp:550-552`). That
  only changes a static string (`NetworkManager.cpp:168-172`); nothing
  before `WiFi.begin()` writes it into the netif. DHCP option 12 therefore
  carries `esp32s3-F3B3E4`.
- Exception: after an AP fallback, `WiFi.disconnect(true, true)` turns the
  STA off and `WiFi.mode(WIFI_AP_STA)` re-applies the stored name
  (`WiFiManagerLite.cpp:732-739`). That is why the correct name shows up
  occasionally.
- The dev backend does it in the right order (`src/wifi/WiFiService.cpp:36`
  before `WiFi.mode()`).
- mDNS (`MDNS.begin`) is a separate mechanism; the router never uses it.

### 2. Discovery fails while HTTP works

- Search and access are different paths. Browsers use the known IP over
  unicast (acknowledged and retried by 802.11 and TCP). mDNS is multicast:
  sent once, no ACK, no retry.
- Power save is **not** the core default: wifiMangerLite calls
  `WiFi.setSleep(false)`, applied at once because STA is already started
  (`WiFiGeneric.cpp:779-796`, re-applied on STA start `STA.cpp:120-124`).
  `WIFI_PS_NONE` with NimBLE is allowed since IDF 5.0 (esp-idf #9595);
  core 3.3.12 uses IDF 5.5.
- mDNS is started twice per connect: wifiMangerLite in `handleConnected()`
  (`WiFiManagerLite.cpp:650-659`), then Homewind's `Connected` handler runs
  `MDNS.end()` + `MDNS.begin()` (`src/wifi/WiFiServiceWML.cpp:140-149`).
  `mdns_free()` is synchronous, so the restart works; it only costs goodbye
  packets, a new probe, and freeing/reallocating the mDNS task (4 KB stack).
  Cleanup, not the root cause.
- esp-mdns with `CONFIG_MDNS_PREDEF_NETIF_STA=1` re-enables the STA netif on
  `GOT_IP` by itself, so wifiMangerLite's `_mdnsActive` staying `true` after a
  reconnect is harmless.
- BLE scanning (`BBSNimBLEPort.h:108-109`, interval 97 / window 67 ≈ 69 %
  duty, 5 s every 30 s, occasional 10 s name scans) takes radio time and can
  cost incoming multicast queries. Hypothesis only; Android repeats queries,
  so a 10–15 s browse should usually still hit. Other candidates: Karoo
  Wi-Fi power save, Android NSD cache, mesh multicast handling (**verify**
  with a capture).
- ARP loss (the earlier hypothesis) is **unproven**, not refuted:
  - Modem sleep is off (see above), so "ARP missed in modem sleep" is
    unlikely. Loss of broadcast ARP requests during BLE airtime remains
    possible (**verify**, Task 0).
  - Clients with a cache entry for Homewind re-validate it with unicast ARP
    (Linux NUD probe), which 802.11 retries; browsers kept working.
  - A first contact (or an entry in FAILED state) needs broadcast ARP. The
    Karoo and a /24 sweep both depend on it.
  - ARP failure on Linux/Android takes ~3 s (3 requests, 1 s apart), so an
    *immediate* `ConnectException` points to `ENETUNREACH` or
    `ECONNREFUSED` instead. The Karoo's errno log decides.

**Conclusion:** measure ARP first (Task 0). Independent of that, the client
must not depend on mDNS for every connect, and the firmware should provide a
stable identity and a correct DHCP name (2.2).

### 3. Weak device ID (existing, do not change now)

- `ESP.getEfuseMac() & 0xFFFFFFFF` (`Esp.cpp:544-547`, used in
  `WiFiServiceWML.cpp:134`, `WiFiManagerLite.cpp:215`) prints MAC bytes
  3,2,1,0. Bytes 0–2 are the Espressif OUI, so about 8 bits differ between
  boards: roughly 1/256 that two boards share hostname, mDNS name, AP SSID
  and BLE name.
- Changing the format would rename every existing device (router entries,
  saved Karoo devices, BLE pairings). Keep it; expose the full MAC as a
  separate ID instead (task 3).

## TODO

Target release: **2.2** for tasks 1–3 (the Karoo extension needs
`device_id` to tell two Homewinds apart and to detect an IP handed to
another device). Task 4 only if Task 0 confirms ARP loss.

### Task 0 — measure ARP (no code change)

- From a Mac in the same Wi-Fi, Homewind running with BLE (HR sensor + Zwift
  connected), no WebUI open anywhere:
  ```sh
  sudo arp -d <homewind-ip>
  ping -c 20 -i 0.5 <homewind-ip>
  ```
  10 runs, a few minutes apart. Record lost pings before the first reply
  and total loss. Repeat once with BLE idle (no scan, no links).
- Optional: `tcpdump -i en0 arp and host <homewind-ip>` to see requests
  without replies.
- Verdict: confirmed if first-reply loss correlates with BLE activity;
  otherwise refuted.

### Task 1 — DHCP hostname (Homewind only)

- In `App::initWiFi()` (WML branch), call `WiFi.setHostname()` with the same
  string as mDNS (`homewind-%08lx`, see `WiFiServiceWML.cpp:133-136`)
  **before** `WiFi.mode(WIFI_STA)`. Build the name in one helper and use it
  in both places.
- wifiMangerLite stays unchanged (no library version bump).
- Acceptance: after deleting the FRITZ!Box entry (device off) and
  reconnecting, the router shows `homewind-xxxxxxxx`;
  `ping homewind-xxxxxxxx.fritz.box` resolves. Same after AP fallback and
  after a router Wi-Fi off/on.
- Note for users: FRITZ!Box keeps a name the user renamed by hand; delete
  the entry to pick up the new DHCP name.

### Task 2 — mDNS started once, with identity

- Remove the `MDNS.end()` / `MDNS.begin()` pair in the `Connected` handler
  (`WiFiServiceWML.cpp:140-149`); wifiMangerLite owns the start (same
  lowercased name after the migration at `WiFiServiceWML.cpp:85-105`).
- Add TXT records to `_http._tcp`: `id=<12-hex MAC>`, `api=1`,
  `version=<fw>` (`MDNS.addServiceTxt`, after wifiMangerLite started mDNS).
- Add a second service `_homewind._tcp` on port 80 with the same TXT records
  (Karoo request 3, decided 2026-11-22), so clients browse for Homewind only.
  Add it in the same place as the TXT records, after every mDNS start. Keep it
  only if the heap measurement shows no measurable cost; otherwise drop it.
- No periodic re-announce: `mdns_netif_action()` allocates per call and only
  helps a browse that runs at that moment.
- Acceptance: `dns-sd -B _http._tcp`, `dns-sd -B _homewind._tcp` and
  `dns-sd -L` show the services and TXT records; from a Mac, Homewind is found
  10 of 10 times within 3 s with BLE sensors and an app connected and no
  browser open (Karoo request 3); mDNS answers again after router Wi-Fi
  off/on.

### Task 3 — identity in `GET /api/v1/system/info`

- Append `device_id=<12-hex MAC>` and `hostname=homewind-xxxxxxxx`
  (key=value text, `ApiActions.cpp:1228+`, 512 B buffer has room). Additive;
  the WebUI no longer reads this endpoint.
- Optional: whitelist `system/info` for maintenance mode
  (`ApiActions.cpp:60-73`) so clients can identify the device during OTA;
  today it returns 503.
- Update `src/web/ENDPOINT.md`, `docs/05_APIS.md`, `UiDemo/demo.html`.

### Task 4 — gratuitous ARP (only if Task 0 confirms)

- Every ~30 s while STA is connected: `etharp_gratuitous(netif)` via
  `tcpip_callback` (lwIP core lock). No allocation in the timer path.
- Limit: Linux/Android only **update** an existing cache entry from a
  gratuitous ARP (`arp_accept=0`), they do not create one. It helps clients
  that talked to Homewind before, not a first contact. Android's packet
  filter may drop it while the screen is off (**verify** on the Karoo).
- Acceptance: Task 0 repeated with the build, first-reply loss gone; Karoo
  "Test and save" 10 of 10 without WebUI open; heap within noise.

### Not to do

- No change to Wi-Fi power save, coexistence, or the BLE scan duty cycle
  (HR sensor reconnect time depends on it).
- No change to the 8-hex ID format.

## Impact (tasks 1–3; task 4 adds one broadcast frame per 30 s)

- **BLE:** none. The hostname only affects DHCP before NimBLE starts; TXT
  records cost a few bytes; one mDNS start per connect instead of two means
  less radio traffic.
- **Heap:** one fewer mDNS task free/alloc per connect (better for
  fragmentation). Measure per `HEAP_BENCHMARK.md` anyway.
- **Captive portal, AP fallback, OTA URLs:** unaffected.

## Tests

- Host: unit test for the hostname helper (format, lowercase, length ≤ 32)
  in `tests/`, same style as `tests/bike_metrics_test.cpp`.
- Build: `tools/build.sh` for `touch` and `basic`.
- Hardware: the acceptance steps of tasks 1–3, plus one ride-length run with
  HR + CSC + PWR + Zwift connected and the Karoo polling; heap within noise
  of the RAM gate (`AGENT_FEATURES.md`).

## Effort

| Part | Estimate |
|---|---|
| Task 0 ARP measurement | ~2–3 h |
| Task 1 hostname + helper test | ~2 h |
| Task 2 mDNS once + TXT + hardware checks | 0.5–1 day |
| Task 3 `system/info` fields + docs | ~1 h |
| Release chores (version sync, CHANGELOG, both builds, heap) | ~0.5 day |
| **Total (tasks 0–3)** | **~2.5–3 days** |
| Task 4 gratuitous ARP, if needed | +0.5 day |
