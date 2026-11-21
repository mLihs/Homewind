# Heap Benchmark

Manual tracking of internal heap after WebUI connect. Same method across versions.

## How to measure

1. Flash the build under test (Touch or Basic).
2. Reboot the device; open the WebUI; wait until WebSocket is connected.
3. In the browser console:

```js
ApiV1.getState().system
// → { uptimeMs, freeHeap, largestFreeBlock }
```

4. Record two phases when possible:
   - `boot~30s` — shortly after reboot (`uptimeMs` ≈ 30 000)
   - `steady~1h` — after ~1 h runtime under normal use
5. Fragmentation: `frag% = round(100 * (1 - largestFreeBlock / freeHeap))`
6. Add a row below (newest on top).

Values are ESP internal heap (`ESP.getFreeHeap` / largest free block), not PSRAM.

## Log

| date | version | build | board | phase | freeHeap | largestFreeBlock | frag% | notes |
|------|---------|-------|-------|-------|----------|------------------|-------|-------|
| 2026-11-21 | 2.1.0 (Touch UI 1.7.0, APP badge reason) | release touch 20261121-151156 | touch | idle ~10 min + WebUI stress | 45540 | 31732 | 30 | Release build, so values from `GET /api/v1/system/info` sampled every 0.2 s (no min since boot without serial commands). Trainer mirror ready, no app connected. Stress: 5 rounds of 3 parallel index loads (gzip 31 KB) + 3 parallel `telemetry/refresh`: lowest sample 34.7 KB free, largest block 23.5 KB; back to 45.5 / 31.7 KB after each round (no leak). Short transients between samples not covered |
| 2026-11-21 | 2.1.0 (Touch UI 1.7.0, APP badge reason) | release touch 20261121-151156 | touch | MyWhoosh connected via mirror, sensors on, standing + WebUI stress | 42900 | 31732 | 26 | Same method and stress as the row above, `app_connected:true` before and after. Idle with app 42.9 KB (about 2.6 KB less than without app). Lowest sample 32.3 KB free, largest block 18.4 KB (only 2.4 KB above G7 gate); back to 42.9 / 31.7 KB afterwards (no leak) |
| 2026-11-20 | same + Nagle on WS + WebUI load cleanup + reset report | diagnostic touch --trainer (log touch_fix_20261120-184128) | touch | WebUI loads: laptop reload, iPhone private tab (rejected twice as 3rd client, then connected), Zwift on the mirror | 41948 | 30708 | 27 | `telemetry/refresh` now costs 1–2 KB (before up to ~20 KB per client); app.js transfer ~5 KB. Min since boot 28.9 KB, lowest largest-block sample 23.5 KB; mirror + app connected, dropped=0, no restart |
| 2026-11-20 | same + HTTP request logging | diagnostic basic --trainer (logs xiao_http_20261120-091846, xiao_nagle_20261120-092603) | basic (XIAO, no PSRAM) | WebUI load and `telemetry/refresh` cost, 2 WS clients, trainer mirror on | 49500 (idle) | 31732 | 36 | Assets are cheap (app.css/app.js ~10 KB transient). `telemetry/refresh` (with or without `client_id`, 3× each via curl) dropped free 49.5 → 6.2 / 35.7 / 45.2 / 19.3 / 11.6 KB within 17 ms (min since boot 312 B); `sensor/server/resume` costs nothing. Cause: Nagle off on every AsyncWebServer connection → each small WS frame its own TCP segment. With `setNoDelay(false)` on WS connections the same test cost 2–12 KB (min 30.5 KB). Remaining: an iPhone cold load with up to 12 parallel requests (incl. apple-touch-icon ×2, favicon) still reached 2.0 KB once |
| 2026-11-20 | 1.8.2 + M2a part 2 + UX + APP_STATUS + WS watchdog | diagnostic basic --trainer (log xiao_trainer2_20261120-081903) | basic (XIAO, no PSRAM) | ~20 min ride, HR + NEO mirror + Assioma + Zwift on the mirror | 53798 (avg) | 31732 | 41 | Trainer runtime in internal RAM. Steady 54–56 KB free, largest block 31.7 KB. Boot with mirror + Zwift reconnect: min 53.6 KB. **WebUI is the risk:** cold load (private window) min free 2.4 KB, phone cold load right after 0.98 KB; a second WebUI during the ride: free 20 KB, largest block 3.9 KB, min since boot 792 B; laptop stopped acking (09:00): queue 0→13 in 8 s, free 56→28 KB, held until the laptop reconnected. No restart, no host reset, ERG down=744, dropped=6 (at the Assioma connect). Not release-ready until page serving is cheaper |
| 2026-11-20 | 1.8.2 + M2a part 2 + UX + APP_STATUS | diagnostic touch --trainer (log m2a_g7_20261119-193737) | touch | idle overnight, uptime 10:51 | 42472 | 29684 | 30 | After G7, no WebUI use: free 42.0–43.0 KB, largest block unchanged at 29.7 KB all night, min since boot unchanged 30260; mirror + app connected all night (up=348521, dropped=25); sensors asleep (Assioma, TICKR off the body), scans=991, no restart |
| 2026-11-19 | 1.8.2 + M2a part 2 + UX + APP_STATUS | diagnostic touch --trainer (log m2a_g7_20261119-193737) | touch | ride~61min | 41524 | 29684 | 28 | G7: HR + NEO 2T mirror + power meter, fan, Zwift Apple TV on the mirror, WebUI on one device; range 41.2–43.3 KB; largest block 31.7 → 29.7 KB at ~20:50; min since boot 36276 at ride start (after setup tests), 30260 at ~21:15; no sensor/trainer link loss, no restart; mirror up=40559 down=2421 dropped=7 |
| 2026-11-19 | 1.8.2 + M2a part 2 + UX | diagnostic touch --trainer (log m2a_ux_20261119-082830) | touch | idle~10h | 40668 | 22516 | 45 | Idle with mirror + app connected, WebUI opened from several devices during the day; largest block 31.7 → 24.6 (~15:40) → 22.5 KB (~16:50), min since boot 29672; free heap steady 40.7–44.8 KB (no leak). Controlled test 20:00: opening the WebUI on 2–3 devices repeatedly dropped min since boot 42044 → 29684 (transient), largest block 31.7 → 30.7 KB |
| 2026-11-18 | 1.8.2 + M2a part 1 | diagnostic 20261118-220448-trainer | touch | ride~16min | 41588 | 30708 | 26 | M2a smoke test, trainer mirror on (Trainer in PSRAM): HR + NEO 2T mirror + power meter, fan, Zwift Apple TV, WebUI; range 41.1–41.6 KB during the ride; min since boot 35260 |
| 2026-11-18 | 1.8.2 | diagnostic 20261118-201858 | touch | ride~31min | 50640 | 31732 | 37 | M2a D4 baseline (no mirror): HR + power meter, fan, Zwift Apple TV, WebUI; range 49.96–50.64 KB; min since boot 42432 (at first WebUI connect, before the ride) |
| 2026-11-18 | 1.8.2 | diagnostic 20261117-221836 | touch | ride~28min | 50528 | 31732 | 37 | G1 baseline (trainer mirror): HR, Zwift Apple TV, WebUI; range 50.1–50.5 KB; min since boot 44464 (from 6 min); DMA min 36704; serial `t` hung the loop at 28 min (diagnostic only) |
| 2026-11-08 | 1.7.0 + HomeWindWSAmoled 1.6.1 | diagnostic | touch | steady~88min | 47804 | 31732 | 34 | HR+CSC+PWR, Zwift, WebUI on 2 clients; range 47.8–52.8 KB; min since boot 38944; DMA min 31184 |
| 2026-11-08 | 1.7.0 + HomeWindWSAmoled 1.6.1 | diagnostic | touch | boot~2min | 52812 | 31732 | 40 | same run |
| 2026-11-07 | 1.7.0 | 20261107-201127 | basic | steady~29min | 86056 | 40948 | 52 | all sensors; range 82.8–86.1 KB |
| 2026-11-07 | 1.7.0 | 20261107-201127 | touch | steady~85min | 17280 | 7668 | 56 | HR+CSC+PWR, Zwift, WebUI; range 14.8–17.9 KB |
| 2026-11-01 | 1.5.0 | 20261101-161235 | touch | steady~78min | 22884 | 9204 | 60 | before reboot; WS connected |
| 2026-11-01 | 1.5.0 | 20261101-161235 | touch | boot~37s | 19988 | 10740 | 46 | after reboot; WS connected |
