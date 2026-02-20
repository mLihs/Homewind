# UI ↔ Backend: Datenfluss für Listen und Aktionen

Diese Seite beschreibt den **Datenfluss vom und zum UI** für die zentralen Funktionen: **Verbundene Sensoren**, **Heartrate senden**, **Fan Count** und **Fan State**. Sie baut auf den Docs [05_APIS.md](./05_APIS.md) und [WEBSOCKET_PROTOCOL.md](../src/web/WEBSOCKET_PROTOCOL.md) auf.

---

## 1. Verbundene Sensoren (Sensor-Liste und Status)

### UI → Backend (Aktionen)

| Aktion im UI | HTTP API | Handler | Parameter |
|--------------|----------|---------|-----------|
| Sensor aus Discovery auswählen | `POST /api/v1/action/sensor/set` | `handleSensorSet` | `index` (Discovery-Cache-Index) |
| Sensor manuell hinzufügen | `POST /api/v1/action/sensor/set` | `handleSensorSet` | `name`, `type`, `mac`, `addrType` (optional) |
| Sensor verbinden | `POST /api/v1/action/sensor/connect` | `handleSensorConnect` | `name` |
| Sensor trennen | `POST /api/v1/action/sensor/disconnect` | `handleSensorDisconnect` | `name` |
| Sensor löschen | `POST /api/v1/action/sensor/delete` | `handleSensorDelete` | `name` |
| Sensoren neu laden | `POST /api/v1/action/sensor/reload` | `handleSensorReload` | — |
| Discovery starten | `POST /api/v1/action/discovery/start` | `handleDiscoveryStart` | `type` (HR/CSC) |
| Discovery stoppen | `POST /api/v1/action/discovery/stop` | `handleDiscoveryStop` | — |

**Hinweis:** Nach der Auswahl aus der Discovery-Liste ruft die UI `sensor/set` mit **nur `index`** auf. Der Backend löst Name, Typ und MAC über `BikeSensorServerGetSensor(index, ...)` aus dem Discovery-Cache auf.

### Backend → UI (Listen und Updates)

| Quelle | WebSocket-Frame | Inhalt | Verwendung im UI |
|--------|----------------|--------|------------------|
| Snapshot beim Connect / alle ~30 s | `SENSORS_SNAPSHOT` (0x02) | `[count][sensor1:84]…` pro Sensor: name(64), type(1), mac(17), connected(1), battery(1) | Sensor-Übersicht, HR-Widget-Name, verbundene Sensoren-Liste |
| Einzelupdate | `SENSOR_CONN_STATE` (0x03) | `[name_len][name][connected]` | Status „Connected“/„Disconnected“ pro Sensor |
| Nach Discovery-Ende | `DISCOVERY_RESULTS` (0x05) | `[count][index, name_len, name]…` | Liste „Sensoren suchen“; Auswahl → Aufruf `sensor/set?index=…` |

**Referenz:** [05_APIS.md](./05_APIS.md) (Sensor Actions, SENSORS_SNAPSHOT, SENSOR_CONN_STATE), [SENSORS_SNAPSHOT_USAGE_ANALYSIS.md](./SENSORS_SNAPSHOT_USAGE_ANALYSIS.md).

---

## 2. Heartrate senden (HR-Wert und Min/Max-Einstellungen)

### UI → Backend (Einstellungen)

| Aktion im UI | HTTP API | Handler | Parameter |
|--------------|----------|---------|-----------|
| Min. Herzfrequenz setzen | `POST /api/v1/action/heartrate/setmin` | `handleHeartRateSetMin` | `value` (BPM) |
| Max. Herzfrequenz setzen | `POST /api/v1/action/heartrate/setmax` | `handleHeartRateSetMax` | `value` (BPM) |

**Implementierung UI:** `saveHeartRateSetting("min"|"max", value)` → `POST /api/v1/action/heartrate/set{min|max}` mit `value` (z. B. in `app.js`).

### Backend → UI (HR-Wert und Einstellungen)

| Quelle | WebSocket-Frame | Inhalt | Verwendung im UI |
|--------|----------------|--------|------------------|
| HR-Änderung (vom Sensor) | `HEART_RATE` (0x06) | `[value:uint16]` (BPM, LE) | Anzeige aktueller Puls |
| Snapshot / nach Connect | `HEART_RATE_SETTINGS` (0x0B) | `[min:uint16][max:uint16]` (LE) | Min-/Max-Slider; es gibt **kein** HTTP GET für HR-Settings mehr – nur WebSocket. |

**Referenz:** [05_APIS.md](./05_APIS.md) (HEART_RATE, HEART_RATE_SETTINGS), [WEBSOCKET_PROTOCOL.md](../src/web/WEBSOCKET_PROTOCOL.md).

---

## 3. Fan Count (Anzahl Lüfter)

### UI → Backend

Die **Anzahl** der Lüfter wird nicht per HTTP gesetzt. Sie ergibt sich aus:
- **Fan hinzufügen:** `POST /api/v1/action/fan/add?token=<32-hex>` → `handleFanAdd`
- **Fan entfernen:** `POST /api/v1/action/fan/remove?token=…` → `handleFanRemove`

Die UI begrenzt die Anzeige/Logik mit `MAX_FANS` (z. B. 4) und nutzt `fanManager.getFanCount()` (= Anzahl `.fan-card`-Elemente), die aus dem letzten `FANS_SNAPSHOT` stammt.

### Backend → UI

| Quelle | WebSocket-Frame | Inhalt | Verwendung im UI |
|--------|----------------|--------|------------------|
| Snapshot beim Connect / alle ~30 s / nach Refresh | `FANS_SNAPSHOT` (0x07) | `[count:uint8][fan1:35]…[fanN:35]` | `count` = Fan Count; pro Fan: token(32), connected(1), controlActive(1), recovery(1). UI baut daraus die Fan-Cards und `getFanCount()` |

**Refresh:** `POST /api/v1/action/telemetry/refresh?client_id=<id>` markiert Snapshots als dirty; beim nächsten `loopFlush()` erhält der Client erneut `FANS_SNAPSHOT` (und ggf. `SENSORS_SNAPSHOT`).

**Referenz:** [05_APIS.md](./05_APIS.md) (FANS_SNAPSHOT, Fan State Interpretation), [WEBSOCKET_PROTOCOL.md](../src/web/WEBSOCKET_PROTOCOL.md).

---

## 4. Fan State (Ein/Aus, ACTIVE/INACTIVE/ERROR)

### UI → Backend

| Aktion im UI | HTTP API | Handler | Parameter |
|--------------|----------|---------|-----------|
| Lüfter ein/aus (Teilnahme an HR-Steuerung) | `POST /api/v1/action/fan/control` | `handleFanControl` | `token`, `controlState` (`ACTIVE` oder `INACTIVE`) |

**Implementierung UI:** `setFanControlState(token, controlState)` → `httpPost("/api/v1/action/fan/control", { token, controlState })`. Throttling: schnelle Toggles (&lt; 500 ms) können abgelehnt werden.

### Backend → UI

| Quelle | WebSocket-Frame | Inhalt | Verwendung im UI |
|--------|----------------|--------|------------------|
| Snapshot | `FANS_SNAPSHOT` (0x07) | Pro Fan: `connected`, `control_active`, `recovery` | Darstellung als **Fan State**: ERROR / INACTIVE / ACTIVE (siehe Tabelle unten) |

**Fan State Interpretation (aus 05_APIS.md):**

| connected | control_active | UI State   | Bedeutung |
|-----------|----------------|-----------|-----------|
| 0         | 0              | **ERROR** | Nicht verbunden oder Handshake fehlgeschlagen |
| 1         | 0              | **INACTIVE** | Verbunden, nimmt nicht an HR-Steuerung teil |
| 1         | 1              | **ACTIVE**   | Verbunden, nimmt an HR-Steuerung teil |

Zusätzlich: `recovery` (0=normal, 1=recovering, 2=exhausted) für UI-Hinweise (z. B. „Wird wiederhergestellt“).

**Referenz:** [05_APIS.md](./05_APIS.md) (Fan State Interpretation, Control Fan), [FAN_ERROR_ANALYSIS.md](./FAN_ERROR_ANALYSIS.md).

---

## Kurzüberblick: Wo steht was?

| Thema | HTTP APIs | WebSocket Frames | Ausführliche Docs |
|-------|-----------|------------------|--------------------|
| Verbundene Sensoren | sensor/set, sensor/connect, sensor/disconnect, sensor/delete, discovery/start, discovery/stop | SENSORS_SNAPSHOT, SENSOR_CONN_STATE, DISCOVERY_RESULTS | 05_APIS.md, SENSORS_SNAPSHOT_USAGE_ANALYSIS.md |
| Heartrate senden | heartrate/setmin, heartrate/setmax | HEART_RATE, HEART_RATE_SETTINGS | 05_APIS.md, WEBSOCKET_PROTOCOL.md |
| Fan Count | fan/add, fan/remove, telemetry/refresh | FANS_SNAPSHOT (count) | 05_APIS.md |
| Fan State | fan/control | FANS_SNAPSHOT (connected, control_active, recovery) | 05_APIS.md, FAN_ERROR_ANALYSIS.md |

---

**Letzte Aktualisierung:** 2026-01-29
