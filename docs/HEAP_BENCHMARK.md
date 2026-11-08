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
| 2026-11-08 | 1.7.0 + HomeWindWSAmoled 1.6.1 | diagnostic | touch | steady~88min | 47804 | 31732 | 34 | HR+CSC+PWR, Zwift, WebUI on 2 clients; range 47.8–52.8 KB; min since boot 38944; DMA min 31184 |
| 2026-11-08 | 1.7.0 + HomeWindWSAmoled 1.6.1 | diagnostic | touch | boot~2min | 52812 | 31732 | 40 | same run |
| 2026-11-07 | 1.7.0 | 20261107-201127 | basic | steady~29min | 86056 | 40948 | 52 | all sensors; range 82.8–86.1 KB |
| 2026-11-07 | 1.7.0 | 20261107-201127 | touch | steady~85min | 17280 | 7668 | 56 | HR+CSC+PWR, Zwift, WebUI; range 14.8–17.9 KB |
| 2026-11-01 | 1.5.0 | 20261101-161235 | touch | steady~78min | 22884 | 9204 | 60 | before reboot; WS connected |
| 2026-11-01 | 1.5.0 | 20261101-161235 | touch | boot~37s | 19988 | 10740 | 46 | after reboot; WS connected |
