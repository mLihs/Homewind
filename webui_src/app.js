(function () {
  "use strict";
  
  // WebUI Build Info (for debugging via console)
  // Build hash is read from HTML data attribute (replaced by build_webui.py)
  const WEBUI_BUILD = document.documentElement.dataset.webuiBuild || "dev";
  // Build info logging disabled in production

  // WS frame types (WEBSOCKET_PROTOCOL.md)
  const FRAME = {
    SYSTEM_STATUS: 0x01,
    SENSORS_SNAPSHOT: 0x02,
    SENSOR_CONN_STATE: 0x03,
    DISCOVERY_STATUS: 0x04,
    DISCOVERY_RESULTS: 0x05,
    HEART_RATE: 0x06,
    FANS_SNAPSHOT: 0x07,
    FIRMWARE_VERSION: 0x08,
    FIRMWARE_PROGRESS: 0x09,
    FIRMWARE_SEARCH_RESULT: 0x0A,
    HEART_RATE_SETTINGS: 0x0B,
    CLIENT_ID: 0x0C, // Control: [0x0C][clientId:uint32 LE]
  };

  const SENSOR_TYPE_REV = { 0: "HR", 1: "CSC" };

  // Timing constants (global for use across modules)
  window.TIMING = window.TIMING || {
    INPUT_DEBOUNCE_MS: 150,           // Input validation debounce delay
    SCROLL_DEBOUNCE_MS: 80,           // Scroll event debounce delay
    RECONNECT_BASE_MS: 1000,          // Base delay for WebSocket reconnection
    RECONNECT_MAX_MS: 30000,          // Maximum delay for WebSocket reconnection
    REBOOT_POLL_INTERVAL_MS: 2000,    // Reboot monitoring poll interval
    REBOOT_INITIAL_DELAY_MS: 3000,    // Initial delay before first reboot check
    FIRMWARE_STATUS_POLL_MS: 2500,    // Firmware status polling interval
    DISCOVERY_TIMEOUT_MS: 2000,       // Discovery timeout fallback
  };
  const TIMING = window.TIMING; // Local reference for this scope

  // ---- in-memory state (for adapters / reconnect safety) ----
  const state = {
    system: { uptimeMs: 0, freeHeap: 0, largestFreeBlock: 0 },
    sensors: new Map(), // name -> { id,name,type,mac,connected,battery }
    fans: new Map(),    // id(8) -> { id,token,connected,controlActive }
    heartRate: 0,
    firmwareVersion: "",
    firmwareProgress: 0,
    discovery: { active: false, type: null, results: [] },
  };

  // ---- DOM helpers (tiny, no framework) ----
  function $(id) { return document.getElementById(id); }

  // ---- HTTP Actions (POST + text/plain) ----
  async function httpPost(path, params = {}) {
    const url = new URL(path, location.origin);
    Object.entries(params).forEach(([k, v]) => {
      if (v !== undefined && v !== null) url.searchParams.set(k, String(v));
    });

    try {
      const resp = await fetch(url.toString(), { method: "POST" });
      const text = (await resp.text()).trim();

      // Normalize Homewind responses
      if (text === "OK") return { ok: true };
      if (text === "BUSY" || text === "NOT_READY") return { ok: false, error: text };
      if (text.startsWith("ERR")) return { ok: false, error: text };
      if (!resp.ok) return { ok: false, error: text || ("HTTP " + resp.status) };
      return { ok: true, message: text };
    } catch (e) {
      return { ok: false, error: (e && e.message) ? e.message : "Network error" };
    }
  }

  // Helper: try to resolve full token from fan id (first 8 chars) via DOM
  function resolveFanTokenById(id8) {
    const card = document.querySelector(`[data-fan-id="${id8}"]`);
    if (!card) return null;
    const token = card.getAttribute("data-token");
    return token || null;
  }

  // ---- Binary parsing (little-endian) ----
  const td = new TextDecoder("utf-8");
  function u32(dv, o) { return dv.getUint32(o, true); }
  function u16(dv, o) { return dv.getUint16(o, true); }

  function readLPString(u8, off) {
    if (off >= u8.length) return { str: "", next: off, ok: false };
    const len = u8[off]; off += 1;
    if (len === 0) return { str: "", next: off, ok: true };
    if (off + len > u8.length) return { str: "", next: off, ok: false };
    const str = td.decode(u8.subarray(off, off + len));
    return { str, next: off + len, ok: true };
  }

  // ---- UI adapters (call existing modules; DO NOT rebuild UI) ----
  function applyHeartRate(bpm) {
    state.heartRate = bpm;

    // existing UI element (index.html)
    const el = $("hr-value");
    if (el) el.textContent = String(bpm);

    // keep your existing manager logic intact
    if (window.HeartRateManager && typeof window.HeartRateManager.updateHeartRate === "function") {
      window.HeartRateManager.updateHeartRate(bpm);
    }
    if (typeof window.onHeartRateChanged === "function") window.onHeartRateChanged(bpm);
  }

  function applyFirmwareVersion(v) {
    state.firmwareVersion = v;
    const el = $("firmware-version");
    if (el) el.textContent = "Firmware: " + v;
  }

  function applyFirmwareProgress(pct) {
    state.firmwareProgress = pct;
    // keep existing FirmwareUpdate flow if present
    if (window.FirmwareUpdate && typeof window.FirmwareUpdate.handleDownloadProgress === "function") {
      window.FirmwareUpdate.handleDownloadProgress(pct);
    }
  }

  function applyFirmwareSearchResult(result) {
    // Call FirmwareUpdate module directly (no intermediate callback)
    if (window.FirmwareUpdate && typeof window.FirmwareUpdate.handleSearchResult === "function") {
      window.FirmwareUpdate.handleSearchResult(result);
    }
  }

  function applySensorsSnapshot(list) {
    state.sensors.clear();
    list.forEach((s) => state.sensors.set(s.id, s));

    // Drive existing sensor UI (BLESensorManager owns rendering/modals)
    // FIX: Use batch update to prevent UI flicker from multiple rapid re-renders
    if (window.BLESensorManager) {
      // Begin batch - suppress re-renders during loop
      if (typeof window.BLESensorManager.beginBatchUpdate === "function") {
        window.BLESensorManager.beginBatchUpdate();
      }
      
      if (typeof window.BLESensorManager.updateFromApi === "function") {
        list.forEach((s) => window.BLESensorManager.updateFromApi(s.id, {
          id: s.id,
          Name: s.name,
          Type: s.type,
          MAC: s.mac,
          connected: s.connected,
          battery: s.battery,
        }));
      }
      
      // End batch - single re-render now
      if (typeof window.BLESensorManager.endBatchUpdate === "function") {
        window.BLESensorManager.endBatchUpdate();
      }
    }

    // Update HR widget based on sensor state
    updateHrWidgetFromSensors(list);
  }
  
  function updateHrWidgetFromSensors(sensorList) {
    const hrSensor = sensorList.find((x) => x.type === "HR");
    const hrValueEl = $("hr-value");
    const hrNameEl = $("hr-sensor-name");
    
    if (!hrSensor) {
      // No HR sensor configured
      if (hrNameEl) hrNameEl.textContent = "No HR Sensor";
      if (hrValueEl && hrValueEl.textContent === "--") {
        // Keep placeholder if no value received yet
      }
    } else if (!hrSensor.connected) {
      // HR sensor configured but not connected
      if (hrNameEl) hrNameEl.textContent = hrSensor.name + " (Offline)";
      if (hrValueEl) hrValueEl.textContent = "--";
    } else {
      // HR sensor connected
      if (hrNameEl) hrNameEl.textContent = hrSensor.name;
      if (window.HeartRateManager && typeof window.HeartRateManager.updateSensorName === "function") {
        window.HeartRateManager.updateSensorName(hrSensor.name);
      }
    }
  }

  function applySensorConnDelta(name, connected) {
    const s = state.sensors.get(name);
    if (s) s.connected = connected;

    if (window.BLESensorManager && typeof window.BLESensorManager.updateFromApi === "function") {
      window.BLESensorManager.updateFromApi(name, { id: name, connected });
    }
    
    // Update HR widget if this is the HR sensor
    if (s && s.type === "HR") {
      updateHrWidgetFromSensors(Array.from(state.sensors.values()));
    }
  }

  function applyFansSnapshot(list) {
    state.fans.clear();
    list.forEach((f) => state.fans.set(f.id, f));

    // Restore fans into existing FanManager UI if needed
    // FIX: restoreFans already sets participationState, so we return early to avoid double-update flicker
    if (window.fanManager && typeof window.fanManager.restoreFans === "function") {
      const restore = list.map((f) => {
        const participationState = (!f.connected) ? "ERROR" : (f.controlActive ? "ACTIVE" : "INACTIVE");
        return { token: f.token, participationState };
      });
      // suppress callbacks on restore (matches existing semantics in app.js)
      window.fanManager.restoreFans(restore, true);

      // Apply recovery UI state (tooltip/disabled) without re-rendering
      if (typeof window.fanManager.setFanRecoveryState === "function") {
        list.forEach((f) => window.fanManager.setFanRecoveryState(f.token, f.recovery || 0));
      }
      return; // EXIT - restoreFans already handles status, no need for setFanStatus loop
    }

    // Fallback: Only if restoreFans doesn't exist, use setFanStatus or updateFromApi
    if (window.fanManager && typeof window.fanManager.setFanStatus === "function") {
      list.forEach((f) => {
        const participationState = (!f.connected) ? "ERROR" : (f.controlActive ? "ACTIVE" : "INACTIVE");
        window.fanManager.setFanStatus(f.token, participationState);
        if (typeof window.fanManager.setFanRecoveryState === "function") {
          window.fanManager.setFanRecoveryState(f.token, f.recovery || 0);
        }
      });
    } else if (window.fanManager && typeof window.fanManager.updateFromApi === "function") {
      // fallback adapter if setFanStatus isn't present
      list.forEach((f) => {
        window.fanManager.updateFromApi(f.id, {
          id: f.id,
          token: f.token,
          connected: f.connected,
          controlActive: f.controlActive,
        });
        if (typeof window.fanManager.setFanRecoveryState === "function") {
          window.fanManager.setFanRecoveryState(f.token, f.recovery || 0);
        }
      });
    }
  }

  function applyDiscoveryResults(results) {
    state.discovery.results = results;
    // Existing sensor manager consumes discovery results
    // New format: only index + name (backend resolves full data from cache)
    if (window.BLESensorManager && typeof window.BLESensorManager.handleDiscoveryResult === "function") {
      window.BLESensorManager.handleDiscoveryResult(results.map((r) => ({
        index: r.index,      // Discovery cache index
        Name: r.name,        // Display name only
      })));
    }
  }

  // ---- WS parsing ----
  function handleFrame(u8) {
    if (!u8 || u8.length < 1) return;
    const type = u8[0];
    const dv = new DataView(u8.buffer, u8.byteOffset, u8.byteLength);
    let off = 1;

    switch (type) {
      case FRAME.CLIENT_ID: {
        if (u8.length < off + 4) return;
        wsClientId = u32(dv, off);
        // Now that we know our WS client id, request a targeted refresh.
        // Server will unicast heavy snapshots (SENSORS/FANS) to this client.
        if (typeof window.requestTelemetryRefresh === "function") {
          setTimeout(() => {
            try { window.requestTelemetryRefresh(true); } catch (_) {}
          }, 50);
        }
        return;
      }
      case FRAME.SYSTEM_STATUS: {
        if (u8.length < off + 12) return;
        state.system.uptimeMs = u32(dv, off + 0);
        state.system.freeHeap = u32(dv, off + 4);
        state.system.largestFreeBlock = u32(dv, off + 8);
        return;
      }

      case FRAME.HEART_RATE: {
        if (u8.length < off + 2) return;
        const bpm = u16(dv, off);
        applyHeartRate(bpm);
        return;
      }

      case FRAME.HEART_RATE_SETTINGS: {
        if (u8.length < off + 4) return;
        const min = u16(dv, off);
        const max = u16(dv, off + 2);
        // Update UI elements
        const hrMinDisplay = document.getElementById("hr-min-value");
        const hrMaxDisplay = document.getElementById("hr-max-value");
        if (hrMinDisplay) hrMinDisplay.textContent = min;
        if (hrMaxDisplay) hrMaxDisplay.textContent = max;
        return;
      }

      case FRAME.FIRMWARE_VERSION: {
        const r = readLPString(u8, off);
        if (!r.ok) return;
        applyFirmwareVersion(r.str);
        return;
      }

      case FRAME.FIRMWARE_PROGRESS: {
        if (u8.length < off + 1) return;
        applyFirmwareProgress(u8[off]);
        return;
      }

      case FRAME.SENSORS_SNAPSHOT: {
        // Fixed-size format: [count:uint8][sensor1:84][sensor2:84][sensor3:84]
        // Sensor: [name:64][type:1][mac:17][connected:1][battery:1]
        if (u8.length < off + 1) return;
        const count = u8[off]; off += 1;
        const list = [];
        const SENSOR_ENTRY_SIZE = 84;
        
        for (let i = 0; i < count && i < 3; i++) {
          if (off + SENSOR_ENTRY_SIZE > u8.length) break;
          
          // Read name (fixed 64 bytes, null-terminated)
          const nameBytes = u8.subarray(off, off + 64);
          const nameNullIdx = nameBytes.indexOf(0);
          let name = "";
          try {
            name = (nameNullIdx >= 0) 
              ? td.decode(nameBytes.subarray(0, nameNullIdx))
              : td.decode(nameBytes);
          } catch (e) {
            name = "";  // Fallback on decode error
          }
          off += 64;
          
          // Read type (1 byte)
          const t = u8[off]; off += 1;
          
          // Read MAC (fixed 17 bytes, null-terminated)
          const macBytes = u8.subarray(off, off + 17);
          const macNullIdx = macBytes.indexOf(0);
          let mac = "";
          try {
            mac = (macNullIdx >= 0)
              ? td.decode(macBytes.subarray(0, macNullIdx))
              : td.decode(macBytes);
          } catch (e) {
            mac = "";  // Fallback on decode error
          }
          off += 17;
          
          // Read connected (1 byte)
          const connected = u8[off] !== 0; off += 1;
          
          // Read battery (1 byte)
          const batteryRaw = u8[off]; off += 1;

          list.push({
            id: name,
            name: name,
            type: SENSOR_TYPE_REV[t] || "HR",
            mac: mac,
            connected,
            battery: (batteryRaw === 255) ? null : batteryRaw,
          });
        }
        applySensorsSnapshot(list);
        return;
      }

      case FRAME.SENSOR_CONN_STATE: {
        const nameR = readLPString(u8, off); if (!nameR.ok) return; off = nameR.next;
        if (off >= u8.length) return;
        const connected = u8[off] !== 0;
        applySensorConnDelta(nameR.str, connected);
        return;
      }

      case FRAME.FANS_SNAPSHOT: {
        // Fixed-size format: [count:uint8][fan1:35][fan2:35][fan3:35][fan4:35]
        // Fan: [token:32][connected:1][controlActive:1][recovery:1]
        // recovery: 0=normal, 1=recovering (auto), 2=exhausted (needs user action)
        if (u8.length < off + 1) return;
        const count = u8[off]; off += 1;
        const list = [];
        const FAN_ENTRY_SIZE = 35;
        
        for (let i = 0; i < count && i < 4; i++) {
          if (off + FAN_ENTRY_SIZE > u8.length) break;
          
          // Read token (fixed 32 bytes, null-terminated)
          const tokenBytes = u8.subarray(off, off + 32);
          const tokenNullIdx = tokenBytes.indexOf(0);
          let token = "";
          try {
            token = (tokenNullIdx >= 0)
              ? td.decode(tokenBytes.subarray(0, tokenNullIdx))
              : td.decode(tokenBytes);
          } catch (e) {
            token = "";  // Fallback on decode error
          }
          off += 32;
          
          // Read connected (1 byte)
          const connected = u8[off] !== 0; off += 1;
          
          // Read controlActive (1 byte)
          const controlActive = u8[off] !== 0; off += 1;

          // Read recovery (1 byte)
          const recovery = u8[off] | 0; off += 1;

          list.push({ id: token.substring(0, 8), token, connected, controlActive, recovery });
        }
        applyFansSnapshot(list);
        return;
      }

      case FRAME.DISCOVERY_RESULTS: {
        if (u8.length < off + 1) return;
        const count = u8[off]; off += 1;
        const results = [];
        for (let i = 0; i < count; i++) {
          if (off >= u8.length) break;
          // Read index (discovery cache index)
          const index = u8[off]; off += 1;
          // Read name (length-prefixed string)
          const nameR = readLPString(u8, off); if (!nameR.ok) break; off = nameR.next;
          
          // Store only index + name (backend resolves full data from cache)
          results.push({ index: index, name: nameR.str });
        }
        applyDiscoveryResults(results);
        return;
      }

      case FRAME.DISCOVERY_STATUS: {
        // optional adapter (currently no dedicated UI hook in app.js)
        if (u8.length < off + 2) return;
        state.discovery.active = u8[off] !== 0; off += 1;
        state.discovery.type = SENSOR_TYPE_REV[u8[off]] || null;
        return;
      }

      case FRAME.FIRMWARE_SEARCH_RESULT: {
        // Format: [has_update:uint8][version_len:uint8][version:bytes][notes_len:uint16][notes:bytes]
        // Note: version and notes only present if has_update=1
        if (u8.length < off + 1) return;
        const hasUpdate = u8[off] !== 0; off += 1;
        
        const result = { hasUpdate: hasUpdate };
        
        if (hasUpdate) {
          // Read version (length-prefixed string)
          const versionR = readLPString(u8, off);
          if (!versionR.ok) return;
          off = versionR.next;
          result.latestVersion = versionR.str;
          
          // Read release notes (uint16 length-prefixed string)
          if (off + 2 > u8.length) return;
          const notesLen = u16(dv, off);
          off += 2;
          if (off + notesLen > u8.length) return;
          if (notesLen > 0) {
            result.releaseNotes = td.decode(u8.subarray(off, off + notesLen));
          } else {
            result.releaseNotes = "";
          }
        }
        
        applyFirmwareSearchResult(result);
        return;
      }

      default:
        // ignore unknown frames (production-safe)
        return;
    }
  }

  // ---- WS connection management ----
  let ws = null;
  let wsClientId = 0;
  let reconnectTimer = null;
  let reconnectAttempt = 0;

  function ensureIndicator() {
    let el = document.getElementById("ws-connection-indicator");
    if (el) return el;
    el = document.createElement("div");
    el.id = "ws-connection-indicator";
    el.style.cssText =
      "position:fixed;top:8px;right:8px;padding:4px 8px;font-size:11px;background:rgba(0,0,0,0.7);color:#fff;border-radius:4px;z-index:10000;";
    document.body.appendChild(el);
    return el;
  }

  function setIndicator(connected) {
    const el = ensureIndicator();
    if (connected) {
      el.textContent = "Connected";
      el.style.background = "rgba(0,165,84,0.8)";
    } else {
      el.textContent = "Disconnected";
      el.style.background = "rgba(183,183,183,0.8)";
    }
  }

  function scheduleReconnect() {
    if (reconnectTimer) return;
    const delay = Math.min(
      TIMING.RECONNECT_BASE_MS * Math.pow(2, reconnectAttempt),
      TIMING.RECONNECT_MAX_MS
    );
    reconnectAttempt++;
    reconnectTimer = setTimeout(() => {
      reconnectTimer = null;
      connectWS();
    }, delay);
  }

  function connectWS() {
    try {
      // Prevent reconnect-storms: don't connect if reconnect is already scheduled
      if (reconnectTimer) {
        return;
      }
      
      if (ws && (ws.readyState === WebSocket.OPEN || ws.readyState === WebSocket.CONNECTING || ws.readyState === WebSocket.CLOSING)) return;

      const proto = location.protocol === "https:" ? "wss:" : "ws:";
      const url = `${proto}//${location.host}/ws`;

      ws = new WebSocket(url);
      ws.binaryType = "arraybuffer";

      ws.onopen = () => {
        reconnectAttempt = 0;
        setIndicator(true);
        wsClientId = 0;
        if (window.requestTelemetryRefresh) {
          // Don't force a full refresh on every (re)connect:
          // - During cold loads (no browser cache) the server is already busy serving assets.
          // - Forcing heavy snapshot broadcasts can collapse the largest free block.
          // Server-side, /telemetry/refresh is also guarded to only send heavy snapshots for <=1 client.
          setTimeout(() => {
            try { window.requestTelemetryRefresh(false); } catch (_) {}
          }, 250);
        }
      };

      ws.onmessage = (ev) => {
        if (!(ev.data instanceof ArrayBuffer)) return;
        const u8 = new Uint8Array(ev.data);
        handleFrame(u8);
      };

      ws.onerror = () => {
        setIndicator(false);
      };

      ws.onclose = (event) => {
        setIndicator(false);
        
        // Check if connection was rejected due to max clients limit
        // Close code 1008 = Policy Violation (sent by server when max clients reached)
        if (event.code === 1008 && event.reason === "MAX_CLIENTS_REACHED") {
          const maxClients = 2; // Should match HW_WEBSOCKET_MAX_CLIENTS in Config.h
          if (typeof window.showErrorModal === "function") {
            window.showErrorModal({
              message: `Maximale Anzahl von ${maxClients} Clients erreicht.`,
              error: "Bitte trennen Sie eine andere Verbindung, bevor Sie diese Seite neu laden.",
              code: "MAX_CLIENTS_REACHED"
            });
          } else {
            alert(`Maximale Anzahl von ${maxClients} Clients erreicht. Bitte trennen Sie eine andere Verbindung.`);
          }
          // Don't reconnect if max clients reached - user needs to close another tab
          ws = null;
          return;
        }
        
        ws = null;
        if (window.FirmwareUpdate && typeof window.FirmwareUpdate.ensureStatusPollingWhenDownloading === "function") {
          window.FirmwareUpdate.ensureStatusPollingWhenDownloading();
        }
        // During firmware download, do not reconnect WebSocket: device is under load and WS would
        // disconnect again, causing a connect/disconnect storm. Progress is shown via HTTP polling.
        if (window.FirmwareUpdate && typeof window.FirmwareUpdate.getState === "function" && window.FirmwareUpdate.getState() === "downloading") {
          return;
        }
        scheduleReconnect();
      };
    } catch (e) {
      setIndicator(false);
      scheduleReconnect();
    }
  }

  // ---- Public API surface (keeps existing callers stable) ----
  // NOTE: This replaces the old REST client while preserving method names used by the UI modules.
  window.ApiV1 = {
    // Actions (Homewind endpoints)
    connectSensor: (idOrName) => httpPost("/api/v1/action/sensor/connect", { name: idOrName }),
    disconnectSensor: (idOrName) => httpPost("/api/v1/action/sensor/disconnect", { name: idOrName }),
    deleteSensor: (idOrName) => httpPost("/api/v1/action/sensor/delete", { name: idOrName }),
    setSensor: (index) => httpPost("/api/v1/action/sensor/set", { index: index }),
    reloadSensors: () => httpPost("/api/v1/action/sensor/reload", {}),
    pauseBLERelayServer: () => httpPost("/api/v1/action/sensor/server/pause", {}),
    resumeBLERelayServer: () => httpPost("/api/v1/action/sensor/server/resume", {}),

    discoveryStart: (type) => httpPost("/api/v1/action/discovery/start", { type }),
    discoveryStop: () => httpPost("/api/v1/action/discovery/stop", {}),

    // Fan controls
    setFanControlState: (id8, controlState) => {
      const token = resolveFanTokenById(id8) || id8;
      return httpPost("/api/v1/action/fan/control", {
        token,
        controlState,
      });
    },
    addFan: (token) => httpPost("/api/v1/action/fan/add", { token }),
    updateFan: (oldToken, newToken) => httpPost("/api/v1/action/fan/update", { oldToken, newToken }),
    removeFan: (id8) => {
      const token = resolveFanTokenById(id8) || id8;
      return httpPost("/api/v1/action/fan/remove", { token });
    },

    firmwareSearch: () => httpPost("/api/v1/action/firmware/search", {}),
    firmwareDownload: () => httpPost("/api/v1/action/firmware/download", {}),
    firmwareInstall: () => httpPost("/api/v1/action/firmware/install", {}),
    firmwareAbort: () => httpPost("/api/v1/action/firmware/abort", {}),
    refreshTelemetry: () => httpPost("/api/v1/action/telemetry/refresh", {
      client_id: (wsClientId && wsClientId > 0) ? wsClientId : undefined,
    }),

    // State access (read-only, in-memory)
    getState: () => ({
      system: { ...state.system },
      sensors: Array.from(state.sensors.values()),
      fans: Array.from(state.fans.values()),
      heartRate: state.heartRate,
      firmwareVersion: state.firmwareVersion,
      firmwareProgress: state.firmwareProgress,
      discovery: { ...state.discovery },
    }),

    // WS control
    wsConnect: connectWS,
    wsDisconnect: () => { try { if (ws) ws.close(); } catch (_) {} ws = null; },
  };
  
  let telemetryRefreshRequested = false;
  let telemetryRefreshInFlight = false;
  window.requestTelemetryRefresh = function(force = false) {
    if (telemetryRefreshInFlight) return;
    if (telemetryRefreshRequested && !force) return;
    if (!window.ApiV1 || typeof window.ApiV1.refreshTelemetry !== "function") return;
    telemetryRefreshInFlight = true;
    window.ApiV1.refreshTelemetry()
      .then((res) => {
        if (res && res.ok) {
          telemetryRefreshRequested = true;
        } else {
          telemetryRefreshRequested = false;
        }
      })
      .catch(() => {
        telemetryRefreshRequested = false;
      })
      .finally(() => {
        telemetryRefreshInFlight = false;
      });
  };

  // Start WS on DOM ready
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", connectWS);
  } else {
    connectWS();
  }

  // Reconnect when tab becomes visible again (only if disconnected)
  document.addEventListener("visibilitychange", () => {
    if (!document.hidden) {
      // Only attempt reconnect if truly disconnected (not just backgrounded)
      if (!ws || ws.readyState === WebSocket.CLOSED) {
        connectWS();
      }
    }
  });
})();

// ===============================================================
// FIRMWARE UPDATE API - Status endpoint for reboot detection only
// ===============================================================
const FW_API = {
  status: "/api/v1/action/firmware/status"  // Only used for reboot detection (HTTP polling after restart)
};

// ===============================================================
// FIRMWARE UPDATE WINDOW FUNCTIONS (Aliases to ApiV1)
// These exist for backward compatibility with FirmwareUpdate module.
// All actions go through ApiV1 (Single Source of Truth).
// ===============================================================

/**
 * Starts firmware search via ApiV1.
 * @returns {Promise<boolean>} True if search started successfully
 */
window.searchFirmware = async function () {
  const result = await window.ApiV1.firmwareSearch();
  return result && result.ok;
};

/**
 * Starts firmware download via ApiV1.
 * @returns {Promise<boolean>} True if download started successfully
 */
window.startFirmwareDownload = async function () {
  const result = await window.ApiV1.firmwareDownload();
  return result && result.ok;
};

/**
 * Starts firmware installation via ApiV1.
 * @returns {Promise<boolean>} True if installation started successfully
 */
window.startFirmwareInstallation = async function () {
  const result = await window.ApiV1.firmwareInstall();
  return result && result.ok;
};

/**
 * Aborts current firmware update operation via ApiV1.
 * @returns {Promise<boolean>} True if abort succeeded
 */
window.abortUpdate = async function () {
  try {
    const result = await window.ApiV1.firmwareAbort();
    return result && result.ok;
  } catch (err) {
    return false;
  }
};
/*
===============================================================
AppModal – Flexible Modal Framework
===============================================================

This script provides a fully dynamic modal dialog system that allows
any part of your application (UI or ESP32/Arduino-driven code) to open
a Lightbox/Modal with:

  • Custom title
  • Arbitrary HTML or DOM content
  • Fully configurable footer buttons (0 to N)
  • Custom callback functions per button
  • Selectable auto-close behavior
  • Optional custom CSS class names for button styling

The system is designed to work with dynamically generated content
(e.g. fan setup, fan editing, heart rate picker, firmware dialogs,
confirmation dialogs, etc.) and is safe to use in embedded ESP32 UI
contexts.

---------------------------------------------------------------
Basic Usage
---------------------------------------------------------------

AppModal.open({
  title: "Example",
  content: "<p>Hello World</p>",            // OR: HTMLElement

  // Optional default handlers for Save/Cancel
  onSave: () => {},
  onCancel: () => {},

  // Optional default button configuration
  // (only used if `buttons` is NOT provided)
  saveLabel: "Save",
  cancelLabel: "Cancel",
  showCancel: true
});

---------------------------------------------------------------
Advanced Button Control
---------------------------------------------------------------

AppModal.open({
  title: "Custom Buttons",
  content: "<p>Choose wisely…</p>",
  buttons: [
    {
      id: "cancel",
      type: "cancel",               // "cancel" | "save" | "delete" | custom
      label: "Cancel",
      className: "btn-secondary",   // optional custom classes
      closeOnClick: true,           // auto-close modal after click
      onClick: () => { /* extra logic *-/ }
    },
    {
      id: "delete",
      type: "delete",
      label: "Delete",
      className: "btn-danger",
      closeOnClick: true,
      onClick: () => { /* delete logic *-/ }
    },
    {
      id: "save",
      type: "save",
      label: "Save",
      className: "btn-primary",
      closeOnClick: false,          // usually we validate before closing
      onClick: () => { /* you can handle save here OR use onSave *-/ }
    }
  ],
  actions: false,
});

If `actions` is false:
  • extra closbuttun x and esc key for close are deactivated

If `buttons` is provided:
  • Footer is built exactly from that array.
  • If `buttons.length === 0`, footer is hidden.

If `buttons` is NOT provided:
  • A default footer is created with:
      - optional Cancel button (label = cancelLabel, showCancel !== false)
      - Save button (label = saveLabel)

---------------------------------------------------------------
Default Button Styling
---------------------------------------------------------------

If no `className` is provided, the following default classes are used:

  type "save"   → "hr-modal-btn hr-modal-btn-primary"
  type "cancel" → "hr-modal-btn hr-modal-btn-secondary"
  type "delete" → "hr-modal-btn hr-modal-btn-danger"
  other types   → "hr-modal-btn"

You can override them in CSS or via `className`.

---------------------------------------------------------------
Closing the Modal
---------------------------------------------------------------

AppModal.close();   // manually close the modal

Buttons close automatically if:
  - button.closeOnClick === true
  - For default buttons:
      • "cancel"  → closeOnClick = true
      • "save"    → closeOnClick = false (close only if onSave() !== false)
      • "delete"  → closeOnClick = true

Additionally, the modal closes when:
  • User clicks the close "X" icon
  • User clicks on the backdrop (outside the modal)
  • User presses ESC
In those cases, `onCancel` is called (if provided).

---------------------------------------------------------------
Integration With ESP32 / Arduino
---------------------------------------------------------------

Your ESP32/Arduino code (via WebSocket, HTTP, etc.) can:

  • Open modals:

      AppModal.open({
        title: "New Fan",
        content: "<p>Configure fan…</p>",
        onSave: () => { /* apply settings *-/ }
      });

  • Close modals:

      AppModal.close();

  • Use custom button sets for confirmation, delete dialogs, etc.

===============================================================
*/

(function () {
  let backdrop = null;
  let modal = null;
  let titleEl = null;
  let iconEl = null;
  let sublineEl = null;
  let bodyEl = null;
  let footerEl = null;
  let allowClose = true;
  let closeBtn = null;
  let globalHandlersAttached = false;
  let currentConfig = null;
  let escKeyHandler = null; // Store ESC handler for cleanup

  // Template fallback if no modal markup exists in HTML
  const MODAL_TEMPLATE = `
    <div id="app-modal-backdrop" class="app-modal-backdrop" aria-hidden="true">
      <div class="app-modal" role="dialog" aria-modal="true" aria-labelledby="app-modal-title">
        <div class="app-modal-header">
          <div class="app-modal-header-content">
            <div class="app-modal-icon-wrapper"></div>
            <h2 id="app-modal-title" class="app-modal-title"></h2>
            <div class="app-modal-subline"></div>
          </div>
          <button type="button" class="app-modal-close" aria-label="Close">&times;</button>
        </div>
        <div class="app-modal-body">
          <div class="app-modal-content"></div>
        </div>
        <div class="app-modal-footer"></div>
      </div>
    </div>
  `;

  /**
   * Ensures modal DOM elements exist and are initialized.
   * Creates modal structure if it doesn't exist and attaches global event handlers.
   * 
   * @param {boolean|undefined} allowActions - If false, disables close actions (backdrop click, ESC key, close button)
   * @returns {void}
   * @private
   */
  function ensureModalDOM(allowActions) {
    // Set close permission
    allowClose = allowActions !== false;
    
    // Early return if already initialized
    if (backdrop && modal && titleEl && bodyEl && footerEl) {
      if (closeBtn) {
        closeBtn.style.display = allowClose ? "" : "none";
      }
      return;
    }

    // Find or create backdrop
    backdrop =
      document.getElementById("app-modal-backdrop") ||
      document.getElementById("hr-modal-backdrop");

    if (!backdrop) {
      const wrapper = document.createElement("div");
      wrapper.innerHTML = MODAL_TEMPLATE.trim();
      backdrop = wrapper.firstElementChild;
      document.body.appendChild(backdrop);
    }

    // Get or create modal elements
    modal = backdrop.querySelector(".app-modal");
    if (!modal) {
      return;
    }

    // Get or create header content wrapper
    let headerContent = backdrop.querySelector(".app-modal-header-content");
    if (!headerContent) {
      const header = backdrop.querySelector(".app-modal-header");
      if (header) {
        headerContent = document.createElement("div");
        headerContent.className = "app-modal-header-content";
        const closeBtn = header.querySelector(".app-modal-close");
        if (closeBtn) {
          header.insertBefore(headerContent, closeBtn);
        } else {
          header.appendChild(headerContent);
        }
      }
    }

    // Get or create icon wrapper
    iconEl = backdrop.querySelector(".app-modal-icon-wrapper");
    if (!iconEl && headerContent) {
      iconEl = document.createElement("div");
      iconEl.className = "app-modal-icon-wrapper";
      headerContent.appendChild(iconEl);
    }

    // Get or create subline
    sublineEl = backdrop.querySelector(".app-modal-subline");
    if (!sublineEl && headerContent) {
      sublineEl = document.createElement("div");
      sublineEl.className = "app-modal-subline";
      headerContent.appendChild(sublineEl);
    }

    // Get or create title
    titleEl = backdrop.querySelector(".app-modal-title");
    if (!titleEl) {
      if (headerContent) {
        titleEl = document.createElement("h2");
        titleEl.className = "app-modal-title";
        titleEl.id = "app-modal-title";
        headerContent.appendChild(titleEl);
      } else {
        const header = backdrop.querySelector(".app-modal-header");
        if (header) {
          titleEl = document.createElement("h2");
          titleEl.className = "app-modal-title";
          titleEl.id = "app-modal-title";
          header.insertBefore(titleEl, header.firstChild);
        }
      }
    }

    closeBtn = backdrop.querySelector(".app-modal-close");
    if (closeBtn) {
      closeBtn.style.display = allowClose ? "" : "none";
    }

    // Get or create body and content
    let bodyContainer = backdrop.querySelector(".app-modal-body");
    if (!bodyContainer) {
      bodyContainer = document.createElement("div");
      bodyContainer.className = "app-modal-body";
      modal.appendChild(bodyContainer);
    }

    bodyEl = bodyContainer.querySelector(".app-modal-content");
    if (!bodyEl) {
      bodyEl = document.createElement("div");
      bodyEl.className = "app-modal-content";
      // Move existing children into content wrapper
        while (bodyContainer.firstChild) {
        bodyEl.appendChild(bodyContainer.firstChild);
        }
      bodyContainer.appendChild(bodyEl);
    }

    // Get or create footer
    footerEl = backdrop.querySelector(".app-modal-footer");
    if (!footerEl) {
      footerEl = document.createElement("div");
      footerEl.className = "app-modal-footer";
      modal.appendChild(footerEl);
    }
    
    attachGlobalHandlers();
  }

  /**
   * Handles modal close action from various sources.
   * Checks if closing is allowed and calls onCancel callback if configured.
   * 
   * @param {string} source - Source of close action ("close-button", "backdrop", "escape")
   * @returns {void}
   * @private
   */
  function handleClose(source) {
    if (!allowClose) return;

    if (currentConfig && typeof currentConfig.onCancel === "function") {
      currentConfig.onCancel(source);
    }
    close();
  }

  /**
   * Attaches global event handlers for modal interactions.
   * Sets up click handlers for close button and backdrop, and ESC key handler.
   * Only attaches handlers once to prevent duplicates.
   * 
   * @returns {void}
   * @private
   */
  function attachGlobalHandlers() {
    if (globalHandlersAttached || !backdrop) return;

    // Close button (X)
    if (closeBtn) {
      closeBtn.addEventListener("click", () => {
        handleClose("close-button");
      });
    }

    // Click on backdrop to close
    backdrop.addEventListener("click", (e) => {
      if (e.target === backdrop) {
        handleClose("backdrop");
      }
    });

    // ESC key closes modal (store handler for potential cleanup)
    escKeyHandler = (e) => {
      if (e.key === "Escape" && backdrop && backdrop.classList.contains("is-open")) {
        handleClose("escape");
      }
    };
    document.addEventListener("keydown", escKeyHandler);

    globalHandlersAttached = true;
  }

  /**
   * Sets the modal title, icon, and subline.
   * 
   * @param {string} title - Title text to display
   * @param {string} [icon] - CSS variable name or data URL for icon background image
   * @param {string} [subline] - Subline text to display above title
   * @returns {void}
   * @private
   */
  function setTitle(title, icon, subline) {
    if (titleEl) {
      titleEl.textContent = title || "";
    }

    if (iconEl) {
      if (icon) {
        // Support both CSS variables and direct data URLs
        if (icon.startsWith("var(") || icon.startsWith("url(")) {
          iconEl.style.backgroundImage = icon;
        } else if (icon.startsWith("--")) {
          iconEl.style.backgroundImage = `var(${icon})`;
        } else {
          iconEl.style.backgroundImage = `url(${icon})`;
        }
        iconEl.style.display = "block";
      } else {
        iconEl.style.backgroundImage = "";
        iconEl.style.display = "none";
      }
    }

    if (sublineEl) {
      sublineEl.textContent = subline || "";
      sublineEl.style.display = subline ? "block" : "none";
    }
  }

  /**
   * Sets the modal content.
   * Clears existing content and replaces it with the provided content (HTML string or DOM element).
   * 
   * @param {string|HTMLElement} content - Content to display (HTML string or DOM element)
   * @returns {void}
   * @private
   */
  function setContent(content) {
    if (!bodyEl) return;

    // Clear current content
    while (bodyEl.firstChild) {
      bodyEl.removeChild(bodyEl.firstChild);
    }

    if (content instanceof HTMLElement) {
      bodyEl.appendChild(content);
    } else if (typeof content === "string") {
      bodyEl.innerHTML = content;
    }
  }

  /**
   * Gets default button CSS class based on button type.
   * Returns appropriate hierarchy class (btn-cta, btn-tertiary, btn-danger, etc.) combined with base classes.
   * 
   * @param {string} type - Button type ("save", "cancel", "delete", "primary", "secondary")
   * @returns {string} CSS class name including base classes and type-specific class
   * @private
   */
  function getDefaultButtonClass(type) {
    const baseClasses = "btn btn-regular";
    const typeMap = {
      save: "btn-cta",
      cancel: "btn-tertiary",
      delete: "btn-danger",
      primary: "btn-primary",
      secondary: "btn-secondary",
    };
    return typeMap[type] ? `${baseClasses} ${typeMap[type]}` : baseClasses;
  }

  /**
   * Handles button click based on type and configuration.
   * Executes custom onClick handler if provided, otherwise uses default handlers for standard button types.
   * 
   * @param {Object} btnConfig - Button configuration object
   * @param {string} btnConfig.type - Button type ("save", "cancel", "delete", or custom)
   * @param {boolean} [btnConfig.closeOnClick] - Whether modal should close after click
   * @param {Function} [btnConfig.onClick] - Custom click handler function
   * @returns {boolean} True if modal should close, false otherwise
   * @private
   */
  function handleButtonClick(btnConfig) {
    const { type, closeOnClick, onClick } = btnConfig;

    // Custom onClick handler takes precedence
    if (typeof onClick === "function") {
      const result = onClick();
      // If onClick returns false, don't close even if closeOnClick is true
      if (result === false) return false;
      // If closeOnClick is true and result is not false, close
      if (closeOnClick) return true;
      return false;
    }

    // Default handlers for standard button types
    switch (type) {
      case "cancel":
        if (currentConfig && typeof currentConfig.onCancel === "function") {
          currentConfig.onCancel("button-cancel");
        }
        return closeOnClick !== false;

      case "save":
        let saveResult = undefined;
        if (currentConfig && typeof currentConfig.onSave === "function") {
          saveResult = currentConfig.onSave();
        }
        // Close if onSave didn't return false and closeOnClick allows it
        return closeOnClick !== false && saveResult !== false;

      case "delete":
        if (currentConfig && typeof currentConfig.onDelete === "function") {
          currentConfig.onDelete();
        }
        return closeOnClick !== false;

      default:
        // Custom type: close if closeOnClick is true
        return closeOnClick === true;
    }
  }

  /**
   * Builds footer buttons based on options.
   * Creates button elements and attaches event handlers. Hides footer if empty button array is provided.
   * 
   * @param {Object} opts - Button configuration options
   * @param {Array} [opts.buttons] - Custom button configuration array
   * @param {boolean} [opts.showCancel] - Whether to show cancel button (default: true)
   * @param {string} [opts.cancelLabel] - Cancel button label (default: "Cancel")
   * @param {string} [opts.saveLabel] - Save button label (default: "Save")
   * @returns {void}
   * @private
   */
  function buildFooterButtons(opts) {
    if (!footerEl) return;

    footerEl.innerHTML = "";

    const userButtons = Array.isArray(opts.buttons) ? opts.buttons : null;

    // If user explicitly passes an empty array → hide footer
    if (userButtons && userButtons.length === 0) {
      footerEl.style.display = "none";
      return;
    }

    footerEl.style.display = "";

    let buttonsToBuild = [];

    if (userButtons) {
      buttonsToBuild = userButtons;
    } else {
      // Default button set: Cancel + Save
      const showCancel = opts.showCancel !== false;
      const cancelLabel = opts.cancelLabel || "Cancel";
      const saveLabel = opts.saveLabel || "Save";

      if (showCancel) {
        buttonsToBuild.push({
          type: "cancel",
          label: cancelLabel,
          closeOnClick: true,
        });
      }

      buttonsToBuild.push({
        type: "save",
        label: saveLabel,
        closeOnClick: false, // Save usually validates before closing
      });
    }

    buttonsToBuild.forEach((btnConfig, index) => {
      const btn = document.createElement("button");
      btn.type = "button";
      btn.textContent = btnConfig.label || btnConfig.type || `Button ${index + 1}`;

      // Apply CSS classes
      if (btnConfig.className) {
        btn.className = `btn btn-regular ${btnConfig.className}`.trim();
      } else {
        btn.className = getDefaultButtonClass(btnConfig.type);
      }

      // Attach click handler
      btn.addEventListener("click", () => {
        const shouldClose = handleButtonClick(btnConfig);
        if (shouldClose) {
            close();
        }
      });

      footerEl.appendChild(btn);
    });
  }

  /**
   * Opens the modal with specified options.
   * Initializes modal DOM, sets title and content, builds footer buttons, and displays the modal.
   * 
   * @param {Object} options - Modal configuration object
   * @param {string} options.title - Modal title text
   * @param {string|HTMLElement} options.content - Modal content (HTML string or DOM element)
   * @param {string} [options.icon] - CSS variable name (e.g., "--icon-checkmark-svg-data") or data URL for icon background image
   * @param {string} [options.subline] - Subline text to display above the title
   * @param {Array} [options.buttons] - Custom button configuration array
   * @param {Function} [options.onSave] - Save button callback function
   * @param {Function} [options.onCancel] - Cancel button callback function
   * @param {Function} [options.onDelete] - Delete button callback function
   * @param {boolean} [options.actions=true] - Enable close actions (backdrop click, ESC key, close button)
   * @param {Function} [options.onOpen] - Callback function called after modal opens
   * @returns {void}
   */
  function open(options) {
    currentConfig = options || {};
    ensureModalDOM(currentConfig.actions);
    setTitle(currentConfig.title || "", currentConfig.icon, currentConfig.subline);
    setContent(currentConfig.content || "");
    buildFooterButtons(currentConfig);

    // CSS handles visibility, centering & transitions via ".is-open"
    backdrop.classList.add("is-open");
    backdrop.setAttribute("aria-hidden", "false");
    document.body.classList.add("app-modal-open");

    // Call onOpen callback if provided
    if (currentConfig.onOpen && typeof currentConfig.onOpen === "function") {
      // Use setTimeout to ensure DOM is ready
      setTimeout(() => {
        currentConfig.onOpen();
      }, 0);
    }
  }

  /**
   * Closes the modal.
   * Removes open state classes, resets ARIA attributes, and clears current configuration.
   * 
   * @returns {void}
   */
  function close() {
    if (!backdrop) return;

    backdrop.classList.remove("is-open");
    backdrop.setAttribute("aria-hidden", "true");
    document.body.classList.remove("app-modal-open");
    currentConfig = null;
    allowClose = true; // Reset close permission
  }

  const AppModal = {
    open,
    close,
  };

  window.AppModal = AppModal;
})();

// ===============================================================
// HTML Escaping Utility - Centralized XSS protection
// ===============================================================
/**
 * Escapes HTML special characters to prevent XSS attacks.
 * Centralized utility function used across all modules.
 * 
 * @param {string} str - String to escape
 * @returns {string} Escaped string, or empty string if input is not a string
 */
window.escapeHtml = function(str) {
  if (typeof str !== "string") return "";
  return str
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;")
    .replace(/'/g, "&#39;");
};

// ===============================================================
// Error Modal Utility - Common error handling pattern
// ===============================================================
/**
 * Shows an error modal with consistent styling and behavior.
 * Extracts error information from various formats (string, object with message/error/code/reason).
 * 
 * @param {string|Object} error - Error message or error object
 * @param {Object} [options={}] - Additional options
 * @param {string} [options.title="Error"] - Modal title
 * @param {string} [options.icon] - Icon CSS variable (default: "--icon-error-svg-data")
 * @param {string} [options.subline] - Subline text
 * @param {string} [options.buttonLabel="OK"] - Button label
 * @param {Function} [options.onClose] - Callback when modal is closed
 * @returns {void}
 */
window.showErrorModal = function(error, options = {}) {
  if (!window.AppModal) {
    // Fallback to alert if AppModal is not available
    const message = typeof error === "string" ? error : (error?.message || error?.error || "An error occurred");
    alert(message);
    return;
  }

  // Extract error information
  let errorText = "An error occurred.";
  let errorCode = null;
  let errorReason = null;

  if (typeof error === "string" && error.trim()) {
    errorText = error;
  } else if (error && typeof error === "object") {
    if (typeof error.message === "string") {
      errorText = error.message;
    } else if (typeof error.error === "string") {
      errorText = error.error;
    }
    
    if (typeof error.code === "string" || typeof error.code === "number") {
      errorCode = String(error.code);
    }
    
    if (typeof error.reason === "string") {
      errorReason = error.reason;
    }
  }

  // Escape HTML using centralized utility
  const text = window.escapeHtml(errorText);
  const code = errorCode ? window.escapeHtml(errorCode) : null;
  const reason = errorReason ? window.escapeHtml(errorReason) : null;

  // Build error content
  let errorContent = `<p class="modal-text">${text}</p>`;
  
  if (code || reason) {
    errorContent += `<div class="fw-modal-notes" style="margin-top: 16px; padding-top: 16px; border-top: 1px solid rgba(255,255,255,0.1);">`;
    if (code) {
      errorContent += `<div style="font-size: 12px; opacity: 0.8; margin-bottom: 8px;">Error Code: <strong>${code}</strong></div>`;
    }
    if (reason) {
      errorContent += `<div style="font-size: 12px; opacity: 0.8;">Reason: ${reason}</div>`;
    }
    errorContent += `</div>`;
  }

  window.AppModal.open({
    title: options.title || "Error",
    icon: options.icon || "--icon-error-svg-data",
    subline: options.subline,
    content: `
      <div class="modal-content-centered">
        ${errorContent}
      </div>
    `,
    buttons: [{
      type: "cancel",
      label: options.buttonLabel || "OK",
      className: "btn btn-regular btn-primary",
      closeOnClick: true,
      onClick: options.onClose
    }],
  });
};

// ===============================================================
// Shared reboot monitoring – poll until device is back, then close
// modal and reload. Used by Firmware Update and Device Restart.
// ===============================================================
window.startRebootMonitoring = function (options) {
  const {
    endpoint = (typeof FW_API !== "undefined" && FW_API.status) ? FW_API.status : "/api/v1/action/firmware/status",
    isActive = () => true,
    onSuccess = () => {
      if (window.AppModal && typeof window.AppModal.close === "function") window.AppModal.close();
      window.location.reload();
    },
    onTimeout = () => {
      if (window.AppModal && typeof window.AppModal.open === "function") {
        window.AppModal.open({
          title: "Reboot Taking Longer",
          content: `
            <div class="modal-content-centered">
              <p class="modal-text">
                The device is taking longer to restart than expected.
                Please wait a moment and reload the page manually.
              </p>
            </div>
          `,
          buttons: [{
            type: "save",
            label: "Reload Page",
            className: "btn btn-regular btn-primary",
            closeOnClick: false,
            onClick: () => { window.location.reload(); return true; },
          }],
        });
      } else {
        window.location.reload();
      }
    },
  } = options || {};

  const timing = (window.TIMING || {});
  const startMs = Date.now();

  // Total monitoring budget (ms)
  const maxTotalMs = timing.REBOOT_MAX_TOTAL_MS || 60000;

  // Start quickly (helps when the ESP is already back)
  const initialDelay = timing.REBOOT_INITIAL_DELAY_MS || 500;

  // Fast polling window (ms) and intervals
  const fastWindowMs = timing.REBOOT_POLL_FAST_WINDOW_MS || 5000;
  const fastPollMs = timing.REBOOT_POLL_FAST_INTERVAL_MS || 400;
  const slowPollMs = timing.REBOOT_POLL_INTERVAL_MS || 2000;

  // Per-request timeout (ms). Prevents long TCP timeouts from stretching the UI delay.
  const fastReqTimeoutMs = timing.REBOOT_REQUEST_TIMEOUT_FAST_MS || 1200;
  const slowReqTimeoutMs = timing.REBOOT_REQUEST_TIMEOUT_SLOW_MS || 2500;

  // "Reboot" semantics: prefer offline->online to avoid reloading before reboot actually starts.
  const requireOffline = (typeof timing.REBOOT_REQUIRE_OFFLINE !== "undefined") ? !!timing.REBOOT_REQUIRE_OFFLINE : true;
  const maxWaitForOfflineMs = timing.REBOOT_MAX_WAIT_FOR_OFFLINE_MS || 6000;

  let done = false;
  let attempts = 0;
  let seenOffline = false;

  function parseKeyValueText(text) {
    const params = {};
    let hasAny = false;
    (text || "").split("\n").forEach((line) => {
      const idx = line.indexOf("=");
      if (idx > 0) {
        const k = line.substring(0, idx).trim();
        const v = line.substring(idx + 1).trim();
        if (k) {
          params[k] = v;
          hasAny = true;
        }
      }
    });
    return { params, hasAny };
  }

  function isInProgressState(state) {
    // Firmware update states that indicate "do NOT reload yet".
    return state === "searching" || state === "downloading" || state === "installing";
  }

  function withTimestamp(url) {
    const sep = url.indexOf("?") >= 0 ? "&" : "?";
    return url + sep + "_ts=" + Date.now();
  }

  function scheduleNext() {
    if (done || !isActive()) return;

    const elapsed = Date.now() - startMs;
    if (elapsed >= maxTotalMs) {
      done = true;
      onTimeout();
      return;
    }

    const interval = (elapsed <= fastWindowMs) ? fastPollMs : slowPollMs;
    setTimeout(checkReboot, interval);
  }

  function checkReboot() {
    if (done || !isActive()) return;
    attempts++;

    const elapsed = Date.now() - startMs;
    const reqTimeoutMs = (elapsed <= fastWindowMs) ? fastReqTimeoutMs : slowReqTimeoutMs;

    const controller = (typeof AbortController !== "undefined") ? new AbortController() : null;
    let timeoutHandle = null;
    if (controller) {
      timeoutHandle = setTimeout(() => {
        try { controller.abort(); } catch (_) {}
      }, reqTimeoutMs);
    }

    fetch(withTimestamp(endpoint), {
      method: "GET",
      cache: "no-store",
      signal: controller ? controller.signal : undefined,
    })
      .then((r) => {
        if (timeoutHandle) clearTimeout(timeoutHandle);
        if (!r.ok) throw new Error("Status: " + r.status);
        return r.text();
      })
      .then((text) => {
        const { params, hasAny } = parseKeyValueText(text);
        const state = (params && typeof params.state === "string") ? params.state : "";

        // If we see any valid response that parses, the device is online.
        // We reload when the device is back AND not in an in-progress update state.
        if (hasAny && !isInProgressState(state)) {
          const offlineSatisfied = !requireOffline || seenOffline || (elapsed >= maxWaitForOfflineMs);
          if (offlineSatisfied) {
            done = true;
            onSuccess();
            return;
          }
        }

        scheduleNext();
      })
      .catch(() => {
        if (timeoutHandle) clearTimeout(timeoutHandle);
        // Any network/HTTP failure is a good "offline" signal during reboot.
        seenOffline = true;
        scheduleNext();
      });
  }

  setTimeout(checkReboot, initialDelay);
};

/**
 * ===============================================================
 * HeartRateManager
 * ===============================================================
 * Manages heart rate display, sensor name, and min/max range picker.
 * Provides public API for ESP32/Arduino integration.
 *
 * Public API:
 * - HeartRateManager.open(mode) - Open heart rate picker ("min" or "max")
 * - HeartRateManager.updateHeartRate(value) - Update displayed heart rate value
 * - HeartRateManager.updateSensorName(name) - Update displayed sensor name
 *
 * Arduino Integration Callbacks:
 * - window.onMinHeartRateChanged(value) - Called when min heart rate is saved
 *   - value: number - The new min heart rate value
 * - window.onMaxHeartRateChanged(value) - Called when max heart rate is saved
 *   - value: number - The new max heart rate value
 * - window.onHeartRateChanged(value) - Called when heart rate value is updated
 *   - value: number - The new heart rate value
 * - window.onSensorNameChanged(name) - Called when sensor name is updated
 *   - name: string - The new sensor name
 * ===============================================================
 */
  (function () {
    const HARD_MIN = 50;
    const HARD_MAX = 200;
    const ITEM_HEIGHT = 40;
  const HR_MIN_VALUE = 0;
  const HR_MAX_VALUE = 300;

  // DOM element references
    const hrMinDisplay = document.getElementById("hr-min-value");
    const hrMaxDisplay = document.getElementById("hr-max-value");
  const hrValueDisplay = document.getElementById("hr-value");
  const hrSensorNameDisplay = document.getElementById("hr-sensor-name");

  /**
   * Clamps a value between minimum and maximum bounds.
   * 
   * @param {number} v - Value to clamp
   * @param {number} min - Minimum allowed value
   * @param {number} max - Maximum allowed value
   * @returns {number} Clamped value within [min, max] range
   * @private
   */
    function clamp(v, min, max) {
      return Math.min(max, Math.max(min, v));
    }

  /**
   * Opens the heart rate picker modal for min or max value.
   * 
   * The Save button starts in an inactive state and becomes active (CTA style)
   * when the user changes the value from the initial value. If the user cancels,
   * no changes are saved and the original value is preserved.
   * 
   * @param {string} mode - "min" or "max" to determine which heart rate value to edit
   * @returns {void}
   */
    function openHeartRatePicker(mode) {
      const currentMin = parseInt(hrMinDisplay.textContent, 10) || HARD_MIN;
      const currentMax = parseInt(hrMaxDisplay.textContent, 10) || HARD_MAX;

      // Dynamische Grenzen abhängig vom Modus
      let activeMin, activeMax, labelText;
      if (mode === "min") {
        activeMin = HARD_MIN;
        activeMax = clamp(currentMax - 10, HARD_MIN, HARD_MAX);
        labelText = "Set Min Heart Rate";
      } else {
        activeMin = clamp(currentMin + 10, HARD_MIN, HARD_MAX);
        activeMax = HARD_MAX;
        labelText = "Set Max Heart Rate";
      }

      // Calculate initial value (current min or max, clamped to valid range)
      const initialValue =
        mode === "min"
          ? clamp(currentMin, activeMin, activeMax)
          : clamp(currentMax, activeMin, activeMax);

      // Modal-Content DOM bauen
      const container = document.createElement("div");
      container.innerHTML = `
        <div class="picker-wrapper">
          <div class="picker-window">
            <div class="picker-spacer"></div>
            <!-- Items kommen per JS -->
            <div class="picker-spacer"></div>
          </div>
          <div class="picker-highlight"></div>
        </div>
      `;

      const pickerWindow = container.querySelector(".picker-window");

      // Store initial value for change detection
      const initialValueStored = initialValue;
      let saveButton = null;

      // Items einsetzen
      const spacerTop = pickerWindow.firstElementChild;
      const spacerBottom = pickerWindow.lastElementChild;

      for (let v = activeMin; v <= activeMax; v++) {
        const div = document.createElement("div");
        div.className = "picker-item";
        div.textContent = v;
        pickerWindow.insertBefore(div, spacerBottom);
      }

      const items = Array.from(container.querySelectorAll(".picker-item"));
      let scrollTimer = null;

      /**
       * Gets the currently selected value from the active picker item.
       * Falls back to calculating from scroll position if no active item is found.
       * 
       * @returns {number} The current selected value
       * @private
       */
      function getCurrentValue() {
        const activeItem = items.find((item) => item.classList.contains("active"));
        if (activeItem) {
          return parseInt(activeItem.textContent, 10);
        }
        // Fallback: calculate from scroll position
        const center = pickerWindow.scrollTop + pickerWindow.clientHeight / 2;
        let closestIndex = 0;
        let closestDist = Infinity;
        items.forEach((item, idx) => {
          const itemCenter = item.offsetTop + ITEM_HEIGHT / 2;
          const dist = Math.abs(itemCenter - center);
          if (dist < closestDist) {
            closestDist = dist;
            closestIndex = idx;
          }
        });
        return activeMin + closestIndex;
      }

      /**
       * Checks if the current value has changed from the initial value.
       * 
       * @returns {boolean} True if value has changed, false otherwise
       * @private
       */
      function hasValueChanged() {
        const currentValue = getCurrentValue();
        return currentValue !== initialValueStored;
      }

      /**
       * Updates Save button state based on whether value has changed.
       * Enables and styles button as CTA if value changed, otherwise disables and styles as inactive.
       * 
       * @returns {void}
       * @private
       */
      function updateSaveButtonState() {
        if (!saveButton) return;

        const hasChanged = hasValueChanged();

        if (hasChanged) {
          // Value changed - make Save button CTA
          saveButton.disabled = false;
          saveButton.classList.remove("btn-inactive", "btn-primary");
          saveButton.classList.add("btn-cta");
        } else {
          // Value unchanged - make Save button inactive
          saveButton.disabled = true;
          saveButton.classList.remove("btn-cta", "btn-primary");
          saveButton.classList.add("btn-inactive");
        }
      }

      /**
       * Centers the picker on a specific value and updates the display.
       * Clamps the value to valid range and scrolls to the corresponding item.
       * 
       * @param {number} targetValue - The value to center on
       * @returns {void}
       * @private
       */
      function centerOnValue(targetValue) {
        const value = clamp(targetValue, activeMin, activeMax);
        const index = value - activeMin;
        const item = items[index];
        if (!item) return;

        items.forEach((el, idx) => {
          el.classList.toggle("active", idx === index);
        });

        const targetScroll =
          item.offsetTop + ITEM_HEIGHT / 2 - pickerWindow.clientHeight / 2;

        pickerWindow.scrollTop = targetScroll;
        updateSaveButtonState();
      }

      /**
       * Updates the active picker item based on scroll position.
       * Called automatically when the user scrolls the picker wheel.
       * Finds the item closest to the center and updates active state.
       * 
       * @returns {void}
       * @private
       */
      function updateActiveFromScroll() {
        const center = pickerWindow.scrollTop + pickerWindow.clientHeight / 2;
        let closestIndex = 0;
        let closestDist = Infinity;

        items.forEach((item, idx) => {
          const itemCenter = item.offsetTop + ITEM_HEIGHT / 2;
          const dist = Math.abs(itemCenter - center);
          if (dist < closestDist) {
            closestDist = dist;
            closestIndex = idx;
          }
        });

        items.forEach((item, idx) => {
          item.classList.toggle("active", idx === closestIndex);
        });

        updateSaveButtonState();

        const targetScroll =
          items[closestIndex].offsetTop + ITEM_HEIGHT / 2 - pickerWindow.clientHeight / 2;
        pickerWindow.scrollTo({ top: targetScroll, behavior: "smooth" });
      }

      pickerWindow.addEventListener("scroll", function () {
        if (scrollTimer !== null) {
          clearTimeout(scrollTimer);
        }
        scrollTimer = setTimeout(updateActiveFromScroll, TIMING.SCROLL_DEBOUNCE_MS);
      });

      // Modal öffnen
      AppModal.open({
        title: "Heart Rate Range",
        subline: labelText,
        content: container,
        saveLabel: "Save",
        cancelLabel: "Cancel",
        onSave: () => {
          // Defensive check: only save if button is enabled
          if (!saveButton || saveButton.disabled) {
            return false;
          }

          const chosen = getCurrentValue();

          if (mode === "min") {
            const maxAllowed = currentMax - 10;
            const clamped = clamp(chosen, HARD_MIN, maxAllowed);
            hrMinDisplay.textContent = clamped;
            
            // Call Arduino callback
            if (typeof window.onMinHeartRateChanged === "function") {
              window.onMinHeartRateChanged(clamped);
            }
          } else {
            const minAllowed = currentMin + 10;
            const clamped = clamp(chosen, minAllowed, HARD_MAX);
            hrMaxDisplay.textContent = clamped;
            
            // Call Arduino callback
            if (typeof window.onMaxHeartRateChanged === "function") {
              window.onMaxHeartRateChanged(clamped);
            }
          }

          AppModal.close();
          return true;
        },
        onCancel: () => {
          // Cancel handler: no changes are saved (AppModal handles closing)
          // This ensures the initial value is preserved
        },
      });

      // Get Save button reference after modal is rendered
      // Use setTimeout to ensure DOM is fully rendered
      setTimeout(() => {
        // Find Save button in modal footer
        // AppModal creates Save button with btn-cta class by default
        const modalFooter = document.querySelector(".app-modal-footer");
        if (modalFooter) {
          const buttons = modalFooter.querySelectorAll("button");
          // Save button is the one with btn-cta class (AppModal default for save type)
          // or the last button if no btn-cta found (fallback)
          saveButton = Array.from(buttons).find(
            (btn) => btn.classList.contains("btn-cta")
          ) || (buttons.length > 0 ? buttons[buttons.length - 1] : null);

          if (saveButton) {
            // Set initial state: inactive
            saveButton.disabled = true;
            saveButton.classList.remove("btn-cta", "btn-primary");
            saveButton.classList.add("btn-inactive");
          }
        }

        // Center on initial value
        centerOnValue(initialValue);
      }, 0);

    }

  /**
   * Updates the displayed heart rate value.
   * Clamps the value to valid range (0-300) before updating the display.
   * 
   * @param {number} value - Heart rate value (0-300)
   * @returns {boolean} True if update was successful, false if element not found or value invalid
   */
  function updateHeartRate(value) {
    if (!hrValueDisplay) {
      return false;
    }

    const numValue = Number(value);
    if (!Number.isFinite(numValue)) {
      return false;
    }

    const clamped = clamp(numValue, HR_MIN_VALUE, HR_MAX_VALUE);
    const rounded = Math.round(clamped);
    hrValueDisplay.textContent = rounded;
    
    // Call Arduino callback
    if (typeof window.onHeartRateChanged === "function") {
      window.onHeartRateChanged(rounded);
    }
    
    return true;
  }

  /**
   * Updates the displayed sensor name.
   * 
   * @param {string} name - Sensor name to display
   * @returns {boolean} True if update was successful, false if element not found
   */
  function updateSensorName(name) {
    if (!hrSensorNameDisplay) {
      return false;
    }

    if (typeof name !== "string") {
      return false;
    }

    const trimmedName = name.trim() || "Unknown";
    hrSensorNameDisplay.textContent = trimmedName;
    
    // Call Arduino callback
    if (typeof window.onSensorNameChanged === "function") {
      window.onSensorNameChanged(trimmedName);
    }
    
    return true;
  }

  // Public API
  const HeartRateManager = {
    open: openHeartRatePicker,
    updateHeartRate,
    updateSensorName,
  };

  // Expose to global scope
  window.HeartRateManager = HeartRateManager;
  window.hrPicker = { open: openHeartRatePicker }; // Backward compatibility

  /**
   * Saves heart rate setting (min or max) to the server
   * @param {string} type - "min" or "max"
   * @param {number} value - Heart rate value to save
   * @returns {Promise<void>}
   * @private
   */
  function saveHeartRateSetting(type, value) {
    const endpoint = `/api/v1/action/heartrate/set${type}`;
    const formData = new URLSearchParams();
    formData.append("value", value.toString());
    
    return fetch(endpoint, {
      method: "POST",
      headers: { "Content-Type": "application/x-www-form-urlencoded" },
      body: formData
    })
      .then(response => {
        if (!response.ok) {
          throw new Error(`Failed to save ${type} heart rate`);
        }
        return response.json();
      })
      .then(data => {
        if (data.min !== undefined && data.max !== undefined) {
          hrMinDisplay.textContent = data.min;
          hrMaxDisplay.textContent = data.max;
        }
      })
      .catch(error => {
        // Silent error handling
      });
  }

  /**
   * Sets up JavaScript callbacks to save heart rate settings to the server
   * @returns {void}
   * @private
   */
  function setupHeartRateCallbacks() {
    window.onMinHeartRateChanged = (value) => saveHeartRateSetting("min", value);
    window.onMaxHeartRateChanged = (value) => saveHeartRateSetting("max", value);
  }

  // Initialize event listeners when DOM is ready
  // NOTE: Heart rate settings are now loaded via WebSocket (HEART_RATE_SETTINGS frame)
  // No HTTP GET request needed - data comes automatically on WebSocket connect
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", function() {
      initializeHeartRateListeners();
      setupHeartRateCallbacks();
    });
  } else {
    initializeHeartRateListeners();
    setupHeartRateCallbacks();
  }

  /**
   * Initializes click handlers for heart rate range pickers.
   * Attaches event listeners to min and max heart rate range elements.
   * 
   * @returns {void}
   * @private
   */
  function initializeHeartRateListeners() {
    document.querySelectorAll(".hr-range").forEach((el) => {
      el.addEventListener("click", () => {
        const type = el.getAttribute("data-type");
        if (type === "min" || type === "max") {
          openHeartRatePicker(type);
        }
      });
    });
  }
  })();

/**
 * ===============================================================
 * FanManager
 * ===============================================================
 * Manages fan tokens, fan cards, and fan-related UI operations.
 * Provides public API for ESP32/Arduino integration.
 *
 * Public API:
 * - fanManager.addFanFromToken(token, options) - Add a fan
 * - fanManager.removeFanByToken(token) - Remove by token
 * - fanManager.removeFanByIndex(index) - Remove by index
 * - fanManager.removeAllFans() - Remove all fans
 * - fanManager.restoreFans(fansArray) - Restore all fans from stored values (for Arduino initialization)
 * - fanManager.getFanCount() - Get fan count
 * - fanManager.getTokens() - Get all tokens
 * - fanManager.refreshAddButtonState() - Refresh button state
 * - fanManager.updateFanToken(oldToken, newToken) - Update token
 * - fanManager.setFanStatus(token, participationState) - Update fan participation state
 *   - participationState: "ACTIVE" | "INACTIVE" | "ERROR"
 *
 * Arduino Integration Callbacks:
 * - window.onFanAdded(token, options) - Called when a fan is successfully added
 *   - token: string - The fan token that was added
 *   - options: object - Options object with {participationState: "ACTIVE"|"INACTIVE"|"ERROR"}
 * - window.onFanRemoved(token) - Called when a fan is successfully removed
 *   - token: string - The fan token that was removed
 * - window.onFanUpdated(oldToken, newToken) - Called when a fan token is updated
 *   - oldToken: string - The previous token value
 *   - newToken: string - The new token value
 *
 * Notes:
 * - Tokens are stored in window.fanTokens and synced with DOM
 * - Tokens displayed as first 6 chars + "…" but stored as full 32-char hex
 * - MAX_FANS limits the number of fans that can be added
 * - Button styling: btn-cta (0 fans), btn-primary (1-3 fans), btn-inactive (max fans)
 * - Fan participation states (mapped to toggle switch):
 *   - ACTIVE: Toggle switch ON (green background, white circle on right)
 *   - INACTIVE: Toggle switch OFF (gray background, green circle on left)
 *   - ERROR: Toggle switch ERROR (gray background, pink circle centered)
 * ===============================================================
*/
(function () {
  const MAX_FANS = 4;
  const TOKEN_LENGTH = 32;
  const TOKEN_REGEX = /^[0-9a-zA-Z]{32}$/;

  // Global token list
  window.fanTokens = [];

  /**
   * Debounce utility function - delays function execution until after wait time
   * @param {Function} func - Function to debounce
   * @param {number} wait - Wait time in milliseconds
   * @returns {Function} Debounced function
   */
  function debounce(func, wait) {
    let timeout;
    return function executedFunction(...args) {
      const later = () => {
        clearTimeout(timeout);
        func(...args);
      };
      clearTimeout(timeout);
      timeout = setTimeout(later, wait);
    };
  }

  const addFanButton = document.querySelector("#btn-add-fan");
  const fanGrid = document.querySelector(".fan-grid");

  if (!addFanButton || !fanGrid) {
    return;
  }

  /**
   * Gets the current number of fan cards in the grid.
   * 
   * @returns {number} Number of fan cards currently displayed
   * @private
   */
  function getFanCount() {
    return fanGrid.querySelectorAll(".fan-card").length;
  }

  /**
   * Rebuilds the token list from DOM elements.
   * Synchronizes window.fanTokens with the current state of fan cards in the DOM.
   * 
   * @returns {void}
   * @private
   */
  function rebuildTokenListFromDOM() {
    window.fanTokens = Array.from(fanGrid.querySelectorAll(".fan-card"))
      .map((card) => card.getAttribute("data-token") || "")
      .filter(Boolean);
  }

  /**
   * Updates the Add Fan button state and styling based on fan count.
   * Applies btn-cta (0 fans), btn-primary (1-3 fans), or btn-inactive (max fans) styling.
   * 
   * @returns {void}
   * @private
   */
  function updateAddFanButtonState() {
    const fanCount = getFanCount();
    const disabled = fanCount >= MAX_FANS;
    
    // Remove all hierarchy classes
    addFanButton.classList.remove("btn-cta", "btn-primary", "btn-inactive");
    
    // Apply appropriate class based on fan count
    if (fanCount === 0) {
      // No fans added - use CTA style
      addFanButton.classList.add("btn-cta");
    } else if (disabled) {
      // Max fans reached - use inactive style
      addFanButton.classList.add("btn-inactive");
    } else {
      // Fans exist but not max - use primary style
      addFanButton.classList.add("btn-primary");
    }
    
    addFanButton.disabled = disabled;
  }

  /**
   * Checks if a token is already in use.
   * Rebuilds token list from DOM before checking to ensure accuracy.
   * 
   * @param {string} token - Token to check
   * @param {string} [excludeToken] - Token to exclude from check (e.g., current token when editing)
   * @returns {boolean} True if token is already in use, false otherwise
   * @private
   */
  function isTokenInUse(token, excludeToken) {
    if (!token) return false;
    const trimmed = token.trim().toLowerCase();
    const exclude = excludeToken ? excludeToken.trim().toLowerCase() : null;
    
    // Rebuild token list from DOM to ensure it's up to date
    rebuildTokenListFromDOM();
    
    return window.fanTokens.some((existingToken) => {
      const existing = (existingToken || "").trim().toLowerCase();
      return existing === trimmed && existing !== exclude;
    });
  }

  /**
   * Validates a fan token.
   * Checks length (32 characters), format (hexadecimal characters only), and optionally duplicates.
   * 
   * @param {string} token - Token to validate
   * @param {Object} [options={}] - Validation options
   * @param {string} [options.excludeToken] - Token to exclude from duplicate check (for edit mode)
   * @param {boolean} [options.allowEmpty=false] - If true, empty input returns no error (for real-time validation)
   * @param {boolean} [options.checkDuplicates=false] - If true, checks if token is already in use
   * @param {boolean} [options.checkMaxFans=false] - If true, checks if max fans limit is reached
   * @returns {{valid: boolean, value?: string, message?: string}} Validation result object
   * @private
   */
  function validateToken(token, options = {}) {
    const { excludeToken = null, allowEmpty = false, checkDuplicates = false, checkMaxFans = false } = options;
    const trimmed = (token || "").trim();
    
    if (trimmed.length === 0) {
      return { valid: false, message: allowEmpty ? "" : "Please enter a token." };
    }
    
    if (trimmed.length !== TOKEN_LENGTH) {
      return {
        valid: false,
        message: `Token must be exactly ${TOKEN_LENGTH} characters.`,
      };
    }
    
    if (!TOKEN_REGEX.test(trimmed)) {
      return {
        valid: false,
        message: "Token may contain only hexadecimal characters.",
      };
    }
    
    if (checkDuplicates && isTokenInUse(trimmed, excludeToken)) {
      return {
        valid: false,
        message: "Token already in use.",
      };
    }
    
    if (checkMaxFans && getFanCount() >= MAX_FANS) {
      return {
        valid: false,
        message: "Maximum number of fans reached.",
      };
    }
    
    return { valid: true, value: trimmed };
  }

  /**
   * Shortens token for display.
   * Returns first 6 characters followed by ellipsis, or full token if shorter than 6 characters.
   * 
   * @param {string} token - Full token string
   * @returns {string} Shortened token for display
   * @private
   */
  function shortenToken(token) {
    if (!token || token.length < 6) return token || "";
    return token.substring(0, 6) + "…";
  }

  /**
   * Renumbers fan names sequentially.
   * Updates all fan card names to "Fan 1", "Fan 2", etc. based on their order in the grid.
   * 
   * @returns {void}
   * @private
   */
  function renumberFanNames() {
    fanGrid.querySelectorAll(".fan-card").forEach((card, idx) => {
      const nameEl = card.querySelector(".fan-name");
      if (nameEl) {
        nameEl.textContent = "Fan " + (idx + 1);
      }
    });
  }

  /**
   * Updates the participation state of a fan card.
   * Finds the fan card by token and updates its toggle switch state.
   * 
   * @param {string} token - Fan token to update
   * @param {string} state - Participation state ("ACTIVE", "INACTIVE", or "ERROR")
   * @returns {boolean} True if update was successful, false if token not found or invalid
   */
  function setFanStatus(token, state) {
    if (!token || typeof token !== "string") {
      return false;
    }

    const searchToken = token.trim().toLowerCase();
    const cards = fanGrid.querySelectorAll(".fan-card");
    
    for (const card of cards) {
      const storedToken = (card.getAttribute("data-token") || "").toLowerCase();
      if (storedToken === searchToken) {
        const switchEl = card.querySelector(".switch");
        const inputEl = switchEl ? switchEl.querySelector("input[type='checkbox']") : null;
        
        if (switchEl && inputEl) {
          // Remove error class first
          switchEl.classList.remove("error");
          switchEl.classList.remove("recovering");
          switchEl.removeAttribute("title");
          
          // Set state based on participation state
          if (state === "ACTIVE") {
            inputEl.checked = true;
          } else if (state === "INACTIVE") {
            inputEl.checked = false;
          } else if (state === "ERROR") {
            inputEl.checked = false;
            switchEl.classList.add("error");
          } else {
            return false;
          }
          return true;
        }
      }
    }

    return false;
  }

  /**
   * Updates fan recovery UI state (tooltip + disabled).
   * recovery: 0=normal, 1=recovering (auto), 2=exhausted (needs user action)
   */
  function setFanRecoveryState(token, recovery) {
    if (!token || typeof token !== "string") return false;
    const searchToken = token.trim().toLowerCase();
    const cards = fanGrid.querySelectorAll(".fan-card");
    for (const card of cards) {
      const storedToken = (card.getAttribute("data-token") || "").toLowerCase();
      if (storedToken !== searchToken) continue;

      const switchEl = card.querySelector(".switch");
      const inputEl = switchEl ? switchEl.querySelector("input[type='checkbox']") : null;
      if (!switchEl || !inputEl) return false;

      // Store recovery state on input (so change handler can respect it)
      inputEl.dataset.recovery = String(recovery || 0);

      // Visual + tooltip
      switchEl.classList.remove("recovering");
      if ((recovery | 0) === 1) {
        switchEl.classList.add("recovering");
        switchEl.setAttribute("title", "Recovering…");
      } else if ((recovery | 0) === 2) {
        // Exhausted: user action required, do NOT disable the switch
        switchEl.setAttribute("title", "Auto-recovery failed. Toggle to retry.");
      } else {
        switchEl.removeAttribute("title");
      }

      // Disabled logic: in-flight and cooldown are handled by change handler; recovery(1) forces disable.
      if ((recovery | 0) === 1) {
        inputEl.disabled = true;
      } else {
        // Only re-enable if not in-flight and not cooling down
        const now = Date.now();
        const inFlight = inputEl.dataset.inFlight === "1";
        const until = parseInt(inputEl.dataset.cooldownUntil || "0", 10) || 0;
        if (!inFlight && now >= until) inputEl.disabled = false;
      }
      return true;
    }
    return false;
  }

  /**
   * Updates a fan token in both UI and global list.
   * Updates the DOM card's data-token attribute and display text, and synchronizes window.fanTokens.
   * 
   * @param {string} oldToken - Old token value
   * @param {string} newToken - New token value
   * @returns {void}
   * @private
   */
  function updateFanTokenInUI(oldToken, newToken) {
    const oldLower = (oldToken || "").toLowerCase();
    const newTrimmed = (newToken || "").trim();

    // Update DOM card
    const cards = fanGrid.querySelectorAll(".fan-card");
    cards.forEach((card) => {
      const stored = (card.getAttribute("data-token") || "").toLowerCase();
      if (stored === oldLower) {
        card.setAttribute("data-token", newTrimmed);
        const tokenEl = card.querySelector(".fan-token");
        if (tokenEl) {
          tokenEl.textContent = "Token: " + shortenToken(newTrimmed);
        }
      }
    });

    // Update global list
    window.fanTokens = window.fanTokens.map((t) =>
      (t || "").toLowerCase() === oldLower ? newTrimmed : t
    );
  }

  /**
   * Creates a new fan card element and adds it to the grid.
   * Automatically renumbers fan names, updates button state, and calls Arduino callback.
   * 
   * @param {string} token - Fan token (32-character hexadecimal string)
   * @param {Object} [opts={}] - Options object
   * @param {string} [opts.participationState="INACTIVE"] - Initial participation state ("ACTIVE", "INACTIVE", or "ERROR")
   * @returns {HTMLElement} Created fan card element
   * @private
   */
  function createFanCard(token, opts = {}) {
    const fanName = "Fan " + (getFanCount() + 1);
    // Default to INACTIVE if no state specified
    const participationState = opts.participationState || "INACTIVE";
    
    // Determine initial toggle state
    let isChecked = false;
    let hasError = false;
    if (participationState === "ACTIVE") {
      isChecked = true;
    } else if (participationState === "ERROR") {
      hasError = true;
    }

    const card = document.createElement("div");
    card.className = "fan-card";
    card.setAttribute("data-token", token);

    const switchClass = hasError ? "switch error" : "switch";
    
    card.innerHTML = `
      <div class="fan-art">
        <svg xmlns="http://www.w3.org/2000/svg" width="114" height="114" fill="none" viewBox="0 0 114 114"><g id="FanBlades-${token.substring(0, 8)}"><path fill="#b5b5b5" d="M103.212 61.865c-3.686-2.379-7.473-4.406-11.477-6.243-2.873-1.319-5.705-2.327-8.737-3.254-3.378-1.032-6.716-1.657-10.22-2.032-1.878-.203-3.647-.367-5.518-.312-.055 0-.086-.027-.13-.04a.53.53 0 0 1 .21-.206c9.809-5.29 20.306-7.118 31.325-7.453 1.15-.035 2.127-.604 2.716-1.629.455-.795.489-1.831.114-2.851-1.718-4.663-4.262-8.909-7.466-12.722-1.099-1.308-2.73-1.81-4.36-1.263-4.16 1.398-8.105 3.096-12.038 5.08-2.823 1.425-5.376 3.01-7.991 4.802-2.915 1.998-5.483 4.218-7.961 6.722-1.33 1.342-2.562 2.623-3.685 4.12a.5.5 0 0 1-.105.1.52.52 0 0 1-.037-.317c1.981-10.965 7.096-20.315 13.704-29.139.69-.92.854-2.039.42-3.138-.337-.85-1.126-1.525-2.16-1.868C65.1 8.658 60.193 8 55.216 8.127c-1.708.044-3.12 1.006-3.707 2.621-1.5 4.124-2.633 8.266-3.533 12.58-.646 3.095-.998 6.08-1.227 9.242-.255 3.523-.12 6.915.292 10.415.22 1.876.454 3.637.923 5.45a.5.5 0 0 1 .014.144.52.52 0 0 1-.27-.169c-7.34-8.386-11.459-18.213-14.238-28.882-.29-1.113-1.062-1.939-2.193-2.285-.876-.266-1.894-.069-2.805.524-4.164 2.711-7.738 6.137-10.742 10.109-1.03 1.362-1.159 3.065-.262 4.531 2.29 3.743 4.822 7.213 7.632 10.605 2.017 2.435 4.13 4.571 6.46 6.722 2.596 2.396 5.333 4.407 8.325 6.266 1.605.997 3.127 1.913 4.837 2.676.05.023.066.062.1.092a.53.53 0 0 1-.28.094c-11.131.51-21.382-2.397-31.456-6.876-1.05-.467-2.178-.38-3.153.29-.755.518-1.237 1.438-1.341 2.521-.476 4.946-.025 9.876 1.207 14.7.422 1.657 1.674 2.817 3.38 3.03 4.354.545 8.645.728 13.05.646 3.162-.06 6.149-.38 9.284-.86 3.491-.535 6.769-1.422 10.088-2.602 1.78-.633 3.446-1.253 5.107-2.114a.5.5 0 0 1 .139-.046.5.5 0 0 1-.105.302c-6.54 9.02-15.206 15.224-24.988 20.307-1.021.53-1.656 1.467-1.74 2.646-.066.914.352 1.863 1.135 2.62 3.571 3.455 7.706 6.177 12.246 8.221 1.558.702 3.246.446 4.476-.753 3.14-3.065 5.96-6.306 8.64-9.8 1.927-2.508 3.537-5.044 5.118-7.795 1.758-3.063 3.11-6.178 4.257-9.51.615-1.787 1.168-3.475 1.531-5.31.01-.05.043-.07.062-.109a.5.5 0 0 1 .16.242c2.974 10.739 2.42 21.382.296 32.199-.222 1.128.114 2.209.982 3.009.673.621 1.677.887 2.755.746 4.928-.637 9.635-2.172 14.064-4.447 1.52-.78 2.373-2.26 2.202-3.97-.439-4.366-1.215-8.59-2.274-12.866-.761-3.07-1.739-5.911-2.905-8.86-1.298-3.285-2.892-6.282-4.782-9.257-1.013-1.594-1.987-3.08-3.196-4.51a.5.5 0 0 1-.075-.124.53.53 0 0 1 .319.034c10.25 4.37 18.226 11.439 25.358 19.843.744.877 1.798 1.288 2.967 1.108.904-.14 1.737-.759 2.3-1.69 2.574-4.25 4.308-8.887 5.293-13.77.338-1.674-.288-3.264-1.732-4.196v.003zm-46.283-4.819-.08-.085.017-.116.102-.06.11.044.035.112-.066.097-.116.01z"/><path stroke="#d8d8d8" stroke-miterlimit="10" d="M6.376 57.001h100.699M6.888 64.166l99.675-14.33M8.415 71.186l96.621-28.37M10.926 77.917l91.599-41.832M14.368 84.222 99.083 29.78M18.674 89.973l76.103-65.944M23.754 95.053l65.943-76.104M29.505 99.358l54.442-84.714M35.81 102.801l41.832-91.6M42.54 105.311l28.37-96.62M49.56 106.839 63.89 7.163M56.726 107.351V6.651M63.89 106.839 49.56 7.163M70.91 105.311 42.54 8.691M77.642 102.801l-41.833-91.6M83.947 99.358 29.505 14.644M89.697 95.053 23.754 18.949M94.777 89.973 18.674 24.029M99.083 84.222 14.368 29.78M102.525 77.917l-91.6-41.832M105.036 71.186l-96.62-28.37M106.563 64.166 6.888 49.836M82.849 55.133l24.386-1.743M6.216 60.612l24.426-1.745M82.318 51.435l23.889-5.196M7.244 67.763l23.93-5.205M81.265 47.851l22.908-8.542M9.278 74.693l22.946-8.556M79.714 44.452l21.459-11.714M12.278 81.264l21.495-11.733M77.694 41.31l19.574-14.65M16.183 87.342l19.606-14.673M75.248 38.485 92.538 21.2M20.912 92.802l17.32-17.314M72.425 36.038 87.08 16.469M26.37 97.533l14.68-19.601M69.283 34.017l11.722-21.455M32.446 101.44l11.742-21.49M65.884 32.465 74.435 9.56M39.016 104.442 47.58 81.5M62.3 31.41l5.204-23.888M45.947 106.48l5.212-23.929M58.603 30.878l1.752-24.386M53.096 107.51l1.755-24.427M54.867 30.877 53.132 6.49M60.32 107.512 58.58 83.084M51.168 31.407 45.981 7.515M67.47 106.487l-5.196-23.931M47.583 32.458 39.05 9.548M74.402 104.454l-8.548-22.948M44.185 34.008 32.477 12.546M80.974 101.456 69.247 79.958M41.04 36.027 26.4 16.447M87.052 97.555 72.386 77.942M38.216 38.472 20.937 21.175M92.514 92.827 75.206 75.502M35.768 41.294 16.203 26.632M97.248 87.37 77.65 72.683M33.746 44.435l-21.45-11.729M101.156 81.296 79.669 69.547M32.192 47.834 9.291 39.275M104.16 74.727l-22.94-8.573M31.137 51.418 7.251 46.205M106.2 67.797l-23.926-5.221M30.603 55.115 6.218 53.354M107.233 60.648l-24.425-1.764"/></g><path stroke="#d8d8d8" stroke-linecap="round" stroke-linejoin="round" d="M56.726 83.554c14.418 0 26.106-11.688 26.106-26.106S71.144 31.342 56.726 31.342 30.618 43.03 30.618 57.448s11.688 26.106 26.106 26.106"/><path fill="#fffdfd" d="M56.5 69C62.851 69 68 63.851 68 57.5S62.851 46 56.5 46 45 51.149 45 57.5 50.149 69 56.5 69"/><path stroke="url(#paint0_linear_477_2350_${token.substring(0, 8)})" stroke-linecap="round" stroke-linejoin="round" stroke-width="2" d="M57 113c30.928 0 56-25.072 56-56S87.928 1 57 1 1 26.072 1 57s25.072 56 56 56"/><path stroke="#e6e6e6" stroke-linecap="round" stroke-linejoin="round" d="M57 110.255c29.412 0 53.255-23.843 53.255-53.255S86.412 3.745 57 3.745 3.745 27.588 3.745 57 27.588 110.255 57 110.255"/><path stroke="#e0e0e0" stroke-linecap="round" stroke-linejoin="round" d="M57 107.367c27.896 0 50.51-22.614 50.51-50.51S84.896 6.347 57 6.347 6.49 28.961 6.49 56.857s22.614 50.51 50.51 50.51"/><defs><linearGradient id="paint0_linear_477_2350_${token.substring(0, 8)}" x1="57" y1="1" x2="57" y2="113" gradientUnits="userSpaceOnUse"><stop stop-color="#EFEFEF"/><stop offset="1" stop-color="#BABABA"/></linearGradient></defs></svg>
      </div>
      <div class="fan-meta">
        <label class="${switchClass}">
          <input type="checkbox" ${isChecked ? 'checked' : ''}>
          <span class="slider round"></span>
        </label>
        <div>
          <div class="fan-name">${fanName}</div>
          <div class="fan-token">Token: ${shortenToken(token)}</div>
        </div>
      </div>
    `;
    
    // Store fan ID for API v1 (first 8 chars of token)
    card.setAttribute("data-fan-id", token.substring(0, 8));

    fanGrid.appendChild(card);

    // Sync token list with DOM
    rebuildTokenListFromDOM();
    renumberFanNames();
    updateAddFanButtonState();
    
    // Call Arduino callback
    if (typeof window.onFanAdded === "function") {
      window.onFanAdded(token, opts);
    }
    
    return card;
  }

  /**
   * Validates token input in real-time for Add Fan modal.
   * Checks length, format, duplicates, and maximum fan count.
   * 
   * @param {string} value - Token value to validate
   * @returns {{valid: boolean, message?: string}} Validation result object
   * @private
   */
  function validateTokenInput(value) {
    return validateToken(value, {
      allowEmpty: true,
      checkDuplicates: true,
      checkMaxFans: true
    });
  }

  /**
   * Validates token input for edit modal in real-time.
   * Excludes oldToken from duplicate check to allow keeping the same token.
   * 
   * @param {string} value - Token value to validate
   * @param {string} oldToken - Original token value to exclude from duplicate check
   * @returns {{valid: boolean, message?: string}} Validation result object
   * @private
   */
  function validateTokenInputForEdit(value, oldToken) {
    return validateToken(value, {
      excludeToken: oldToken,
      allowEmpty: true,
      checkDuplicates: true
    });
  }

  /**
   * Opens the Add Fan modal.
   * Creates modal with token input field, real-time validation, and dynamic save button state.
   * Save button starts inactive and becomes active (CTA) when valid token is entered.
   * 
   * @returns {void}
   */
  function openAddFanModal() {
    if (getFanCount() >= MAX_FANS) return;

    const container = document.createElement("div");
    container.innerHTML = `
      <input
        type="text"
        class="fan-token-input fan-token-input-full"
        placeholder="${TOKEN_LENGTH}-char hex token"
        maxlength="${TOKEN_LENGTH}"
        autocomplete="off"
        spellcheck="false"
      />
      <div class="fan-modal-error" aria-live="polite"></div>
    `;

    const tokenInput = container.querySelector(".fan-token-input");
    const errorEl = container.querySelector(".fan-modal-error");
    let saveButton = null;

    /**
     * Shows error message in the modal error element.
     * 
     * @param {string} msg - Error message to display
     * @returns {void}
     * @private
     */
    function showError(msg) {
      if (msg) {
      errorEl.textContent = msg;
        errorEl.classList.add("is-visible");
      } else {
        errorEl.textContent = "";
        errorEl.classList.remove("is-visible");
      }
    }

    /**
     * Clears error message from the modal error element.
     * 
     * @returns {void}
     * @private
     */
    function clearError() {
      errorEl.textContent = "";
      errorEl.classList.remove("is-visible");
    }

    /**
     * Updates Save button state based on token validation.
     * Enables/disables button and applies appropriate styling.
     * 
     * @returns {void}
     * @private
     */
    function updateSaveButtonState() {
      if (!saveButton) return;
      const validation = validateTokenInput(tokenInput.value);
      const isValid = validation.valid;
      
      saveButton.disabled = !isValid;
      if (isValid) {
        saveButton.classList.remove("btn-inactive");
      } else {
        saveButton.classList.add("btn-inactive");
      }
    }

    // Real-time validation on input (debounced for performance)
    const handleInput = debounce(() => {
      const validation = validateTokenInput(tokenInput.value);
      if (validation.message) {
        showError(validation.message);
      } else {
        clearError();
      }
      updateSaveButtonState();
    }, TIMING.INPUT_DEBOUNCE_MS);

    tokenInput.addEventListener("input", handleInput);

    setTimeout(() => {
      tokenInput.focus();
      // Get save button reference after modal is opened and buttons are created
      const footer = document.querySelector(".app-modal-footer");
      if (footer) {
        saveButton = footer.querySelector('button.btn-cta, button:not(.btn-tertiary)');
        if (saveButton) {
          saveButton.disabled = true;
          saveButton.classList.add("btn-inactive");
        }
      }
    }, 0);

    AppModal.open({
      title: "Add Fan",
      subline: "Enter a 32-character fan token",
      content: container,
      saveLabel: "Save",
      cancelLabel: "Cancel",
      showCancel: true,
      onSave: () => {
        clearError();
        const result = validateToken(tokenInput.value);

        if (!result.valid) {
          showError(result.message);
          updateSaveButtonState();
          return false;
        }

        if (isTokenInUse(result.value)) {
          showError("Token already in use.");
          updateSaveButtonState();
          return false;
        }

        if (getFanCount() >= MAX_FANS) {
          showError("Maximum number of fans reached.");
          updateSaveButtonState();
          return false;
        }

        // Call API to add fan (persist to NVS and start discovery)
        const token = result.value;
        saveButton.disabled = true;
        
        window.ApiV1.addFan(token)
          .then(apiResult => {
            saveButton.disabled = false;
            if (apiResult.ok) {
              // API call succeeded
              // NOTE: WebSocket FANS_SNAPSHOT may have already created the card
              // Only create if it doesn't exist yet (prevents duplicate cards)
              // Initial state is ERROR because fan is not yet discovered/connected
              if (!isTokenInUse(token)) {
                createFanCard(token, { participationState: "ERROR" });
              }
              
              // Call Arduino callback (ERROR = not yet connected)
              if (typeof window.onFanAdded === "function") {
                window.onFanAdded(token, { participationState: "ERROR" });
              }

              tokenInput.value = "";
              clearError();
              AppModal.close();
            } else {
              // API call failed
              showError(apiResult.error || 'Failed to add fan');
              updateSaveButtonState();
            }
          })
          .catch(err => {
            saveButton.disabled = false;
            showError('Network error: ' + (err.message || 'Unknown error'));
            updateSaveButtonState();
          });
        
        return false; // Don't close modal yet, wait for async result
      },
      onCancel: () => {
        tokenInput.value = "";
        clearError();
      },
    });
  }

  /**
   * Opens the Edit/Delete Fan modal.
   * Provides real-time validation and dynamic button states:
   * - Save button starts inactive, becomes CTA when token is changed and valid
   * - Delete button changes to tertiary when Save becomes CTA
   * - Real-time validation with error messages
   * - Cancel resets input to original token value
   * 
   * @param {HTMLElement} cardEl - Fan card element to edit
   * @returns {void}
   */
  function openEditFanModal(cardEl) {
    const oldToken = cardEl.getAttribute("data-token") || "";
    const container = document.createElement("div");

    const label = document.createElement("div");
    label.className = "hr-modal-label";
    label.textContent = "Edit Fan Token";

    const input = document.createElement("input");
    input.type = "text";
    input.value = oldToken;
    input.maxLength = TOKEN_LENGTH;
    input.className = "fan-token-input fan-token-input-full";
    input.placeholder = `${TOKEN_LENGTH}-char hex token`;
    input.autocomplete = "off";
    input.spellcheck = false;

    const errorEl = document.createElement("div");
    errorEl.className = "fan-modal-error";
    errorEl.setAttribute("aria-live", "polite");

    container.appendChild(label);
    container.appendChild(input);
    container.appendChild(errorEl);

    let saveButton = null;
    let deleteButton = null;

    /**
     * Shows error message in the modal error element.
     * 
     * @param {string} msg - Error message to display
     * @returns {void}
     * @private
     */
    function showError(msg) {
      if (msg) {
        errorEl.textContent = msg;
        errorEl.classList.add("is-visible");
      } else {
        errorEl.textContent = "";
        errorEl.classList.remove("is-visible");
      }
    }

    /**
     * Clears error message from the modal error element.
     * 
     * @returns {void}
     * @private
     */
    function clearError() {
      errorEl.textContent = "";
      errorEl.classList.remove("is-visible");
    }

    /**
     * Checks if the current token value has changed from the original.
     * 
     * @returns {boolean} True if token has changed, false otherwise
     * @private
     */
    function hasTokenChanged() {
      const currentValue = input.value.trim();
      const oldTrimmed = oldToken.trim();
      return currentValue.toLowerCase() !== oldTrimmed.toLowerCase();
    }

    /**
     * Updates Save and Delete button states based on token validation and change status.
     * Save button becomes CTA when token is changed and valid; Delete becomes tertiary when Save is CTA.
     * 
     * @returns {void}
     * @private
     */
    function updateButtonStates() {
      if (!saveButton || !deleteButton) return;

      const currentValue = input.value.trim();
      const hasChanged = hasTokenChanged();
      const validation = validateTokenInputForEdit(currentValue, oldToken);
      const isValid = validation.valid && hasChanged;

      // Update Save button
      if (isValid) {
        // Token changed and valid - make Save button CTA
        saveButton.disabled = false;
        saveButton.classList.remove("btn-inactive", "btn-primary");
        saveButton.classList.add("btn-cta");
      } else {
        // Token unchanged or invalid - make Save button inactive
        saveButton.disabled = true;
        saveButton.classList.remove("btn-cta", "btn-primary");
        saveButton.classList.add("btn-inactive");
      }

      // Update Delete button
      if (isValid) {
        // Save is CTA - make Delete tertiary
        deleteButton.classList.remove("btn-danger");
        deleteButton.classList.add("btn-tertiary");
      } else {
        // Save is inactive - make Delete danger
        deleteButton.classList.remove("btn-tertiary");
        deleteButton.classList.add("btn-danger");
      }
    }

    // Real-time validation on input (debounced for performance)
    const handleInput = debounce(() => {
      const validation = validateTokenInputForEdit(input.value, oldToken);
      if (validation.message) {
        showError(validation.message);
      } else {
        clearError();
      }
      updateButtonStates();
    }, TIMING.INPUT_DEBOUNCE_MS);

    input.addEventListener("input", handleInput);

    setTimeout(() => {
      input.focus();
      // Get button references after modal is opened and buttons are created
      const footer = document.querySelector(".app-modal-footer");
      if (footer) {
        const buttons = footer.querySelectorAll("button");
        buttons.forEach((btn) => {
          if (btn.id === "save" || btn.textContent.trim() === "Save") {
            saveButton = btn;
          } else if (btn.id === "delete" || btn.textContent.trim() === "Delete") {
            deleteButton = btn;
          }
        });
        
        // Initialize button states
        if (saveButton) {
          saveButton.disabled = true;
          saveButton.classList.remove("btn-primary", "btn-cta");
          saveButton.classList.add("btn-inactive");
        }
        if (deleteButton) {
          deleteButton.classList.remove("btn-tertiary");
          deleteButton.classList.add("btn-danger");
        }
      }
    }, 0);

    AppModal.open({
      title: "Edit Fan",
      subline: "Modify fan token or remove fan",
      content: container,
      buttons: [
        {
          id: "cancel",
          type: "cancel",
          label: "Cancel",
          className: "btn btn-regular btn-tertiary",
          closeOnClick: true,
          onClick: () => {
            // Reset input to original value on cancel
            input.value = oldToken;
            clearError();
          },
        },
        {
          id: "delete",
          type: "delete",
          label: "Delete",
          className: "btn btn-regular btn-danger",
          closeOnClick: true,
          onClick: () => {
            removeFanByToken(oldToken);
          },
        },
        {
          id: "save",
          type: "save",
          label: "Save",
          className: "btn btn-regular btn-inactive",
          closeOnClick: false,
          onClick: () => {
            clearError();
            const inputToken = input.value.trim();
            const validation = validateToken(inputToken);

            if (!validation.valid) {
              showError(validation.message);
              updateButtonStates();
              return false;
            }

            if (isTokenInUse(validation.value, oldToken)) {
              showError("Token already in use.");
              updateButtonStates();
              return false;
            }

            // Call API to update fan token (persist to NVS and start discovery)
            const newToken = validation.value;
            saveButton.disabled = true;
            
            window.ApiV1.updateFan(oldToken, newToken)
              .then(apiResult => {
                saveButton.disabled = false;
                if (apiResult.ok) {
                  // API call succeeded - update UI
                  updateFanTokenInUI(oldToken, newToken);
                  cardEl.setAttribute("data-token", newToken);
                  
                  // Call Arduino callback
                  if (typeof window.onFanUpdated === "function") {
                    window.onFanUpdated(oldToken, newToken);
                  }

                  AppModal.close();
                } else {
                  // API call failed
                  showError(apiResult.error || 'Failed to update fan');
                  updateButtonStates();
                }
              })
              .catch(err => {
                saveButton.disabled = false;
                showError('Network error: ' + (err.message || 'Unknown error'));
                updateButtonStates();
              });
            
            return false; // Don't close modal yet, wait for async result
          },
        },
      ],
    });
  }


  /**
   * Adds a fan from token (public API).
   * Validates token, checks maximum fan count, and creates fan card if valid.
   * 
   * @param {string} token - Fan token (32-character hexadecimal string)
   * @param {Object} [options={}] - Options object
   * @param {string} [options.participationState="INACTIVE"] - Initial participation state ("ACTIVE", "INACTIVE", or "ERROR")
   * @returns {{success: boolean, error?: string}} Result object with success status and optional error message
   */
  function addFanFromToken(token, options = {}) {
    const result = validateToken(token);
    if (!result.valid) {
      return { success: false, error: result.message };
    }

    if (getFanCount() >= MAX_FANS) {
      return { success: false, error: "Maximum number of fans reached." };
    }

    createFanCard(result.value, options);
    // Note: onFanAdded callback is called in createFanCard()
    return { success: true };
  }

  /**
   * Removes a fan by token (public API).
   * Finds fan card by token, removes it from DOM, and calls Arduino callback.
   * 
   * @param {string} token - Token to remove
   * @returns {boolean} True if fan was removed, false if token not found or invalid
   */
  function removeFanByToken(token) {
    const search = (token || "").trim().toLowerCase();
    if (!search) return false;

    // Find the card first to get the full token
    const cards = fanGrid.querySelectorAll(".fan-card");
    let cardToRemove = null;
    let removedToken = null;

    cards.forEach((card) => {
      if (cardToRemove) return;
      const stored = (card.getAttribute("data-token") || "").toLowerCase();
      if (stored === search) {
        removedToken = card.getAttribute("data-token"); // Get original case
        cardToRemove = card;
      }
    });

    if (!cardToRemove) {
      return false;
    }

    // Flag to ensure we only remove once (handles race conditions with WebSocket updates)
    let removed = false;
    
    // Helper to safely remove the card (only once)
    const doRemove = () => {
      if (removed) return false;
      
      // Check if card still exists and is a child of fanGrid
      if (!cardToRemove || !cardToRemove.parentNode || cardToRemove.parentNode !== fanGrid) {
        removed = true; // Mark as handled even if already gone
        return false;
      }
      
      try {
        fanGrid.removeChild(cardToRemove);
        removed = true;
        renumberFanNames();
        rebuildTokenListFromDOM();
        updateAddFanButtonState();
        return true;
      } catch (err) {
        // Card was already removed (race condition with WebSocket/restoreFans)
        removed = true;
        return false;
      }
    };

    // Call API to remove fan (sets speed to 0, turns off, deletes from NVS)
    const fanId = removedToken.substring(0, 8);
    window.ApiV1.removeFan(fanId)
      .then(apiResult => {
        const wasRemoved = doRemove();
        
        if (apiResult.ok) {
          // API call succeeded
          if (wasRemoved) {
            // Call Arduino callback only if we actually removed it
            if (typeof window.onFanRemoved === "function") {
              window.onFanRemoved(removedToken);
            }
          }
        } else {
          // Card was still removed from UI for user feedback
        }
      })
      .catch(err => {
        // Still try to remove from UI for user feedback
        doRemove();
      });

    return true; // Return true immediately (async removal)
  }

  /**
   * Removes a fan by index (public API).
   * Removes fan card at specified index, renumbers remaining fans, and calls Arduino callback.
   * 
   * @param {number} index - Zero-based index of fan to remove
   * @returns {boolean} True if fan was removed, false if index is invalid
   */
  function removeFanByIndex(index) {
    const cards = fanGrid.querySelectorAll(".fan-card");
    const idx = Number(index);

    if (Number.isNaN(idx) || idx < 0 || idx >= cards.length) return false;

    const card = cards[idx];
    const token = card.getAttribute("data-token");
    fanGrid.removeChild(card);

    renumberFanNames();
    rebuildTokenListFromDOM();
    updateAddFanButtonState();
    
    // Call Arduino callback
    if (typeof window.onFanRemoved === "function") {
      window.onFanRemoved(token);
    }
    
    return true;
  }

  /**
   * Removes all fans (public API).
   * Removes all fan cards from DOM and calls Arduino callback for each removed fan.
   * 
   * @returns {void}
   */
  function removeAllFans() {
    const tokens = [...window.fanTokens];
    fanGrid.querySelectorAll(".fan-card").forEach((card) =>
      fanGrid.removeChild(card)
    );
    rebuildTokenListFromDOM();
    updateAddFanButtonState();
    
    // Call Arduino callback for each removed fan
    if (typeof window.onFanRemoved === "function") {
      tokens.forEach((token) => {
        window.onFanRemoved(token);
      });
    }
  }

  /**
   * Restores all fans from stored values (for Arduino initialization).
   * Clears existing fans and adds all fans from the provided array.
   * This function is typically called by Arduino on page load to restore fans from Preferences/NVS.
   * 
   * @param {Array<Object>} fansArray - Array of fan objects to restore
   * @param {string} fansArray[].token - Fan token (32-character hexadecimal string)
   * @param {string} [fansArray[].participationState="INACTIVE"] - Initial participation state ("ACTIVE", "INACTIVE", or "ERROR")
   * @param {boolean} [suppressCallbacks=false] - If true, suppresses Arduino callbacks during restore
   * @returns {{success: boolean, restored: number, errors: Array<string>}} Result object with restore status
   * 
   * @example
   * // Arduino calls this on page load:
   * fanManager.restoreFans([
   *   { token: "412a2c36f14d0939238635f96f73d71c", participationState: "ACTIVE" },
   *   { token: "a1b2c3d4e5f6789012345678901234ab", participationState: "INACTIVE" }
   * ]);
   */
  function restoreFans(fansArray, suppressCallbacks = false) {
    if (!Array.isArray(fansArray)) {
      return { success: false, restored: 0, errors: ["Invalid input: expected array"] };
    }

    // Clear existing fans silently (without callbacks)
    const existingTokens = [...window.fanTokens];
    fanGrid.querySelectorAll(".fan-card").forEach((card) =>
      fanGrid.removeChild(card)
    );
    rebuildTokenListFromDOM();
    
    // Optionally call callbacks for removed fans
    if (!suppressCallbacks && typeof window.onFanRemoved === "function") {
      existingTokens.forEach((token) => {
        window.onFanRemoved(token);
      });
    }

    // Restore fans from array
    let restored = 0;
    const errors = [];

    for (const fan of fansArray) {
      if (!fan || typeof fan !== "object") {
        errors.push("Invalid fan object in array");
        continue;
      }

      const token = fan.token;
      if (!token || typeof token !== "string") {
        errors.push("Fan object missing or invalid token");
        continue;
      }

      const options = {
        participationState: fan.participationState || "INACTIVE", // Default to INACTIVE
      };

      // Temporarily suppress callback if requested
      let originalCallback = null;
      if (suppressCallbacks) {
        originalCallback = window.onFanAdded;
        window.onFanAdded = undefined;
      }

      const result = addFanFromToken(token, options);
      
      // Restore callback
      if (suppressCallbacks && originalCallback !== undefined) {
        window.onFanAdded = originalCallback;
      }

      if (result.success) {
        restored++;
      } else {
        errors.push(`Failed to restore fan ${token}: ${result.error || "Unknown error"}`);
      }
    }

    updateAddFanButtonState();

    return {
      success: errors.length === 0,
      restored: restored,
      errors: errors.length > 0 ? errors : undefined,
    };
  }

  /**
   * Initializes event listeners for fan management.
   * Attaches click handlers to Add Fan button and fan cards, and updates button state.
   * 
   * @returns {void}
   * @private
   */
  function initializeFanManager() {
    // Add Fan button handler
    addFanButton.addEventListener("click", (evt) => {
      evt.preventDefault();
      if (!addFanButton.disabled) openAddFanModal();
    });

    // Fan card click handler (for editing) - only trigger on fan-art clicks
    fanGrid.addEventListener("click", (evt) => {
      // Only open edit modal if clicking on fan-art element
      const fanArt = evt.target.closest(".fan-art");
      if (!fanArt) return;
      
      // Find the parent fan-card element
      const card = fanArt.closest(".fan-card");
      if (!card) return;
      openEditFanModal(card);
    });
    
    // Fan toggle switch handler (event delegation)
    fanGrid.addEventListener("change", (evt) => {
      if (evt.target.type === "checkbox" && evt.target.closest(".fan-card")) {
        const card = evt.target.closest(".fan-card");
        const token = card.getAttribute("data-token");
        if (!token) return;
        
        const fanId = token.substring(0, 8); // Use first 8 chars as ID
        const controlState = evt.target.checked ? "ACTIVE" : "INACTIVE";
        
        // If system is recovering this fan, ignore user toggles (should already be disabled)
        if ((evt.target.dataset.recovery | 0) === 1) {
          evt.target.checked = !evt.target.checked;
          return;
        }
        
        // Disable toggle while request is in flight + short cooldown to avoid backend throttle spam
        evt.target.dataset.inFlight = "1";
        evt.target.disabled = true;
        
        // Use POST action directly (same principle as Firmware Update: POST for actions, WebSocket for status)
        window.ApiV1.setFanControlState(fanId, controlState)
          .then(result => {
            delete evt.target.dataset.inFlight;
            // Cooldown (3s) after every action to prevent rapid toggling
            const cooldownMs = 3000;
            evt.target.dataset.cooldownUntil = String(Date.now() + cooldownMs);
            setTimeout(() => {
              const now = Date.now();
              const until = parseInt(evt.target.dataset.cooldownUntil || "0", 10) || 0;
              if (now >= until && (evt.target.dataset.recovery | 0) !== 1 && evt.target.dataset.inFlight !== "1") {
                evt.target.disabled = false;
              }
            }, cooldownMs);
            
            if (!result.ok) {
              // Revert toggle on error
              evt.target.checked = !evt.target.checked;
              
              // Check for 503 Service Unavailable (fan manager integration not available)
              const errorCode = result.error?.code;
              if (errorCode === 503 || errorCode === 'SERVICE_UNAVAILABLE' || result.error === "BUSY" || result.error === "NOT_READY") {
                // Show user-friendly message for service unavailable
                if (typeof window.showErrorModal === "function") {
                  window.showErrorModal({
                    message: 'Fan management is not available because the fan manager integration is not registered.',
                    reason: 'This is expected when running the basic example without integrations.'
                  }, {
                    title: 'Fan Management Unavailable'
                  });
                }
              } else {
                // Other errors: avoid modal spam; keep UX lightweight (cooldown + tooltip).
                const errorMsg = result.error?.message || result.error || 'Unknown error';
                const switchEl = card.querySelector(".switch");
                if (switchEl) switchEl.setAttribute("title", String(errorMsg));
              }
            }
          })
          .catch(err => {
            delete evt.target.dataset.inFlight;
            evt.target.disabled = false;
            evt.target.checked = !evt.target.checked;
            if (typeof window.showErrorModal === "function") {
              window.showErrorModal({
                message: `Failed to connect to the server: ${err.message || 'Unknown error'}`
              }, {
                title: 'Network Error'
              });
            }
          });
      }
    });

    updateAddFanButtonState();
  }

  /**
   * Update fan from API v1 data (for incremental updates)
   */
  function updateFromApi(id, fanData) {
    const token = fanData.token;
    if (!token) return;
    
    // Find fan card by token (first 8 chars match id)
    const cards = fanGrid.querySelectorAll(".fan-card");
    for (const card of cards) {
      const cardToken = card.getAttribute("data-token");
      if (cardToken && cardToken.substring(0, 8) === id) {
        // Update control state
        const controlState = fanData.controlState || "INACTIVE";
        setFanStatus(cardToken, controlState);
        
        // Update connected status if available
        if (fanData.connected !== undefined) {
          // Could add visual indicator for connection status if needed
        }
        break;
      }
    }
  }
  
  /**
   * Calculates fan speed from heart rate and min/max HR settings.
   * Linear interpolation between min and max HR.
   * 
   * @param {number} currentHR - Current heart rate
   * @param {number} minHR - Minimum heart rate setting
   * @param {number} maxHR - Maximum heart rate setting
   * @returns {number} Fan speed percentage (0-100)
   * @private
   */
  function calculateFanSpeed(currentHR, minHR, maxHR) {
    if (currentHR < minHR) {
      return 0;
    }
    if (currentHR >= maxHR) {
      return 100;
    }
    const range = maxHR - minHR;
    if (range === 0) return 0;
    const position = (currentHR - minHR) / range;
    return Math.round(position * 100);
  }

  /**
   * Fan animation system.
   * Animates fan blades based on toggle state and calculated fan speed.
   */
  (function () {
    const centerX = 57;
    const centerY = 57;
    const maxSpeed = 360; // degrees per second
    const accel = 400;
    const decel = 200;
    
    const fanAnimations = new Map(); // token -> { angle, speed, targetSpeed }
    let animationFrameId = null;
    let lastTime = performance.now();

    // Cache DOM elements for performance (re-query if null to handle dynamic DOM changes)
    let cachedHrValueEl = null;
    let cachedHrMinEl = null;
    let cachedHrMaxEl = null;

    /**
     * Gets cached HR value element, re-queries if needed
     */
    function getHrValueEl() {
      if (!cachedHrValueEl) cachedHrValueEl = document.getElementById("hr-value");
      return cachedHrValueEl;
    }

    /**
     * Gets cached HR min element, re-queries if needed
     */
    function getHrMinEl() {
      if (!cachedHrMinEl) cachedHrMinEl = document.getElementById("hr-min-value");
      return cachedHrMinEl;
    }

    /**
     * Gets cached HR max element, re-queries if needed
     */
    function getHrMaxEl() {
      if (!cachedHrMaxEl) cachedHrMaxEl = document.getElementById("hr-max-value");
      return cachedHrMaxEl;
    }

    /**
     * Updates animation state for all fans.
     * Called via requestAnimationFrame.
     */
    function animate(now) {
      const dt = Math.min(0.05, (now - lastTime) / 1000);
      lastTime = now;

      // Get current HR and min/max settings (using cached elements)
      const hrValueEl = getHrValueEl();
      const hrMinEl = getHrMinEl();
      const hrMaxEl = getHrMaxEl();
      
      const currentHR = hrValueEl ? parseInt(hrValueEl.textContent, 10) || 0 : 0;
      const minHR = hrMinEl ? parseInt(hrMinEl.textContent, 10) || 80 : 80;
      const maxHR = hrMaxEl ? parseInt(hrMaxEl.textContent, 10) || 160 : 160;

      const calculatedSpeed = calculateFanSpeed(currentHR, minHR, maxHR);

      // Update each fan animation
      let hasActiveFans = false;
      const fanCards = fanGrid.querySelectorAll(".fan-card");
      
      fanCards.forEach((card) => {
        const token = card.getAttribute("data-token");
        if (!token) return;

        const checkbox = card.querySelector('input[type="checkbox"]');
        const isActive = checkbox && checkbox.checked && !card.querySelector(".switch.error");
        const targetSpeed = (isActive && calculatedSpeed > 0) ? (maxSpeed * calculatedSpeed / 100) : 0;

        let anim = fanAnimations.get(token);
        if (!anim) {
          anim = { angle: 0, speed: 0, targetSpeed: 0 };
          fanAnimations.set(token, anim);
        }

        anim.targetSpeed = targetSpeed;

        // Interpolate speed
        if (anim.speed < anim.targetSpeed) {
          anim.speed = Math.min(anim.targetSpeed, anim.speed + accel * dt);
        } else if (anim.speed > anim.targetSpeed) {
          anim.speed = Math.max(anim.targetSpeed, anim.speed - decel * dt);
        }

        // Update rotation
        if (anim.speed > 0.1) {
          hasActiveFans = true;
          anim.angle = (anim.angle + anim.speed * dt) % 360;
          
          const fanId = token.substring(0, 8);
          const blades = document.getElementById(`FanBlades-${fanId}`);
          if (blades) {
            blades.setAttribute("transform", `rotate(${anim.angle.toFixed(2)} ${centerX} ${centerY})`);
          }
        } else {
          anim.speed = 0;
        }
      });

      // Continue animation loop if there are fans (so we can react to HR/toggle changes)
      // Only stop if there are no fans at all
      const hasFans = fanCards.length > 0;
      if (hasFans) {
        animationFrameId = requestAnimationFrame(animate);
      } else {
        animationFrameId = null;
        fanAnimations.clear();
      }
    }

    /**
     * Starts fan animation loop if not already running.
     */
    function startAnimation() {
      if (animationFrameId === null) {
        lastTime = performance.now();
        animationFrameId = requestAnimationFrame(animate);
      }
    }

    // Start animation on first fan card creation
    const originalCreateFanCard = createFanCard;
    createFanCard = function(token, opts) {
      const card = originalCreateFanCard(token, opts);
      startAnimation();
      return card;
    };

    // Start animation when HR changes
    const originalHRChanged = window.onHeartRateChanged;
    window.onHeartRateChanged = function(value) {
      if (originalHRChanged) originalHRChanged(value);
      startAnimation();
    };

    // Start animation on toggle change
    fanGrid.addEventListener("change", (evt) => {
      if (evt.target.type === "checkbox" && evt.target.closest(".fan-card")) {
        startAnimation();
      }
    });

    // Initialize animation if fans already exist
    if (getFanCount() > 0) {
      startAnimation();
    }
  })();

  // Public API
  window.fanManager = {
    addFanFromToken,
    removeFanByToken,
    removeFanByIndex,
    removeAllFans,
    restoreFans,
    refreshAddButtonState: updateAddFanButtonState,
    getFanCount,
    getTokens: () => [...window.fanTokens],
    updateFanToken: updateFanTokenInUI,
    setFanStatus,
    setFanRecoveryState,
    rebuildTokenListFromDOM,
    updateFromApi,
  };

  // Backfill fans from WS state if snapshot arrived before fanManager existed
  (function bootstrapFansFromState() {
    if (!window.ApiV1 || typeof window.ApiV1.getState !== "function") return;
    if (typeof getFanCount === "function" && getFanCount() > 0) return;
    const wsState = window.ApiV1.getState();
    const list = (wsState && Array.isArray(wsState.fans)) ? wsState.fans : [];
    if (!list.length || typeof restoreFans !== "function") return;
    const restore = list.map((f) => {
      const participationState = (!f.connected) ? "ERROR" : (f.controlActive ? "ACTIVE" : "INACTIVE");
      return { token: f.token, participationState };
    });
    restoreFans(restore, true);
  })();

  // Initialize when DOM is ready
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initializeFanManager);
  } else {
    initializeFanManager();
  }
  
  if (window.requestTelemetryRefresh) {
    window.requestTelemetryRefresh();
  }
})();

/**
   * FirmwareUpdate
   * --------------
   * Self-contained state machine for the firmware update flow with Arduino integration.
   *
   * State Flow:
   *   IDLE → SEARCHING → READY_TO_UPDATE → DOWNLOADING → INSTALLING → RESTARTING → IDLE
   *     ↓         ↓            ↓              ↓             ↓
   *   ERROR    ERROR        ABORTED        ERROR        ERROR
   *
   * Arduino Functions to Implement (called by JavaScript):
   *   - window.searchFirmware()              // Step 1: Start firmware search
   *   - window.startFirmwareDownload()       // Step 3: Start downloading firmware
   *   - window.startFirmwareInstallation()   // Step 4: Start installing firmware
   *   - window.abortUpdate()                 // Cancel current operation (optional)
   *
   * Arduino Callbacks (called by Arduino to notify JavaScript):
   *   - window.onFirmwareSearchResult(result)        // Step 1 → Step 2: Search complete
   *   - window.onFirmwareDownloadProgress(percent)    // Step 3: Download progress (0-100)
   *   - window.onFirmwareInstallError(error)          // Any phase → Step 5: Update error (can be called during any phase)
   *   - window.onFirmwareDownloadComplete()           // Step 3 → Step 4: Download complete (optional, 100% also triggers)
   *   - window.onFirmwareInstallationResult(result)   // Step 4 → Step 5 or RESTARTING: Install result
   *   - window.onFirmwareRestartComplete()            // RESTARTING → IDLE: Device restarted successfully
   *
   * Usage:
   *   FirmwareUpdate.start();  // Called when user clicks "Update Firmware" button
   */

  const FirmwareUpdate = (function () {
    // --- State Constants ----------------------------------------------------
    const STATE = {
      IDLE: "idle",
      SEARCHING: "searching",
      READY_TO_UPDATE: "ready_to_update",
      DOWNLOADING: "downloading",
      INSTALLING: "installing",
      RESTARTING: "restarting",
      ERROR: "error",
      ABORTED: "aborted"
    };

    // --- SVG assets ---------------------------------------------------------
    // --- Internal state -----------------------------------------------------
    let currentState = STATE.IDLE;
    let lastSearchResult = null;
    let pollTimerId = null; // FIX: Track polling timer for cancellation
    let statusPollTimerId = null; // HTTP status polling during DOWNLOADING (P1)

    /**
     * Validates state transition and logs for debugging.
     * Checks if transition from current state to target state is allowed by state machine rules.
     * 
     * @param {string} fromState - Current state
     * @param {string} toState - Target state
     * @returns {boolean} True if transition is valid, false otherwise
     * @private
     */
    function validateStateTransition(fromState, toState) {
      // Allow same-state transitions (no-op, but valid)
      if (fromState === toState) {
        return true;
      }
      
      // ESP32-OTA: Installation happens automatically during download,
      // so we skip INSTALLING and go directly to RESTARTING after download.
      const validTransitions = {
        [STATE.IDLE]: [STATE.SEARCHING, STATE.ERROR],
        [STATE.SEARCHING]: [STATE.READY_TO_UPDATE, STATE.IDLE, STATE.ERROR, STATE.ABORTED],
        [STATE.READY_TO_UPDATE]: [STATE.DOWNLOADING, STATE.ABORTED, STATE.ERROR],
        [STATE.DOWNLOADING]: [STATE.RESTARTING, STATE.ERROR], // Skip INSTALLING for ESP32-OTA
        [STATE.INSTALLING]: [STATE.RESTARTING, STATE.ERROR],  // Keep for backward compatibility
        [STATE.RESTARTING]: [STATE.IDLE, STATE.ERROR],
        [STATE.ERROR]: [STATE.IDLE],
        [STATE.ABORTED]: [STATE.IDLE]
      };

      const allowed = validTransitions[fromState] || [];
      if (!allowed.includes(toState)) {
        return false;
      }
      return true;
    }

    /**
     * Sets the current state with validation.
     * Validates transition before updating state and logs the change.
     * 
     * @param {string} newState - New state to set
     * @returns {void}
     * @private
     */
    function setState(newState) {
      // Only log and update if state actually changes
      if (currentState !== newState) {
        if (validateStateTransition(currentState, newState)) {
          currentState = newState;
        }
      }
    }

    // Use centralized escapeHtml utility (window.escapeHtml)

    /**
     * Renders the search content HTML.
     * Creates HTML for the searching state modal with spinner and message.
     * 
     * @returns {string} HTML content string
     * @private
     */

     

    /**
     * Starts the firmware update process (Step 1).
     * Transitions to SEARCHING state, opens modal, and calls Arduino searchFirmware() function.
     * User can cancel during search, which transitions to ABORTED state.
     * 
     * If the state machine is in ERROR or ABORTED state, it first resets to IDLE before starting.
     * If already in an active state (SEARCHING, DOWNLOADING, etc.), the function returns early.
     * 
     * @returns {void}
     */
    function start() {
      // Check if we can start from current state
      if (currentState === STATE.SEARCHING || 
          currentState === STATE.READY_TO_UPDATE || 
          currentState === STATE.DOWNLOADING || 
          currentState === STATE.INSTALLING || 
          currentState === STATE.RESTARTING) {
        return;
      }

      // If in ERROR or ABORTED, reset to IDLE first
      if (currentState === STATE.ERROR || currentState === STATE.ABORTED) {
        setState(STATE.IDLE);
      }

      // Now transition to SEARCHING (only valid from IDLE)
      if (currentState !== STATE.IDLE) {
        return;
      }

      // Open modal first (UI feedback)
      AppModal.open({
        title: "Searching Update",
        icon: "--icon-spinner-svg-data",
        subline: "Please wait while the device checks for updates…",
        buttons: [
          {
            type: "cancel",
            label: "Cancel",
            className: "btn btn-regular btn-tertiary",
            closeOnClick: true,
            onClick: () => {
              // FIX: Cancel polling timer if active
              if (pollTimerId) {
                clearTimeout(pollTimerId);
                pollTimerId = null;
              }
              setState(STATE.ABORTED);
              if (window.ApiV1 && typeof window.ApiV1.firmwareAbort === "function") {
                window.ApiV1.firmwareAbort().catch(() => {});
              }
            },
          },
        ]
      });

      // Only transition state after HTTP call succeeds
      if (window.ApiV1 && typeof window.ApiV1.firmwareSearch === "function") {
        setState(STATE.SEARCHING);
        window.ApiV1.firmwareSearch().then((result) => {
          if (!result || !result.ok) {
            // HTTP call failed - show error and reset
            setState(STATE.ERROR);
            openError(result?.error || "Failed to start firmware search");
            return;
          }
          
          // Search is async on backend - WebSocket will deliver FIRMWARE_SEARCH_RESULT frame
          // No polling needed - handleSearchResult() will be called via WS
        }).catch((err) => {
          setState(STATE.ERROR);
          openError("Network error during search");
        });
      } else {
        setState(STATE.ERROR);
        openError("Firmware search function not available");
      }
    }

    /**
     * Opens modal showing new firmware is available (Step 2).
     * Displays firmware version and release notes. User can cancel or start the update.
     * 
     * @returns {void}
     * @private
     */
    function openNewFirmwareAvailable() {
      setState(STATE.READY_TO_UPDATE);

      const versionText =
        lastSearchResult && lastSearchResult.latestVersion
          ? `Version ${window.escapeHtml(String(lastSearchResult.latestVersion))}`
          : "New firmware version is available.";

      const notesHtml =
        lastSearchResult && lastSearchResult.releaseNotes
          ? `<div class="fw-modal"><p class="fw-modal-notes">${window.escapeHtml(
              String(lastSearchResult.releaseNotes)
            )}</p></div>`
          : "";

      AppModal.open({
        title: "Firmware Available",
        subline: versionText,
        icon: "--icon-firmware-available-svg-data",
        content: notesHtml,
        buttons: [
          {
            type: "cancel",
            label: "Cancel",
            className: "btn btn-regular btn-tertiary",
            closeOnClick: true,
            onClick: () => {
              setState(STATE.ABORTED);
              if (window.ApiV1 && typeof window.ApiV1.firmwareAbort === "function") {
                window.ApiV1.firmwareAbort().catch(() => {});
              }
            },
          },
          {
            type: "save",
            label: "Start Update",
            className: "btn btn-regular btn-primary",
            closeOnClick: false,
            onClick: () => {
              startDownload();
            },
          },
        ],
      });
    }

    /**
     * Opens modal showing no update is available (Step 2 - no update).
     * Transitions to IDLE state and displays message that device is up to date.
     * 
     * @returns {void}
     * @private
     */
    function openNoUpdate() {
      setState(STATE.IDLE);

      AppModal.open({
        title: "No Update Available",
        icon: "--icon-firmware-unavailable-svg-data",
        content: `
          <div class="fw-modal">
            <p class="modal-text">
              Your device is already running the latest firmware.
            </p>
          </div>
        `,
        buttons: [
          {
            type: "cancel",
            label: "Close",
            className: "btn btn-regular btn-tertiary",
            closeOnClick: true,
          },
        ],
      });
    }

    /**
     * Opens error modal (Step 5).
     * Centralized error handling for all error scenarios. Extracts error message from various formats.
     * 
     * @param {string|Object} message - Error message or error object
     * @param {string} [message.message] - Error message if object provided
     * @param {string} [message.error] - Alternative error message property
     * @returns {void}
     * @private
     */
    function openError(message) {
      stopFirmwareStatusPolling();
      setState(STATE.ERROR);

      // Extract error message from various formats
      let errorText = "An error occurred during the software update.";
      let errorCode = null;
      let errorReason = null;

      if (typeof message === "string" && message.trim()) {
        errorText = message;
      } else if (message && typeof message === "object") {
        // Support structured error objects with code, reason, message, error
        if (typeof message.message === "string") {
          errorText = message.message;
        } else if (typeof message.error === "string") {
          errorText = message.error;
        }
        
        // Extract error code if provided
        if (typeof message.code === "string" || typeof message.code === "number") {
          errorCode = String(message.code);
        }
        
        // Extract error reason if provided
        if (typeof message.reason === "string") {
          errorReason = message.reason;
        }
      }

      // Use centralized escapeHtml utility (defined globally)
      const text = window.escapeHtml(errorText);
      const code = errorCode ? window.escapeHtml(errorCode) : null;
      const reason = errorReason ? window.escapeHtml(errorReason) : null;

      // Build error content with optional code and reason
      let errorContent = `<p class="modal-text">${text}</p>`;
      
      if (code || reason) {
        errorContent += `<div class="fw-modal-notes" style="margin-top: 16px; padding-top: 16px; border-top: 1px solid rgba(255,255,255,0.1);">`;
        if (code) {
          errorContent += `<div style="font-size: 12px; opacity: 0.8; margin-bottom: 8px;">Error Code: <strong>${code}</strong></div>`;
        }
        if (reason) {
          errorContent += `<div style="font-size: 12px; opacity: 0.8;">Reason: ${reason}</div>`;
        }
        errorContent += `</div>`;
      }

      AppModal.open({
        title: "Error During Software Update",
        icon: "--icon-error-svg-data",
        content: `
          <div class="fw-modal">
            ${errorContent}
          </div>
        `,
        buttons: [
          {
            type: "cancel",
            label: "Close",
            className: "btn btn-regular btn-tertiary",
            closeOnClick: true,
          },
        ],
      });
      // Reconnect WebSocket after error (e.g. after download failed) so UI works without page reload
      if (window.ApiV1 && typeof window.ApiV1.wsConnect === "function") {
        window.ApiV1.wsConnect();
      }
    }

    /**
     * Handles firmware search result (Step 1 → Step 2).
     * Called by Arduino via window.onFirmwareSearchResult(). Validates state and routes to appropriate modal.
     * 
     * @param {Object} result - Search result from Arduino
     * @param {boolean} [result.hasUpdate] - Whether update is available
     * @param {string} [result.status] - Status string ("update_available" indicates update)
     * @param {string} [result.error] - Error message if search failed
     * @param {string} [result.latestVersion] - Latest version string
     * @param {string} [result.releaseNotes] - Release notes
     * @returns {void}
     */
    function handleSearchResult(result) {
      // Ignore callbacks if aborted
      if (currentState === STATE.ABORTED) {
        return;
      }

      // Accept results when in SEARCHING or READY_TO_UPDATE state
      // (READY_TO_UPDATE means search completed and found an update)
      if (currentState !== STATE.SEARCHING && currentState !== STATE.READY_TO_UPDATE) {
        return;
      }

      lastSearchResult = result || {};

      if (result && result.error) {
        openError(result.error);
        return;
      }

      // Interpret result:
      //  - result.hasUpdate === true
      //  - or result.status === "update_available"
      const hasUpdate =
        (result && result.hasUpdate === true) ||
        (result && result.status === "update_available");

      if (hasUpdate) {
        openNewFirmwareAvailable();
      } else {
        openNoUpdate();
      }
    }

    /**
     * Renders download progress content.
     * Creates HTML for the downloading state modal with progress bar and percentage display.
     * 
     * @param {number} initialPercent - Initial progress percentage (0-100)
     * @returns {string} HTML content string
     * @private
     */
    function renderDownloadContent(initialPercent) {
      const pct = Math.max(0, Math.min(100, Math.round(initialPercent || 0)));
      return `
        <div class="fw-modal">
          <div class="fw-progress">
            <div class="fw-progress-bar" id="fw-progress-bar" style="width:${pct}%;"></div>
          </div>
          <div class="fw-progress-label" id="fw-progress-label">${pct}%</div>
        </div>
      `;
    }

    /**
     * Starts firmware download (Step 2 → Step 3).
     * Transitions to DOWNLOADING state, opens modal with progress bar, and calls Arduino startFirmwareDownload().
     * User cannot interact during download.
     * 
     * @returns {void}
     * @private
     */
    function startDownload() {
      setState(STATE.DOWNLOADING);
      AppModal.open({
        title: "Downloading Firmware",
        subline: "The update is being downloaded. Do not power off the device.",
        content: renderDownloadContent(0),
        buttons: [],
        actions: false,
      });

      if (!window.ApiV1 || typeof window.ApiV1.firmwareDownload !== "function") {
        setState(STATE.ERROR);
        openError("Firmware download function not available");
        return;
      }
      startFirmwareStatusPolling();
      window.ApiV1.firmwareDownload().then((result) => {
        if (result && result.ok) {
          // Progress: WebSocket FIRMWARE_PROGRESS + HTTP status polling (P1)
        } else {
          setState(STATE.ERROR);
          openError(result?.error || "Failed to start firmware download");
        }
      }).catch((err) => {
        setState(STATE.ERROR);
        openError("Network error during download");
      });
    }

    /**
     * HTTP status polling during DOWNLOADING (P1).
     * Updates progress bar and drives phase transitions from state/progress.
     * Robust when WebSocket disconnects during OTA.
     * @private
     */
    function startFirmwareStatusPolling() {
      stopFirmwareStatusPolling();
      const pollInterval = (window.TIMING && window.TIMING.FIRMWARE_STATUS_POLL_MS) || 2500;
      const endpoint = (typeof FW_API !== "undefined" && FW_API.status) ? FW_API.status : "/api/v1/action/firmware/status";

      const check = () => {
        if (currentState !== STATE.DOWNLOADING) return;
        fetch(endpoint, { method: "GET", cache: "no-cache" })
          .then((r) => (r.ok ? r.text() : Promise.reject(new Error("Status " + r.status))))
          .then((text) => {
            if (currentState !== STATE.DOWNLOADING) return;
            const params = {};
            text.split("\n").forEach((line) => {
              const idx = line.indexOf("=");
              if (idx > 0) params[line.substring(0, idx).trim()] = line.substring(idx + 1).trim();
            });
            const state = params.state || "";
            const progress = Math.max(0, Math.min(100, parseInt(params.progress, 10) || 0));

            if (state === "error") {
              stopFirmwareStatusPolling();
              openError(params.error || "Update failed");
              return;
            }
            if (state === "idle" || state === "unavailable") {
              stopFirmwareStatusPolling();
              if (window.AppModal && typeof window.AppModal.close === "function") window.AppModal.close();
              window.location.reload();
              return;
            }
            const bar = document.getElementById("fw-progress-bar");
            const label = document.getElementById("fw-progress-label");
            if (bar) bar.style.width = progress + "%";
            if (label) label.textContent = Math.round(progress) + "%";
            if (progress >= 100 || state === "installing") {
              stopFirmwareStatusPolling();
              showInstallingThenRestarting();
              return;
            }
            statusPollTimerId = setTimeout(check, pollInterval);
          })
          .catch(() => {
            if (currentState !== STATE.DOWNLOADING) return;
            statusPollTimerId = setTimeout(check, pollInterval);
          });
      };
      statusPollTimerId = setTimeout(check, pollInterval);
    }

    function stopFirmwareStatusPolling() {
      if (statusPollTimerId) {
        clearTimeout(statusPollTimerId);
        statusPollTimerId = null;
      }
    }

    /**
     * P2: When WS disconnects during DOWNLOADING, ensure status polling runs.
     */
    function ensureStatusPollingWhenDownloading() {
      if (currentState === STATE.DOWNLOADING && !statusPollTimerId) {
        startFirmwareStatusPolling();
      }
    }

    /**
     * Handles download progress updates (Step 3).
     * Called by Arduino via window.onFirmwareDownloadProgress(). Updates progress bar and label.
     * Automatically transitions to installation when progress reaches 100%.
     * 
     * @param {number} percent - Download progress (0-100)
     * @returns {void}
     */
    function handleDownloadProgress(percent) {
      if (currentState !== STATE.DOWNLOADING) {
        return;
      }

      let p = Number(percent);
      if (!Number.isFinite(p)) p = 0;
      p = Math.max(0, Math.min(100, p));

      const bar = document.getElementById("fw-progress-bar");
      const label = document.getElementById("fw-progress-label");
      if (bar) bar.style.width = p + "%";
      if (label) label.textContent = Math.round(p) + "%";

      if (p >= 100) {
        showInstallingThenRestarting();
      }
    }

    /**
     * Handles download completion (e.g. Arduino callback). Alternative to 100% progress.
     * ESP32-OTA: Show Installing then Restarting; install happens on device after download.
     * @returns {void}
     */
    function handleDownloadComplete() {
      if (currentState === STATE.RESTARTING) return;
      if (currentState === STATE.INSTALLING) {
        openRestarting();
        return;
      }
      if (currentState !== STATE.DOWNLOADING) return;
      showInstallingThenRestarting();
    }

    /**
     * Handles download error (Step 3 → Step 5).
     * Called by Arduino via window.onFirmwareInstallError(). Validates state and displays error modal.
     * 
     * @param {string|Object} error - Error message or error object
     * @param {string} [error.message] - Error message if object provided
     * @returns {void}
     */
    function handleDownloadError(error) {
      // Allow errors during any active update state (not IDLE, ERROR, or ABORTED)
      const activeStates = [
        STATE.SEARCHING,
        STATE.READY_TO_UPDATE,
        STATE.DOWNLOADING,
        STATE.INSTALLING,
        STATE.RESTARTING
      ];
      
      if (!activeStates.includes(currentState)) {
        return;
      }
      
      // Pass error directly to openError - it handles string, object with message/error, or structured object with code/reason
      openError(error);
    }

    /**
     * Opens installation modal (Step 3 → Step 4).
     * Transitions to INSTALLING state, opens modal, and calls Arduino startFirmwareInstallation().
     * User cannot interact during installation.
     * 
     * @returns {void}
     * @private
     */
    function openInstallation() {
      // Open modal first (UI feedback)
      AppModal.open({
        title: "Installing Firmware",
        icon: "--icon-spinner-svg-data",
        content: `
          <div class="fw-modal">
            <p class="modal-text">
              The device is installing the update and will reboot automatically.
              Do not disconnect power during this process.
            </p>
          </div>
        `,
        buttons: [],
        actions: false,
      });

      // If we're already in INSTALLING state (e.g., detected from status polling),
      // just start monitoring - don't try to start installation again
      if (currentState === STATE.INSTALLING) {
        startRebootMonitoring();
        return;
      }

      // Only transition state after HTTP call succeeds
      if (window.ApiV1 && typeof window.ApiV1.firmwareInstall === "function") {
        window.ApiV1.firmwareInstall().then((result) => {
          if (result && result.ok) {
            setState(STATE.INSTALLING);
            // Start monitoring for device reboot
            startRebootMonitoring();
          } else {
            // HTTP call failed - show error and reset
            setState(STATE.ERROR);
            openError(result?.error || "Failed to start firmware installation");
          }
        }).catch((err) => {
          setState(STATE.ERROR);
          openError("Network error during installation");
        });
      } else {
        setState(STATE.ERROR);
        openError("Firmware installation function not available");
      }
    }
    
    /**
     * Monitors for device reboot after firmware installation.
     * Polls status endpoint every 3 seconds until device is back online, then refreshes page.
     * @private
     */
    function startRebootMonitoring() {
      let attempts = 0;
      const maxAttempts = 20; // 20 attempts × 3 seconds = 60 seconds max
      const pollInterval = (window.TIMING && window.TIMING.REBOOT_POLL_INTERVAL_MS) || 3000;
      let monitoringActive = true;
      
      const checkReboot = () => {
        // Stop monitoring if we're no longer in INSTALLING state
        if (currentState !== STATE.INSTALLING || !monitoringActive) {
          return;
        }
        
        attempts++;
        
        fetch(FW_API.status, { 
          method: "GET",
          cache: "no-cache"
        })
          .then((r) => {
            if (r.ok) {
              // Device is back online - check if we can get status
              return r.text();
            } else {
              throw new Error(`Status check failed: ${r.status}`);
            }
          })
          .then((text) => {
            // Parse status response
            const lines = text.split('\n');
            const params = {};
            lines.forEach((line) => {
              const idx = line.indexOf('=');
              if (idx > 0) {
                const key = line.substring(0, idx).trim();
                const value = line.substring(idx + 1).trim();
                params[key] = value;
              }
            });
            
            // Device is back online and responding
            // If state is "idle" or we get a valid response, device rebooted successfully
            if (params.state === "idle" || params.state === "unavailable" || params.state) {
              // Device rebooted - refresh page to show new firmware
              monitoringActive = false;
              window.location.reload();
              return; // Stop polling
            }
            
            // Still installing or unknown state - continue polling
            if (attempts < maxAttempts && monitoringActive) {
              setTimeout(checkReboot, pollInterval);
            } else if (monitoringActive) {
              monitoringActive = false;
              window.location.reload();
            }
          })
          .catch((err) => {
            // Connection error - device is likely rebooting
            // Continue polling until device is back
            if (attempts < maxAttempts && monitoringActive) {
              setTimeout(checkReboot, pollInterval);
            } else if (monitoringActive) {
              // Refresh anyway - user can manually refresh if needed
              monitoringActive = false;
              window.location.reload();
            }
          });
      };
      
      // Start polling after a short delay (device needs time to start rebooting)
      const initialDelay = (window.TIMING && window.TIMING.REBOOT_INITIAL_DELAY_MS) || 3000;
      setTimeout(checkReboot, initialDelay);
    }

    
    /**
     * Handles installation result (Step 4 → RESTARTING or Step 5).
     * Called by Arduino via window.onFirmwareInstallationResult(). Validates state and routes to restart or error.
     * 
     * @param {boolean|Object} result - Installation result
     *   - true: Installation successful, proceed to restart
     *   - false or object with error: Installation failed
     * @param {string} [result.message] - Error message if failed
     * @param {string} [result.error] - Alternative error message property
     * @returns {void}
     */
    function handleInstallationResult(result) {
      if (currentState !== STATE.INSTALLING) {
        return;
      }

      // Check if installation was successful
      const ok = result === true || (result && result !== false && !result.error && !result.message);

      if (ok) {
        // Installation successful → proceed to restart
        openRestarting();
      } else {
        // Installation failed → show error
        const msg =
          result && typeof result.message === "string"
            ? result.message
            : result && typeof result.error === "string"
            ? result.error
            : "Installation failed or timed out.";
        openError(msg);
      }
    }

    /**
     * Shows "Installing" modal briefly, then opens Restarting modal and starts reboot monitoring.
     * Used when download reaches 100% or handleDownloadComplete fires (ESP32-OTA).
     * @private
     */
    function showInstallingThenRestarting() {
      if (currentState === STATE.RESTARTING) return;
      stopFirmwareStatusPolling();
      AppModal.open({
        title: "Installing Firmware",
        icon: "--icon-spinner-svg-data",
        content: `
          <div class="fw-modal">
            <p class="modal-text">
              The device is installing the update and will reboot automatically.
              Do not disconnect power during this process.
            </p>
          </div>
        `,
        buttons: [],
        actions: false,
      });
      setTimeout(() => openRestarting(), 1500);
    }

    /**
     * Opens restarting modal (RESTARTING state).
     * Idempotent: no-op if already RESTARTING. Starts shared reboot monitoring; modal closes on reload.
     * @private
     */
    function openRestarting() {
      if (currentState === STATE.RESTARTING) return;
      setState(STATE.RESTARTING);
      AppModal.open({
        title: "Restarting Device",
        icon: "--icon-spinner-svg-data",
        content: `
          <div class="fw-modal">
            <p class="modal-text">
              The firmware has been installed successfully. The device is restarting...
            </p>
            <p class="fw-modal-notes">
              Please wait for the device to restart. The page will reload automatically.
            </p>
          </div>
        `,
        buttons: [],
        actions: false,
      });
      startRebootMonitoringForRestart();
    }
    
    /**
     * Monitors for device reboot during RESTARTING state.
     * Uses shared window.startRebootMonitoring (same as Device Restart).
     * @private
     */
    function startRebootMonitoringForRestart() {
      if (typeof window.startRebootMonitoring !== "function") return;
      window.startRebootMonitoring({
        endpoint: FW_API.status,
        isActive: () => currentState === STATE.RESTARTING,
        onSuccess: () => {
          setState(STATE.IDLE);
          if (window.AppModal && typeof window.AppModal.close === "function") window.AppModal.close();
          window.location.reload();
        },
      });
    }

    /**
     * Handles restart completion (RESTARTING → IDLE).
     * Called by Arduino via window.onFirmwareRestartComplete(). Validates state, transitions to IDLE, and closes modal.
     * 
     * @returns {void}
     */
    function handleRestartComplete() {
      if (currentState !== STATE.RESTARTING) {
        return;
      }

      setState(STATE.IDLE);
      AppModal.close();
    }

    // Public API
    return {
      start,
      handleSearchResult,
      handleDownloadProgress,
      handleDownloadComplete,
      handleDownloadError,
      handleInstallationResult,
      handleRestartComplete,
      ensureStatusPollingWhenDownloading,
      getState: () => currentState,
    };
  })();

  // Expose to global scope
  window.FirmwareUpdate = FirmwareUpdate;


  /**
   * Called by Arduino when download completes successfully (Step 3 → Step 4).
   * Global callback function that routes download completion to FirmwareUpdate module.
   * Alternative to waiting for 100% progress via onFirmwareDownloadProgress.
   * 
   * @returns {void}
   */
  window.onFirmwareDownloadComplete = function () {
    FirmwareUpdate.handleDownloadComplete();
  };

  /**
   * Called by Arduino when an error occurs during the firmware update process.
   * Can be triggered during any phase of the update (searching, downloading, installing, etc.).
   * Supports simple strings, error objects, or structured error objects with codes and reasons.
   * 
   * Note: This function can be called during any active update state:
   * - SEARCHING: Error checking for updates
   * - READY_TO_UPDATE: Error before starting download
   * - DOWNLOADING: Error during download (original use case)
   * - INSTALLING: Error during installation
   * - RESTARTING: Error during restart process
   * 
   * @param {string|Object} error - Error message or error object
   * @param {string} [error.message] - Error message if object provided
   * @param {string} [error.error] - Alternative error message property
   * @param {string|number} [error.code] - Error code (e.g., "NETWORK_TIMEOUT", "HTTP_404", 1001)
   * @param {string} [error.reason] - Detailed error reason or description
   * @returns {void}
   * 
   * @example
   * // Simple string
   * onFirmwareInstallError("Update failed: Network timeout");
   * 
   * // Object with message
   * onFirmwareInstallError({message: "Update failed: Network timeout"});
   * 
   * // Structured error with code and reason
   * onFirmwareInstallError({
   *   message: "Update failed",
   *   code: "NETWORK_TIMEOUT",
   *   reason: "Connection to server timed out after 30 seconds"
   * });
   * 
   * // Error during search phase
   * onFirmwareInstallError({
   *   message: "Failed to check for updates",
   *   code: "SEARCH_ERROR",
   *   reason: "Could not connect to GitHub API"
   * });
   * 
   * // Error during installation phase
   * onFirmwareInstallError({
   *   message: "Installation failed",
   *   code: "INSTALL_ERROR",
   *   reason: "Insufficient flash memory"
   * });
   */
  window.onFirmwareInstallError = function (error) {
    FirmwareUpdate.handleDownloadError(error);
  };

  /**
   * Called by Arduino when installation completes (Step 4 → RESTARTING or Step 5).
   * Global callback function that routes installation results to FirmwareUpdate module.
   * 
   * @param {boolean|Object} result - Installation result
   *   - true → Success, proceed to restart
   *   - false or {error: "message"} → Failure, show error
   * @param {string} [result.message] - Error message if installation failed
   * @param {string} [result.error] - Alternative error message property
   * @returns {void}
   */
  window.onFirmwareInstallationResult = function (result) {
    FirmwareUpdate.handleInstallationResult(result);
  };

  /**
   * Called by Arduino after successful restart (RESTARTING → IDLE).
   * Global callback function that routes restart completion to FirmwareUpdate module.
   * Should be called when the device has restarted and is ready.
   * 
   * @returns {void}
   */
  window.onFirmwareRestartComplete = function () {
    FirmwareUpdate.handleRestartComplete();
  };


  // Initialize firmware update button (with fallback for dynamic script loading)
  function initializeFirmwareUpdateButton() {
    const fwUpdateButton = document.getElementById("btn-firmware-update");

    if (!fwUpdateButton) {
      return;
    }

    fwUpdateButton.addEventListener("click", function () {
      if (window.FirmwareUpdate && typeof FirmwareUpdate.start === "function") {
        FirmwareUpdate.start();
      }
    });
  }

  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initializeFirmwareUpdateButton);
  } else {
    initializeFirmwareUpdateButton();
  }

/**
 * BLESensorManager - BLE bike sensor setup state machine (HR, CSC)
 * Arduino: loadConnectedSensors(), getSensorConnectionState(), deleteConfiguredSensor(), startDiscovery(), setConfiguredSensor()
 * Callback: window.onSensorDiscoveryResult(sensors[])
 */

const BLESensorManager = (function () {
  const STATE = { SENSOR_OVERVIEW: "sensor_overview", DISCOVER_SENSORS: "discover_sensors", SELECT_SENSOR: "select_sensor", ERROR: "error" };
  
  const DEFAULT_SENSOR_TYPES = ["HR", "CSC"];

  if (typeof window.DEFAULT_SENSOR_TYPES === "undefined") window.DEFAULT_SENSOR_TYPES = DEFAULT_SENSOR_TYPES;

  let currentState = STATE.SENSOR_OVERVIEW, currentDiscoveryType = null, discoveredSensors = [], selectedSensor = null, configuredSensors = [];
  let isSensorOverviewModalOpen = false; // Track if sensor overview modal is open
  
  // Batch update support to prevent UI flicker from rapid WS updates
  let _batchUpdateActive = false;
  let _pendingRender = false;
  let _updateDebounceTimer = null;

  function validateStateTransition(fromState, toState) {
    if (fromState === toState) return true;
    const validTransitions = {
      [STATE.SENSOR_OVERVIEW]: [STATE.DISCOVER_SENSORS, STATE.ERROR],
      [STATE.DISCOVER_SENSORS]: [STATE.SELECT_SENSOR, STATE.ERROR, STATE.SENSOR_OVERVIEW],
      [STATE.SELECT_SENSOR]: [STATE.SENSOR_OVERVIEW, STATE.DISCOVER_SENSORS, STATE.ERROR],
      [STATE.ERROR]: [STATE.DISCOVER_SENSORS, STATE.SENSOR_OVERVIEW]
    };
    return (validTransitions[fromState] || []).includes(toState);
  }

  function setState(newState) {
    if (validateStateTransition(currentState, newState)) {
      currentState = newState;
    }
  }

  // Use centralized escapeHtml utility (window.escapeHtml)
  const getSensorTypeLabel = (type) => ({ HR: "Heart Rate", CSC: "Cadence" }[type] || type);
  const getSensorIconClass = (type, connected) => `sensor-icon-${type.toLowerCase()}${connected ? "" : "-grayscale"}`;

  function loadSensorsWithStates() {
    // Use WebSocket state directly - sensors are loaded via SENSORS_SNAPSHOT frame
    // Same principle as Firmware Update: POST for actions, WebSocket for status
    // Access state via window.ApiV1.getState() since we're outside the IIFE scope
    if (window.ApiV1 && typeof window.ApiV1.getState === "function") {
      const wsState = window.ApiV1.getState();
      const sensors = wsState.sensors || [];
      return Promise.resolve(sensors.map(s => ({
        Name: s.name || '',
        Type: s.type || 'HR',
        MAC: s.mac || '',
        Connected: s.connected || false,
        Battery: s.battery !== undefined ? s.battery : null,
        id: s.id || s.name
      })));
    }
    // Fallback: return empty array if state not available
    return Promise.resolve([]);
  }
  

  function renderSensorOverview() {
    // Get allowed types - handle both Promise and synchronous return
    const getAllowedTypes = () => {
      if (typeof window.getAllowedSensorTypes === "function") {
        const result = window.getAllowedSensorTypes();
        // If it's a Promise, return it; otherwise wrap in Promise.resolve()
        return Promise.resolve(result);
      }
      // Fallback to default types
      return Promise.resolve(window.DEFAULT_SENSOR_TYPES || DEFAULT_SENSOR_TYPES);
    };
    
    // Chain both Promises: first get allowed types, then load sensors
    return getAllowedTypes().then(function(allowedTypes) {
      // Ensure allowedTypes is an array
      const typesArray = Array.isArray(allowedTypes) ? allowedTypes : (window.DEFAULT_SENSOR_TYPES || DEFAULT_SENSOR_TYPES);
      
      return loadSensorsWithStates().then(function(sensors) {
        configuredSensors = sensors;
        const sensorListHtml = typesArray.map(type => {
          const sensor = configuredSensors.find(s => s.Type === type);
          const iconClass = getSensorIconClass(type, sensor?.Connected || false);
          if (sensor) {
            const name = window.escapeHtml(sensor.Name || "");
            const isConnected = sensor.Connected || false;
            const battery = sensor.Battery !== undefined && sensor.Battery !== null ? `${sensor.Battery}%` : "N/A";
            const statusClass = isConnected ? "sensor-status-connected" : "sensor-status-disconnected";
            const statusText = isConnected ? "Connected" : "Disconnected";
            return `<div class="sensor-tile ${statusClass}" data-type="${window.escapeHtml(type)}" data-name="${window.escapeHtml(sensor.Name)}"><div class="sensor-info"><div class="sensor-icon ${iconClass}"></div><div class="sensor-text"><div class="sensor-name">${name}</div><div class="sensor-status"><span class="sensor-battery">Battery: ${battery}</span><span class="sensor-conn-indicator">${statusText}</span></div></div></div><button class="btn btn-small btn-tertiary sensor-remove-btn" type="button">Remove</button></div>`;
          }
          return `<div class="sensor-tile" data-type="${window.escapeHtml(type)}"><div class="sensor-info"><div class="sensor-icon ${iconClass}"></div><div class="sensor-text"><div class="sensor-name">No ${getSensorTypeLabel(type)} Sensor Connected</div></div></div><button class="btn btn-small btn-cta sensor-add-btn" type="button">Add</button></div>`;
        }).join("");
        return `<div class="sensor-overview"><div class="sensor-list">${sensorListHtml}</div></div>`;
      });
    });
  }

  function openSensorOverview() {
    setState(STATE.SENSOR_OVERVIEW);
    selectedSensor = null;
    discoveredSensors = [];
    currentDiscoveryType = null;
    isSensorOverviewModalOpen = true; // Mark modal as open
    
    // Pause BLE Relay Server when opening sensor overview
    if (window.ApiV1 && typeof window.ApiV1.pauseBLERelayServer === "function") {
      window.ApiV1.pauseBLERelayServer().catch(function(err) {
      });
    }
    
    renderSensorOverview().then(function(content) {
      AppModal.open({
        title: "Setup Sensors",
        icon: "--icon-sensor-hr-svg-data",
        content: content,
        buttons: [{ type: "cancel", label: "Close", className: "btn btn-regular btn-secondary", closeOnClick: true }],
        onCancel: function() {
          isSensorOverviewModalOpen = false; // Mark modal as closed
          // Resume BLE Relay Server when modal is closed (via close button, backdrop, or ESC)
          if (window.ApiV1 && typeof window.ApiV1.resumeBLERelayServer === "function") {
            window.ApiV1.resumeBLERelayServer().catch(function(err) {
            });
          }
        },
        onOpen: function() {
          document.querySelectorAll(".sensor-add-btn").forEach(function(btn) {
            btn.addEventListener("click", function() {
              const type = btn.closest(".sensor-tile")?.getAttribute("data-type");
              if (type) startDiscovery(type);
            });
          });
          document.querySelectorAll(".sensor-remove-btn").forEach(function(btn) {
            btn.addEventListener("click", async function() {
              const name = btn.closest(".sensor-tile")?.getAttribute("data-name");
              if (name) {
                if (window.ApiV1 && typeof window.ApiV1.deleteSensor === "function") {
                  try {
                    const result = await window.ApiV1.deleteSensor(name);
                    if (result.ok) {
                      // Refresh the sensor overview after successful deletion
                      openSensorOverview();
                    } else {
                    }
                  } catch (err) {
                  }
                } else {
                  // Fallback if function doesn't exist
                  openSensorOverview();
                }
              }
            });
          });
        },
      });
    });
  }

  const renderDiscoverContent = (type) => `<div class="discover-modal"><p class="modal-text-muted">Searching for ${window.escapeHtml(getSensorTypeLabel(type))} Sensors…</p></div>`;

  function startDiscovery(type) {
    setState(STATE.DISCOVER_SENSORS);
    isSensorOverviewModalOpen = false; // FIX: Mark overview as closed when entering discovery
    currentDiscoveryType = type;
    discoveredSensors = [];
    selectedSensor = null;
    const iconMap = { HR: "--icon-sensor-hr-svg-data", CSC: "--icon-sensor-csc-svg-data" };
    AppModal.open({ title: "Searching", icon: "--icon-spinner-svg-data", subline: `Searching for ${getSensorTypeLabel(type)} Sensors…`, buttons: [], actions: false });
    const startArduinoDiscovery = async () => {
      if (window.ApiV1 && typeof window.ApiV1.discoveryStart === "function") {
        try {
          const result = await window.ApiV1.discoveryStart(type);
          if (!result.ok) {
            const timeout = (window.TIMING && window.TIMING.DISCOVERY_TIMEOUT_MS) || 2000;
            setTimeout(() => handleDiscoveryResult([]), timeout);
          }
          // Discovery status will be updated via WebSocket frames
        } catch (e) {
          setTimeout(() => handleDiscoveryResult([]), 2000);
        }
      } else {
        setTimeout(() => handleDiscoveryResult([]), 2000);
      }
    };
    startArduinoDiscovery();
  }

  function renderSelectContent() {
    if (!discoveredSensors?.length) return "";
    const iconClass = getSensorIconClass(currentDiscoveryType, true);
    const sensorListHtml = discoveredSensors.map(sensor => {
      const isSelected = selectedSensor?.index === sensor.index;
      return `<div class="sensor-item ${isSelected ? "selected" : ""}" data-index="${sensor.index}" data-name="${window.escapeHtml(sensor.Name || "")}"><div class="sensor-icon ${iconClass}"></div><div class="sensor-name">${window.escapeHtml(sensor.Name || "Unknown Sensor")}</div>${isSelected ? `<div class="sensor-check"></div>` : ""}</div>`;
    }).join("");
    return `<div class="select-sensor"><div class="sensor-list">${sensorListHtml}</div></div>`;
  }

  function openSelectSensor() {
    setState(STATE.SELECT_SENSOR);
    isSensorOverviewModalOpen = false; // FIX: Ensure overview flag is false in select state
    let saveButton = null;
    const iconMap = { HR: "--icon-sensor-hr-svg-data", CSC: "--icon-sensor-csc-svg-data" };
    AppModal.open({
      title: `Found ${getSensorTypeLabel(currentDiscoveryType)} Sensors`,
      icon: iconMap[currentDiscoveryType] || "--icon-sensor-hr-svg-data",
      subline: `Select your preferred ${getSensorTypeLabel(currentDiscoveryType)} Sensor`,
      content: renderSelectContent(),
      buttons: [
        { id: "search", type: "cancel", label: "Search", className: "btn btn-regular btn-tertiary", closeOnClick: false, onClick: () => startDiscovery(currentDiscoveryType) },
        { id: "save", type: "save", label: "Save", className: "btn btn-regular btn-cta", closeOnClick: false, onClick: () => { if (selectedSensor && saveButton && !saveButton.disabled) saveSelectedSensor(); } }
      ],
      onOpen: () => {
        saveButton = Array.from(document.querySelectorAll(".app-modal-footer button")).find(btn => btn.id === "save" || btn.textContent.trim() === "Save");
        if (saveButton) { saveButton.disabled = true; saveButton.classList.remove("btn-cta"); saveButton.classList.add("btn-inactive"); }
        document.querySelectorAll(".sensor-item").forEach(item => {
          item.addEventListener("click", () => {
            const indexStr = item.getAttribute("data-index");
            const index = indexStr !== null ? parseInt(indexStr, 10) : -1;
            const name = item.getAttribute("data-name");
            // Find the sensor in discoveredSensors
            const sensor = discoveredSensors.find(s => s.index === index);
            selectedSensor = (selectedSensor?.index === index) ? null : { 
              index: index,      // Discovery cache index
              Name: name,        // Display name only
            };
            document.querySelectorAll(".sensor-item").forEach(i => {
              i.classList.remove("selected");
              const check = i.querySelector(".sensor-check");
              if (check) check.remove();
            });
            if (selectedSensor) {
              const selectedItem = Array.from(document.querySelectorAll(".sensor-item")).find(i => i.getAttribute("data-index") === indexStr);
              if (selectedItem) {
                selectedItem.classList.add("selected");
                if (!selectedItem.querySelector(".sensor-check")) {
                  const checkmark = document.createElement("div");
                  checkmark.className = "sensor-check";
                  selectedItem.appendChild(checkmark);
                }
              }
            }
            if (saveButton) {
              saveButton.disabled = !selectedSensor;
              saveButton.classList.toggle("btn-cta", !!selectedSensor);
              saveButton.classList.toggle("btn-inactive", !selectedSensor);
            }
          });
        });
      },
    });
  }

  async function saveSelectedSensor() {
    if (!selectedSensor || selectedSensor.index === undefined) return;
    if (window.ApiV1 && typeof window.ApiV1.setSensor === "function") {
      try {
        // Send only index (backend resolves full data from discovery cache)
        const result = await window.ApiV1.setSensor(selectedSensor.index);
        if (!result.ok) {
        }
      } catch (e) {
      }
    }
    // FIX: Don't close then reopen - just transition directly to sensor overview
    // This prevents the visible modal hide/show flicker
    openSensorOverview();
  }

  const renderErrorContent = () => `<div class="error-modal"><p class="modal-text-muted">Sorry no ${window.escapeHtml(getSensorTypeLabel(currentDiscoveryType))} Sensors found. Ensure sensors reachable and search again.</p></div>`;

  function openError() {
    setState(STATE.ERROR);
    isSensorOverviewModalOpen = false; // FIX: Ensure overview flag is false in error state
    AppModal.open({
      title: "No Sensors Found",
      icon: "--icon-error-svg-data",
      content: renderErrorContent(),
      buttons: [
        { type: "cancel", label: "Return", className: "btn btn-regular btn-secondary", closeOnClick: false, onClick: () => openSensorOverview() },
        { type: "save", label: "Search again", className: "btn btn-regular btn-tertiary", closeOnClick: false, onClick: () => startDiscovery(currentDiscoveryType) }
      ],
      actions: false,
    });
  }

  function handleDiscoveryResult(sensors) {
    if (currentState !== STATE.DISCOVER_SENSORS) return;
    discoveredSensors = Array.isArray(sensors) ? sensors : [];
    selectedSensor = null;
    (discoveredSensors.length > 0 ? openSelectSensor : openError)();
  }

  function open() { openSensorOverview(); }
  function close() {
    isSensorOverviewModalOpen = false; // Mark modal as closed
    AppModal.close();
    setState(STATE.SENSOR_OVERVIEW);
    selectedSensor = null;
    discoveredSensors = [];
    currentDiscoveryType = null;
    // Note: Resume is handled by onCancel callback in AppModal.open() config
  }

  /**
   * Update sensor from API v1 data (for incremental updates)
   */
  function updateFromApi(id, sensorData) {
    // Update configuredSensors array if sensor exists
    const index = configuredSensors.findIndex(s => (s.Type === id || s._apiId === id || s.Name === id));
    if (index >= 0) {
      configuredSensors[index] = {
        ...configuredSensors[index],
        Name: sensorData.name || configuredSensors[index].Name,
        Connected: sensorData.connected !== undefined ? sensorData.connected : configuredSensors[index].Connected,
        Battery: sensorData.battery !== undefined ? sensorData.battery : configuredSensors[index].Battery,
        _apiId: sensorData.id || id
      };
    } else if (sensorData.name) {
      // New sensor - add to array
      configuredSensors.push({
        Name: sensorData.name,
        Type: sensorData.type || id,
        MAC: sensorData.mac || '',
        Connected: sensorData.connected || false,
        Battery: sensorData.battery,
        _apiId: sensorData.id || id
      });
    }
    
    // Update modal if it's open AND we're in SENSOR_OVERVIEW state
    // FIX: Don't update sensor-overview when in DISCOVER_SENSORS or SELECT_SENSOR state
    // This prevents flicker of sensor-overview appearing briefly during discovery
    if (isSensorOverviewModalOpen && currentState === STATE.SENSOR_OVERVIEW) {
      if (_batchUpdateActive) {
        _pendingRender = true; // Defer render until batch ends
      } else {
        updateSensorOverviewModal();
      }
    }
  }
  
  /**
   * Begin batch update mode - suppresses re-renders until endBatchUpdate() is called.
   * Use when applying multiple sensor updates from a single WS frame.
   */
  function beginBatchUpdate() {
    _batchUpdateActive = true;
    _pendingRender = false;
  }
  
  /**
   * End batch update mode - triggers single re-render if any updates were pending.
   */
  function endBatchUpdate() {
    _batchUpdateActive = false;
    if (_pendingRender && isSensorOverviewModalOpen) {
      updateSensorOverviewModal();
    }
    _pendingRender = false;
  }
  
  /**
   * Updates the sensor overview modal content with current sensor states.
   * Called automatically when WebSocket updates arrive and modal is open.
   * Uses debouncing to prevent UI flicker from rapid updates.
   */
  function updateSensorOverviewModal() {
    // Debounce rapid updates to prevent flicker
    if (_updateDebounceTimer) {
      return; // Already scheduled
    }
    
    _updateDebounceTimer = requestAnimationFrame(function() {
      _updateDebounceTimer = null;
      _doUpdateSensorOverviewModal();
    });
  }
  
  /**
   * Internal: Actually performs the modal update after debounce.
   * @private
   */
  function _doUpdateSensorOverviewModal() {
    // Check if modal is actually open (AppModal might have been closed externally)
    const backdrop = document.getElementById("app-modal-backdrop") || document.getElementById("hr-modal-backdrop");
    if (!backdrop || !backdrop.classList.contains("is-open")) {
      isSensorOverviewModalOpen = false;
      return;
    }
    
    // Re-render sensor overview and update modal content
    renderSensorOverview().then(function(content) {
      // RE-CHECK modal state after async operation (FIX: race condition)
      const backdropAfter = document.getElementById("app-modal-backdrop") || document.getElementById("hr-modal-backdrop");
      if (!backdropAfter || !backdropAfter.classList.contains("is-open") || !isSensorOverviewModalOpen) {
        return; // Modal was closed while we were rendering
      }
      
      const contentEl = backdropAfter.querySelector(".app-modal-content");
      if (contentEl) {
        contentEl.innerHTML = content;
        
        // Re-attach event listeners to new buttons (FIX: scope to modal content, not document)
        contentEl.querySelectorAll(".sensor-add-btn").forEach(function(btn) {
          btn.addEventListener("click", function() {
            const type = btn.closest(".sensor-tile")?.getAttribute("data-type");
            if (type) startDiscovery(type);
          });
        });
        
        contentEl.querySelectorAll(".sensor-remove-btn").forEach(function(btn) {
          btn.addEventListener("click", async function() {
            const name = btn.closest(".sensor-tile")?.getAttribute("data-name");
            if (name) {
              if (window.ApiV1 && typeof window.ApiV1.deleteSensor === "function") {
                try {
                  const result = await window.ApiV1.deleteSensor(name);
                  if (result.ok) {
                    // Refresh the sensor overview after successful deletion
                    openSensorOverview();
                  } else {
                  }
                } catch (err) {
                }
              } else {
                // Fallback if function doesn't exist
                openSensorOverview();
              }
            }
          });
        });
      }
    }).catch(function(err) {
    });
  }

  window.BLESensorManager = { open, close, handleDiscoveryResult, updateFromApi, beginBatchUpdate, endBatchUpdate };
  return window.BLESensorManager;
})();

if (typeof window.getAllowedSensorTypes !== "function") {
  window.getAllowedSensorTypes = () => window.DEFAULT_SENSOR_TYPES;
}
window.onSensorDiscoveryResult = (sensors) => {
  if (window.BLESensorManager?.handleDiscoveryResult) window.BLESensorManager.handleDiscoveryResult(sensors);
};
// Initialize setup sensors button (with fallback for dynamic script loading)
function initializeSetupSensorsButton() {
  const btn = document.getElementById("btn-setup-sensors");
  if (btn) btn.addEventListener("click", () => window.BLESensorManager?.open());
}

if (document.readyState === "loading") {
  document.addEventListener("DOMContentLoaded", initializeSetupSensorsButton);
} else {
  initializeSetupSensorsButton();
}

/**
 * ===============================================================
 * SystemManager
 * ===============================================================
 * Manages system-level operations including reset functionality.
 * Provides public API for ESP32/Arduino integration.
 * ===============================================================
 */
(function () {
  const WIFI_RESET_API_ENDPOINT = "/api/v1/action/system/wifi_reset";
  const FACTORY_RESET_API_ENDPOINT = "/api/v1/action/system/factory_reset";
  const DEVICE_RESTART_API_ENDPOINT = "/api/v1/action/system/restart";
  let isRestarting = false;

  /**
   * Shows a confirmation modal with reset options.
   * Displays four buttons: Device Restart, Wifi Reset, Factory Reset, and Cancel.
   *
   * @returns {void}
   */
  function showResetConfirmation() {
    AppModal.open({
      title: "Reset Options",
      icon: "--icon-error-svg-data",
      subline: "Choose a reset option",
      content: `
        <div class="modal-content-wrapper">
          <p class="modal-text">
            Select the type of reset you want to perform:
          </p>
        </div>
      `,
      buttons: [
        {
          type: "save",
          label: "Device Restart",
          className: "btn-cta",
          closeOnClick: false,
          onClick: () => {
            performDeviceRestart();
            return true;
          },
        },
        {
          type: "delete",
          label: "Wifi Reset",
          className: "btn btn-regular btn-danger",
          closeOnClick: false,
          onClick: () => {
            performWifiReset();
            return true;
          },
        },
        {
          type: "delete",
          label: "Factory Reset",
          className: "btn btn-regular btn-danger",
          closeOnClick: false,
          onClick: () => {
            performFactoryReset();
            return true;
          },
        },
        {
          type: "cancel",
          label: "Cancel",
          className: "btn btn-regular btn-tertiary",
          closeOnClick: true,
        },
      ],
      onOpen: () => {
        // Add vertical layout class to footer
        const footerEl = document.querySelector(".app-modal-footer");
        if (footerEl) {
          footerEl.classList.add("app-modal-footer-vertical");
        }
      },
    });
  }

  /**
   * Starts shared reboot monitoring (same as Firmware Update).
   * Polls until device is back, then closes modal and reloads.
   * @private
   */
  function beginRebootMonitoring() {
    if (typeof window.startRebootMonitoring !== "function") return;
    window.startRebootMonitoring({
      isActive: () => isRestarting,
      onSuccess: () => {
        isRestarting = false;
        if (window.AppModal && typeof window.AppModal.close === "function") window.AppModal.close();
        window.location.reload();
      },
    });
  }

  /**
   * Performs device restart by calling the API endpoint.
   * Shows loading/success/error modals and monitors restart.
   *
   * @returns {void}
   * @private
   */
  function performDeviceRestart() {
    AppModal.open({
      title: "Restarting Device...",
      icon: "--icon-spinner-svg-data",
      subline: "Please wait",
      content: `
        <div class="modal-content-centered">
          <p class="modal-text">
            The device is restarting...
          </p>
        </div>
      `,
      buttons: [],
      actions: false,
    });

    fetch(DEVICE_RESTART_API_ENDPOINT, { method: "POST" })
      .then((response) => {
        if (!response.ok) {
          const err = new Error(`HTTP error! status: ${response.status}`);
          err.isHttpError = true;
          err.status = response.status;
          throw err;
        }
        return response.text();
      })
      .then(() => {
        isRestarting = true;
        AppModal.open({
          title: "Device Restarting",
          icon: "--icon-checked-filled-svg-data",
          content: `
            <div class="modal-content-centered">
              <p class="modal-text">
                The device is restarting. The page will reload automatically.
              </p>
            </div>
          `,
          buttons: [],
          actions: false,
        });
        beginRebootMonitoring();
      })
      .catch((error) => {
        if (error && error.isHttpError) {
          isRestarting = false;
          if (typeof window.showErrorModal === "function") {
            window.showErrorModal({
              message: "An error occurred while restarting the device.",
              reason: error.message || "Please try again."
            }, {
              title: "Device Restart Failed"
            });
          }
          return;
        }
        // Connection dropped (e.g. "Load failed") is expected: device restarts and
        // closes the TCP connection before the client receives the full response.
        // Treat as restart in progress and start monitoring.
        isRestarting = true;
        AppModal.open({
          title: "Device Restarting",
          icon: "--icon-checked-filled-svg-data",
          content: `
            <div class="modal-content-centered">
              <p class="modal-text">
                The device is restarting. The page will reload automatically.
              </p>
            </div>
          `,
          buttons: [],
          actions: false,
        });
        beginRebootMonitoring();
      });
  }

  /**
   * Performs the WiFi reset operation by calling the API endpoint.
   * Asynchronously sends POST request to wifi reset endpoint, shows loading/success/error modals, and monitors restart.
   * Uses promises (.then()/.catch()) instead of async/await for compatibility.
   * 
   * @returns {void}
   * @private
   */
  function performWifiReset() {
    // Show loading state
    AppModal.open({
      title: "Resetting WiFi...",
      icon: "--icon-spinner-svg-data",
      subline: "Please wait",
      content: `
        <div class="modal-content-centered">
          <p class="modal-text">
            Resetting WiFi configuration and restarting device...
          </p>
        </div>
      `,
      buttons: [],
      actions: false,
    });

    // Call WiFi reset API
    fetch(WIFI_RESET_API_ENDPOINT, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
    })
      .then((response) => {
        if (!response.ok) {
          throw new Error(`HTTP error! status: ${response.status}`);
        }
        return response.json();
      })
      .then((data) => {
        // Success - show message and start monitoring
        isRestarting = true;
        AppModal.open({
          title: "WiFi Reset Complete",
          icon: "--icon-checked-filled-svg-data",
          content: `
            <div class="modal-content-centered">
              <p class="modal-text">
                WiFi configuration has been reset. The device is restarting...
              </p>
            </div>
          `,
          buttons: [],
          actions: false,
        });

        beginRebootMonitoring();
      })
      .catch((error) => {
        isRestarting = false;
        if (typeof window.showErrorModal === "function") {
          window.showErrorModal({
            message: "An error occurred while resetting WiFi configuration.",
            reason: error.message || "Please try again."
          }, {
            title: "WiFi Reset Failed",
            buttonLabel: "Close"
          });
        }
      });
  }

  /**
   * Performs the factory reset operation by calling the API endpoint.
   * Asynchronously sends POST request to factory reset endpoint, shows loading/success/error modals, and monitors restart.
   * Uses promises (.then()/.catch()) instead of async/await for compatibility.
   * 
   * @returns {void}
   * @private
   */
  function performFactoryReset() {
    // Show loading state
    AppModal.open({
      title: "Performing Factory Reset...",
      icon: "--icon-spinner-svg-data",
      subline: "Please wait",
      content: `
        <div class="modal-content-centered">
          <p class="modal-text">
            Resetting all settings and restarting device...
          </p>
        </div>
      `,
      buttons: [],
      actions: false,
    });

    // Call factory reset API
    fetch(FACTORY_RESET_API_ENDPOINT, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
    })
      .then((response) => {
        if (!response.ok) {
          throw new Error(`HTTP error! status: ${response.status}`);
        }
        return response.json();
      })
      .then((data) => {
        // Success - show message and start monitoring
        isRestarting = true;
        AppModal.open({
          title: "Factory Reset Complete",
          icon: "--icon-checked-filled-svg-data",
          content: `
            <div class="modal-content-centered">
              <p class="modal-text">
                All settings have been reset. The device is restarting...
              </p>
            </div>
          `,
          buttons: [],
          actions: false,
        });

        beginRebootMonitoring();
      })
      .catch((error) => {
        isRestarting = false;
        if (typeof window.showErrorModal === "function") {
          window.showErrorModal({
            message: "An error occurred while performing factory reset.",
            reason: error.message || "Please try again."
          }, {
            title: "Factory Reset Failed",
            buttonLabel: "Close"
          });
        }
      });
  }

  /**
   * Initializes the reset button handler.
   * Attaches click event listener to the reset button element.
   * 
   * @returns {void}
   * @private
   */
  function initializeResetButton() {
    const resetButton = document.getElementById("btn-reset");
    if (!resetButton) {
      return;
    }

    resetButton.addEventListener("click", (e) => {
      e.preventDefault();
      showResetConfirmation();
    });
  }

  // Public API
  const SystemManager = {
    reset: showResetConfirmation,
  };

  // Expose to global scope
  window.SystemManager = SystemManager;

  // Initialize when DOM is ready
  if (document.readyState === "loading") {
    document.addEventListener("DOMContentLoaded", initializeResetButton);
  } else {
    initializeResetButton();
  }
})();