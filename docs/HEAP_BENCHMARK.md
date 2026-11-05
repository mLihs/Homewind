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
| 2026-11-01 | 1.5.0 | 20261101-161235 | touch | steady~78min | 22884 | 9204 | 60 | before reboot; WS connected |
| 2026-11-01 | 1.5.0 | 20261101-161235 | touch | boot~37s | 19988 | 10740 | 46 | after reboot; WS connected |
