/**
 * @file HeapDiagnostics.cpp
 * @brief Heap baseline capture + diagnostic printing with marker-based tracking
 */

#include "HeapDiagnostics.h"

#if HW_ENABLE_SERIAL_COMMANDS

#include "DebugLog.h"
#include <esp_heap_caps.h>
#if HW_ENABLE_FANS
#include <SmartMiFanAsync.h>
#endif

namespace HeapDiagnostics {

// ============================================================
// Static Storage for Markers
// ============================================================
static HeapSnapshot s_snapshots[static_cast<uint8_t>(Marker::_COUNT)];
static uint32_t s_lastTickMs = 0;
static uint32_t s_lastTickFreeHeap = 0;

// ============================================================
// Marker Names (for printing)
// ============================================================
static const char* getMarkerName(Marker marker) {
  switch (marker) {
    case Marker::BOOT_RAW:       return "BOOT_RAW";
    case Marker::READY_BASELINE: return "READY_BASELINE";
    case Marker::BLE_READY:      return "BLE_READY";
    case Marker::FANS_CONNECTED: return "FANS_CONNECTED";
    case Marker::POST_CTRL_DONE: return "POST_CTRL_DONE";
    case Marker::FIRST_WS_CLIENT:return "FIRST_WS_CLIENT";
    case Marker::RUNTIME_TICK:   return "RUNTIME_TICK";
    default:                     return "UNKNOWN";
  }
}

// ============================================================
// Helper: Capture current heap state into a snapshot
// ============================================================
static void captureSnapshot(HeapSnapshot& snap) {
  snap.free8bit       = ESP.getFreeHeap();
  snap.largest8bit    = ESP.getMaxAllocHeap();
  snap.freeInternal   = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  snap.largestInternal= heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  snap.uptimeMs       = millis();
  snap.captured       = true;
}

// ============================================================
// Helper: Calculate fragmentation percentage
// ============================================================
static float calcFrag(uint32_t largest, uint32_t freeHeap) {
  if (freeHeap == 0) return 0.0f;
  return 100.0f - (100.0f * largest / freeHeap);
}

// ============================================================
// Helper: Format uptime as HH:MM:SS
// ============================================================
static void formatUptime(uint32_t ms, char* buf, size_t bufSize) {
  uint32_t sec = ms / 1000;
  uint32_t h = sec / 3600;
  uint32_t m = (sec % 3600) / 60;
  uint32_t s = sec % 60;
  snprintf(buf, bufSize, "%02lu:%02lu:%02lu", h, m, s);
}

// ============================================================
// Capture Functions
// ============================================================

void hwHeapDiagCaptureBoot() {
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::BOOT_RAW)];
  if (!snap.captured) {
    captureSnapshot(snap);
    char ts[12];
    formatUptime(snap.uptimeMs, ts, sizeof(ts));
    HW_DEBUG_PRINTF("[Heap] BOOT_RAW @%s: free=%u, largest=%u\n", 
                   ts, snap.free8bit, snap.largest8bit);
  }
}

void hwHeapDiagCaptureReady() {
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::READY_BASELINE)];
  if (!snap.captured) {
    captureSnapshot(snap);
    char ts[12];
    formatUptime(snap.uptimeMs, ts, sizeof(ts));
    HW_DEBUG_PRINTF("[Heap] READY_BASELINE @%s: free=%u, largest=%u (boot cost: %d)\n",
                   ts, snap.free8bit, snap.largest8bit,
                   (int)(s_snapshots[0].free8bit - snap.free8bit));
  }
}

void hwHeapDiagCaptureBleReady() {
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::BLE_READY)];
  if (!snap.captured) {
    captureSnapshot(snap);
    char ts[12];
    formatUptime(snap.uptimeMs, ts, sizeof(ts));
    const HeapSnapshot& ready = s_snapshots[static_cast<uint8_t>(Marker::READY_BASELINE)];
    int32_t delta = ready.captured ? (int32_t)(ready.free8bit - snap.free8bit) : 0;
    HW_DEBUG_PRINTF("[Heap] BLE_READY @%s: free=%u, largest=%u (BLE cost: %d)\n",
                   ts, snap.free8bit, snap.largest8bit, delta);
  }
}

void hwHeapDiagCaptureFansConnected() {
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)];
  if (!snap.captured) {
    captureSnapshot(snap);
    char ts[12];
    formatUptime(snap.uptimeMs, ts, sizeof(ts));
    const HeapSnapshot& ready = s_snapshots[static_cast<uint8_t>(Marker::READY_BASELINE)];
    int32_t delta = ready.captured ? (int32_t)(ready.free8bit - snap.free8bit) : 0;
    HW_DEBUG_PRINTF("[Heap] FANS_CONNECTED @%s: free=%u, largest=%u (bg-init cost: %d)\n",
                   ts, snap.free8bit, snap.largest8bit, delta);
    
    // Initialize runtime tick baseline
    s_lastTickMs = snap.uptimeMs;
    s_lastTickFreeHeap = snap.free8bit;
  }
}

void hwHeapDiagCapturePostCtrlDone() {
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::POST_CTRL_DONE)];
  if (!snap.captured) {
    captureSnapshot(snap);
    char ts[12];
    formatUptime(snap.uptimeMs, ts, sizeof(ts));
    const HeapSnapshot& fans = s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)];
    int32_t delta = fans.captured ? (int32_t)(fans.free8bit - snap.free8bit) : 0;
    HW_DEBUG_PRINTF("[Heap] POST_CTRL_DONE @%s: free=%u, largest=%u (ctrl cost: %d)\n",
                   ts, snap.free8bit, snap.largest8bit, delta);
  }
}

void hwHeapDiagCaptureFirstWs() {
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::FIRST_WS_CLIENT)];
  if (!snap.captured) {
    captureSnapshot(snap);
    char ts[12];
    formatUptime(snap.uptimeMs, ts, sizeof(ts));
    HW_DEBUG_PRINTF("[Heap] FIRST_WS_CLIENT @%s: free=%u, largest=%u\n",
                   ts, snap.free8bit, snap.largest8bit);
  }
}

// ============================================================
// Runtime Tick (continuous)
// ============================================================

bool hwHeapDiagRuntimeTick(const OperationalStatus* status) {
  uint32_t now = millis();
  
  // Only log if time threshold exceeded (simple 30s interval)
  uint32_t elapsed = now - s_lastTickMs;
  if (elapsed < HW_HEAP_DIAG_TICK_INTERVAL_MS) {
    return false;
  }
  
  // Capture current state into RUNTIME_TICK marker
  HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(Marker::RUNTIME_TICK)];
  captureSnapshot(snap);
  
  // Calculate drift from FANS_CONNECTED (or READY_BASELINE as fallback)
  const HeapSnapshot& baseline = s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)].captured
    ? s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)]
    : s_snapshots[static_cast<uint8_t>(Marker::READY_BASELINE)];
  
  int32_t drift = baseline.captured ? (int32_t)(baseline.free8bit - snap.free8bit) : 0;
  
  char uptimeBuf[12];
  formatUptime(snap.uptimeMs, uptimeBuf, sizeof(uptimeBuf));
  
  // Line 1: Heap info (with minFree to detect temporary spikes)
  uint32_t minFree = ESP.getMinFreeHeap();
  HW_DEBUG_PRINTF("[Heap] TICK %s: free=%u, largest=%u, frag=%.1f%%, drift=%d, minFree=%u\n",
                 uptimeBuf,
                 snap.free8bit, snap.largest8bit,
                 calcFrag(snap.largest8bit, snap.free8bit),
                 drift, minFree);
  
  // Line 2: Operational status (if provided)
  if (status) {
    HW_DEBUG_PRINTF("[Heap] STATUS: WS=%u, BLE=%u/%u, Scan=%s, Discovery=%u, Fans=%u/%u\n",
                   status->wsClientCount,
                   status->bleSensorsConnected, status->bleSensorsConfigured,
                   status->bleScanning ? "Y" : "N",
                   status->bleDiscoveryCount,
                   status->fansConnected, status->fansConfigured);
    
#if HW_ENABLE_FANS
    // Line 3+: Detailed fan status (if fans are configured)
    if (status->fansConfigured > 0) {
      size_t discoveredCount = 0;
      const SmartMiFanDiscoveredDevice* discovered = SmartMiFanAsync_getDiscoveredFans(discoveredCount);
      
      if (discoveredCount > 0) {
        for (size_t i = 0; i < discoveredCount && i < 4; i++) {
          FanParticipationState state = SmartMiFanAsync_getFanParticipationState(static_cast<uint8_t>(i));
          const char* stateStr = (state == FanParticipationState::ACTIVE) ? "ACTIVE" :
                                (state == FanParticipationState::INACTIVE) ? "INACTIVE" : "ERROR";
          
          const char* errorStr = "OK";
          if (discovered[i].lastError != MiioErr::OK) {
            switch (discovered[i].lastError) {
              case MiioErr::TIMEOUT: errorStr = "TIMEOUT"; break;
              case MiioErr::WRONG_SOURCE_IP: errorStr = "WRONG_IP"; break;
              case MiioErr::DECRYPT_FAIL: errorStr = "DECRYPT_FAIL"; break;
              case MiioErr::INVALID_RESPONSE: errorStr = "INVALID_RESP"; break;
              default: errorStr = "UNKNOWN"; break;
            }
          }
          
          // Print first 8 chars of token for identification
          char tokenPrefix[9] = {0};
          strncpy(tokenPrefix, discovered[i].token, 8);
          
          HW_DEBUG_PRINTF("[Heap] Fan[%zu]: ready=%s, state=%s, error=%s, token=%.8s\n",
                         i, discovered[i].ready ? "true" : "false", stateStr, errorStr, tokenPrefix);
        }
      } else {
        // No discovered fans, but configured fans exist
        HW_DEBUG_PRINTF("[Heap] Fan[?]: No discovered fans (configured=%u, discovered=0)\n",
                       status->fansConfigured);
      }
    }
#endif
  }
  
  // Update last tick state
  s_lastTickMs = now;
  s_lastTickFreeHeap = snap.free8bit;
  
  return true;
}

// ============================================================
// Query Functions
// ============================================================

bool hwHeapDiagIsMarkerCaptured(Marker marker) {
  if (static_cast<uint8_t>(marker) >= static_cast<uint8_t>(Marker::_COUNT)) {
    return false;
  }
  return s_snapshots[static_cast<uint8_t>(marker)].captured;
}

const HeapSnapshot* hwHeapDiagGetSnapshot(Marker marker) {
  if (static_cast<uint8_t>(marker) >= static_cast<uint8_t>(Marker::_COUNT)) {
    return nullptr;
  }
  return &s_snapshots[static_cast<uint8_t>(marker)];
}

// ============================================================
// Print Functions
// ============================================================

void hwHeapDiagPrintTick(const char* label) {
  uint32_t now = millis();
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t largestBlock = ESP.getMaxAllocHeap();
  uint32_t minFree = ESP.getMinFreeHeap();
  
  // Calculate drift from FANS_CONNECTED (or READY_BASELINE as fallback)
  const HeapSnapshot& baseline = s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)].captured
    ? s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)]
    : s_snapshots[static_cast<uint8_t>(Marker::READY_BASELINE)];
  
  int32_t drift = baseline.captured ? (int32_t)(baseline.free8bit - freeHeap) : 0;
  
  char uptimeBuf[12];
  formatUptime(now, uptimeBuf, sizeof(uptimeBuf));
  
  // Print TICK format (same as runtime tick)
  if (label) {
    HW_DEBUG_PRINTF("[Heap] TICK %s (%s): free=%u, largest=%u, frag=%.1f%%, drift=%d, minFree=%u\n",
                   uptimeBuf, label,
                   freeHeap, largestBlock,
                   calcFrag(largestBlock, freeHeap),
                   drift, minFree);
  } else {
    HW_DEBUG_PRINTF("[Heap] TICK %s: free=%u, largest=%u, frag=%.1f%%, drift=%d, minFree=%u\n",
                   uptimeBuf,
                   freeHeap, largestBlock,
                   calcFrag(largestBlock, freeHeap),
                   drift, minFree);
  }
}

void hwHeapDiagPrintMarker(Print& out, Marker marker, const char* label) {
  if (static_cast<uint8_t>(marker) >= static_cast<uint8_t>(Marker::_COUNT)) {
    return;
  }
  
  const HeapSnapshot& snap = s_snapshots[static_cast<uint8_t>(marker)];
  const char* name = label ? label : getMarkerName(marker);
  
  if (!snap.captured) {
    out.printf("%-15s (not yet captured)\n", name);
    return;
  }
  
  char uptimeBuf[12];
  formatUptime(snap.uptimeMs, uptimeBuf, sizeof(uptimeBuf));
  
  out.printf("%-15s %6u  %6u  %6u  %6u  %5.1f%%  %s\n",
            name,
            snap.free8bit, snap.largest8bit,
            snap.freeInternal, snap.largestInternal,
            calcFrag(snap.largest8bit, snap.free8bit),
            uptimeBuf);
}

void hwHeapDiagPrintHeapInfo(Print& out) {
  // Current values
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t largestBlock = ESP.getMaxAllocHeap();
  uint32_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  uint32_t largestInternal = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
  uint32_t minFreeHeap = ESP.getMinFreeHeap();
  uint32_t heapSize = ESP.getHeapSize();
  uint32_t uptimeMs = millis();
  
  char uptimeBuf[12];
  formatUptime(uptimeMs, uptimeBuf, sizeof(uptimeBuf));
  
  out.print(F("\n==================== HEAP DIAGNOSTICS ====================\n"));
  out.printf("Uptime: %s (%lu ms)\n\n", uptimeBuf, uptimeMs);
  
  // Header
  out.print(F("Marker           Free8   Lrg8   FreeInt  LrgInt  Frag%%   Time\n"));
  out.print(F("--------------------------------------------------------------\n"));
  
  // Current (not a stored marker, just live values)
  out.printf("%-15s %6u  %6u  %6u  %6u  %5.1f%%  NOW\n",
            "CURRENT",
            freeHeap, largestBlock,
            freeInternal, largestInternal,
            calcFrag(largestBlock, freeHeap));
  
  out.print(F("--------------------------------------------------------------\n"));
  
  // All markers in order
  hwHeapDiagPrintMarker(out, Marker::BOOT_RAW, nullptr);
  hwHeapDiagPrintMarker(out, Marker::READY_BASELINE, nullptr);
  hwHeapDiagPrintMarker(out, Marker::BLE_READY, nullptr);
  hwHeapDiagPrintMarker(out, Marker::FANS_CONNECTED, nullptr);
  hwHeapDiagPrintMarker(out, Marker::POST_CTRL_DONE, nullptr);
  hwHeapDiagPrintMarker(out, Marker::FIRST_WS_CLIENT, nullptr);
  
  // Runtime tick (if captured)
  if (s_snapshots[static_cast<uint8_t>(Marker::RUNTIME_TICK)].captured) {
    hwHeapDiagPrintMarker(out, Marker::RUNTIME_TICK, "LAST_TICK");
  }
  
  out.print(F("--------------------------------------------------------------\n\n"));
  
  // Delta Analysis
  out.print(F("=== DELTA ANALYSIS ===\n"));
  
  const HeapSnapshot& boot = s_snapshots[static_cast<uint8_t>(Marker::BOOT_RAW)];
  const HeapSnapshot& ready = s_snapshots[static_cast<uint8_t>(Marker::READY_BASELINE)];
  const HeapSnapshot& ble = s_snapshots[static_cast<uint8_t>(Marker::BLE_READY)];
  const HeapSnapshot& fans = s_snapshots[static_cast<uint8_t>(Marker::FANS_CONNECTED)];
  const HeapSnapshot& ctrl = s_snapshots[static_cast<uint8_t>(Marker::POST_CTRL_DONE)];
  
  // Boot cost: READY_BASELINE - BOOT_RAW
  if (boot.captured && ready.captured) {
    int32_t bootCost = (int32_t)boot.free8bit - (int32_t)ready.free8bit;
    float fragDelta = calcFrag(ready.largest8bit, ready.free8bit) - calcFrag(boot.largest8bit, boot.free8bit);
    out.printf("Boot cost:        %+6d bytes, frag delta: %+.1f%%\n", bootCost, fragDelta);
  }
  
  // BLE cost: BLE_READY - READY_BASELINE
  if (ready.captured && ble.captured) {
    int32_t bleCost = (int32_t)ready.free8bit - (int32_t)ble.free8bit;
    float fragDelta = calcFrag(ble.largest8bit, ble.free8bit) - calcFrag(ready.largest8bit, ready.free8bit);
    out.printf("BLE init cost:    %+6d bytes, frag delta: %+.1f%%\n", bleCost, fragDelta);
  }
  
  // Fans cost: FANS_CONNECTED - BLE_READY (or READY_BASELINE)
  if (fans.captured) {
    const HeapSnapshot& base = ble.captured ? ble : ready;
    if (base.captured) {
      int32_t fansCost = (int32_t)base.free8bit - (int32_t)fans.free8bit;
      float fragDelta = calcFrag(fans.largest8bit, fans.free8bit) - calcFrag(base.largest8bit, base.free8bit);
      out.printf("Fans init cost:   %+6d bytes, frag delta: %+.1f%%\n", fansCost, fragDelta);
    }
  }
  
  // Post-control cost: POST_CTRL_DONE - FANS_CONNECTED
  if (fans.captured && ctrl.captured) {
    int32_t ctrlCost = (int32_t)fans.free8bit - (int32_t)ctrl.free8bit;
    float fragDelta = calcFrag(ctrl.largest8bit, ctrl.free8bit) - calcFrag(fans.largest8bit, fans.free8bit);
    out.printf("Post-ctrl cost:   %+6d bytes, frag delta: %+.1f%%\n", ctrlCost, fragDelta);
  }
  
  // Background init total: FANS_CONNECTED - READY_BASELINE
  if (ready.captured && fans.captured) {
    int32_t bgTotal = (int32_t)ready.free8bit - (int32_t)fans.free8bit;
    float fragDelta = calcFrag(fans.largest8bit, fans.free8bit) - calcFrag(ready.largest8bit, ready.free8bit);
    out.printf("Bg-init total:    %+6d bytes, frag delta: %+.1f%%\n", bgTotal, fragDelta);
  }
  
  // Runtime drift: CURRENT - FANS_CONNECTED (or POST_CTRL_DONE)
  const HeapSnapshot& driftBase = ctrl.captured ? ctrl : (fans.captured ? fans : ready);
  if (driftBase.captured) {
    int32_t drift = (int32_t)driftBase.free8bit - (int32_t)freeHeap;
    float fragNow = calcFrag(largestBlock, freeHeap);
    float fragBase = calcFrag(driftBase.largest8bit, driftBase.free8bit);
    out.printf("Runtime drift:    %+6d bytes, frag delta: %+.1f%%\n", drift, fragNow - fragBase);
  }
  
  out.print(F("\n"));
  
  // System stats
  out.print(F("=== SYSTEM STATS ===\n"));
  out.printf("Heap size:        %u bytes\n", heapSize);
  out.printf("Min free ever:    %u bytes\n", minFreeHeap);
  out.printf("Current frag:     %.1f%% (8-bit), %.1f%% (internal)\n",
            calcFrag(largestBlock, freeHeap),
            calcFrag(largestInternal, freeInternal));
  
  out.print(F("===========================================================\n\n"));
}

} // namespace HeapDiagnostics

#endif // HW_ENABLE_SERIAL_COMMANDS
