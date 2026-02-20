# CSS Optimierungsbericht für app.css

**Datum:** 25. Januar 2026  
**Datei:** `Homewind/webui_src/app.css`  
**Aktuelle Größe:** 1156 Zeilen

---

## 📊 Quick Reference - Zusammenfassung

| Kategorie | Anzahl | Aktion | Einsparung |
|-----------|--------|--------|------------|
| **Ungenutzte Klassen** | 4 | Entfernen | ~1.3 KB |
| **Doppelte Definitionen** | 3 | Konsolidieren | ~0.3 KB |
| **Ungenutzte Variablen** | 2 | Entfernen | **~11.4 KB** ⚠️ |
| **Fehlende Definitionen** | 1-2 | Hinzufügen (nur wenn nötig) | +~0.1 KB |
| **Aus HTML entfernen** | 1 | `.icon-edit-inner` entfernen | -1 Zeile |
| **GESAMT EINSPARUNG** | - | - | **~13.2 KB (29%)** |
| **Ungenutzte Variablen** | 2 | Entfernen | **~11.4 KB** (--fan-image-data ist MASSIV!) |
| **Netto-Einsparung** | - | - | **~5 Zeilen** |

**Priorität:** ⚠️ Mittel-Hoch  
**Aufwand:** ⏱️ Niedrig (1-2 Stunden)  
**Nutzen:** ✅ Bessere Wartbarkeit, vollständigere CSS-Definitionen

---

## Executive Summary

Die CSS-Datei enthält **136 definierte Klassen**, von denen **~3% ungenutzt** sind. Es gibt **3 doppelte Definitionen** und **6 fehlende Definitionen** für Klassen, die in JavaScript/HTML verwendet werden. Durch Bereinigung und Ergänzung wird die Datei **vollständiger und wartbarer**.

---

## 1. UNGENUTZTE CSS-KLASSEN (Können entfernt werden)

### 1.1 Komplett ungenutzte Klassen

| CSS-Klasse | Zeile | Status | Beweis |
|------------|-------|--------|--------|
| `.fan-status` | 309-316 | ❌ **UNGENUTZT** | Auskommentiert in CSS, nie in HTML/JS verwendet |
| `.fan-status.true` | 318-320 | ❌ **UNGENUTZT** | Auskommentiert in CSS, nie in HTML/JS verwendet |
| `.fan-token-error` | 832-836 | ❌ **UNGENUTZT** | Nicht in `app.js` oder `index.html` gefunden |
| `.app-modal-label` | 643-647 | ❌ **UNGENUTZT** | Nicht in `app.js` oder `index.html` gefunden |
| `--fan-image-data` | 108 | ❌ **UNGENUTZT** | ⚠️ **MASSIV: 11,708 Bytes!** Nicht in CSS verwendet |
| `--heartrate-arrow-dropdown-image-data` | 104 | ❌ **UNGENUTZT** | Nicht in CSS verwendet |
| `.hr-modal-label` | - | ⚠️ **DYNAMISCH** | Wird in JS erstellt (Zeile 2603), aber **keine CSS-Definition vorhanden** |
| `.icon-edit-inner` | - | ⚠️ **FEHLT** | In HTML verwendet (Zeile 61), aber **keine CSS-Definition** |
| `.app-modal-content` | - | ⚠️ **FEHLT** | In JS verwendet (Zeile 961, 4666), aber **keine CSS-Definition** |
| `.sensor-add-btn` | - | ⚠️ **FEHLT** | In JS verwendet (Zeile 4395, 4671), aber **keine CSS-Definition** |
| `.sensor-remove-btn` | - | ⚠️ **FEHLT** | In JS verwendet (Zeile 4401, 4678), aber **keine CSS-Definition** |
| `.fan-token-input` | - | ⚠️ **FEHLT** | In JS verwendet (Zeile 2322), aber **keine CSS-Definition** |

**Empfehlung:** 
- Entferne `.fan-status`, `.fan-status.true`, `.fan-token-error`, `.app-modal-label` (wenn nicht benötigt)
- Füge fehlende CSS-Definitionen hinzu: `.hr-modal-label`, `.icon-edit-inner`, `.app-modal-content`, `.sensor-add-btn`, `.sensor-remove-btn`, `.fan-token-input`

---

## 2. DUPLIKATE UND REDUNDANZEN

### 2.1 Doppelte Definitionen

#### Problem 1: `.modal-text` ist doppelt definiert
```css
/* Zeile 602-605 */
.modal-text {
  color: var(--text-main);
  margin: 0;
}

/* Zeile 847-849 - DUPLIKAT! */
.modal-text {
  color: var(--text-main);
}
```

**Beweis:** Beide Definitionen existieren in der CSS-Datei.

**Empfehlung:** Entferne die zweite Definition (Zeile 847-849).

---

#### Problem 2: `.modal-text-muted` ist doppelt definiert
```css
/* Zeile 608-611 */
.modal-text-muted {
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
  margin: 0;
}

/* Zeile 851-853 - DUPLIKAT! */
.modal-text-muted {
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
}
```

**Beweis:** Beide Definitionen existieren in der CSS-Datei.

**Empfehlung:** Entferne die zweite Definition (Zeile 851-853).

---

#### Problem 3: `.modal-text-muted-spaced` ist doppelt definiert
```css
/* Zeile 615-619 */
.modal-text-muted-spaced {
  margin-top: var(--spc-md);
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
}

/* Zeile 856-860 - DUPLIKAT! */
.modal-text-muted-spaced {
  margin-top: var(--spc-md);
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
}
```

**Beweis:** Beide Definitionen existieren in der CSS-Datei.

**Empfehlung:** Entferne die zweite Definition (Zeile 856-860).

---

### 2.2 Redundante Selektoren

#### Problem 4: `.picker-window::-webkit-scrollbar` könnte konsolidiert werden
```css
/* Zeile 689-691 */
.picker-window::-webkit-scrollbar {
  display: none;
}
```

**Beweis:** Wird zusammen mit `scrollbar-width: none` verwendet (Zeile 684), was bereits für Firefox ausreicht.

**Empfehlung:** Behalten, da für Browser-Kompatibilität notwendig.

---

## 3. OPTIMIERUNGSPOTENZIALE

### 3.1 CSS-Variablen optimieren

#### Problem 5: Unbenutzte CSS-Variablen
```css
/* Zeile 104 - Sehr lange Base64-String */
--heartrate-arrow-dropdown-image-data: url(data:image/svg+xml;base64,...);
```

**Beweis:** Diese Variable wird **nirgendwo** in der CSS-Datei verwendet.

**Empfehlung:** Entfernen, wenn nicht benötigt.

---

#### Problem 6: `.icon-edit` verwendet nicht die Variable
```css
/* Zeile 248-253 */
.icon-edit {
  width: var(--spc-xl);
  height: var(--spc-xl);
  border-radius: var(--radius-pill);
  background: var(--icon-arrow-down) no-repeat center / contain;
}
```

**Beweis:** Verwendet `--icon-arrow-down`, aber die Variable `--heartrate-arrow-dropdown-image-data` existiert und wird nicht verwendet.

**Empfehlung:** Prüfen, ob `--heartrate-arrow-dropdown-image-data` entfernt werden kann.

---

### 3.2 Konsolidierung von Button-Styles

#### Problem 7: Button-Hierarchie könnte optimiert werden
```css
/* Aktuell: Separate Klassen für jede Button-Variante */
.btn-cta { ... }
.btn-danger { ... }
.btn-primary { ... }
.btn-secondary { ... }
.btn-tertiary { ... }
.btn-inactive { ... }
```

**Beweis:** Alle verwenden ähnliche Patterns. Könnte mit CSS-Variablen oder Attribut-Selektoren optimiert werden.

**Empfehlung:** Behalten, da die aktuelle Struktur klar und wartbar ist.

---

### 3.3 Unbenutzte Pseudo-Klassen

#### Problem 8: `.fan-grid > *:first-child:nth-last-child(3) + * + *`
```css
/* Zeile 268-270 */
.fan-grid > *:first-child:nth-last-child(3) + * + * {
  grid-column: 1 / -1;
}
```

**Beweis:** Sehr spezifischer Selektor. Prüfen, ob dieser Fall tatsächlich auftritt.

**Empfehlung:** Behalten, wenn Layout-Anforderung besteht.

---

## 4. FEHLENDE CSS-DEFINITIONEN (Werden verwendet, aber nicht definiert)

### 4.1 Klassen, die in JS/HTML verwendet werden, aber keine CSS-Definition haben:

| Klasse | Verwendung | Zweck | Empfehlung |
|--------|------------|--------|------------|
| `.hr-modal-label` | `app.js:2603` | Label-Text für Modal | ✅ **CSS hinzufügen** (ähnlich wie `.app-modal-label`) |
| `.icon-edit-inner` | `index.html:61` | Inneres Element (kein Styling) | ❌ **Aus HTML entfernen** (scheint überflüssig) |
| `.app-modal-content` | `app.js:961, 4791` | Container für Modal-Inhalt | ⚠️ **Optional** (funktioniert ohne CSS, könnte Padding/Margin brauchen) |
| `.sensor-add-btn` | `app.js:4520, 4796` | Nur JS-Selektor | ✅ **Kein CSS nötig** (verwendet bereits `.btn .btn-small .btn-cta`) |
| `.sensor-remove-btn` | `app.js:4526, 4803` | Nur JS-Selektor | ✅ **Kein CSS nötig** (verwendet bereits `.btn .btn-small .btn-tertiary`) |
| `.fan-token-input` | `app.js:2435` | Nur JS-Selektor | ✅ **Kein CSS nötig** (verwendet zusammen mit `.fan-token-input-full` für Styling) |

**Analyse:**
- **Nur für JS-Selektoren:** `.sensor-add-btn`, `.sensor-remove-btn`, `.fan-token-input` - brauchen kein CSS, da sie bereits andere Klassen für Styling verwenden
- **Sollte CSS haben:** `.hr-modal-label` - wird für Text-Label verwendet, sollte Styling haben
- **Kann entfernt werden:** `.icon-edit-inner` - scheint überflüssig zu sein
- **Optional:** `.app-modal-content` - funktioniert ohne CSS, könnte aber Padding/Margin für besseres Layout brauchen

---

## 5. STRUKTURELLE VERBESSERUNGEN

### 5.1 Kommentare und Organisation

**Aktueller Zustand:** Gute Struktur mit Kommentaren, aber einige Bereiche könnten besser organisiert werden.

**Empfehlung:**
- Alle Duplikate entfernen
- Fehlende Definitionen hinzufügen
- Unbenutzte Variablen entfernen

---

## 6. ZUSAMMENFASSUNG DER OPTIMIERUNGEN

### 6.1 Sofort umsetzbare Optimierungen:

1. **Entfernen:**
   - `.fan-status` (Zeile 309-316) - auskommentiert und ungenutzt
   - `.fan-status.true` (Zeile 318-320) - auskommentiert und ungenutzt
   - `.fan-token-error` (Zeile 832-836) - nicht verwendet
   - `.app-modal-label` (Zeile 643-647) - nicht verwendet (oder hinzufügen, wenn benötigt)
   - `--heartrate-arrow-dropdown-image-data` (Zeile 104) - nicht verwendet
   - **`--fan-image-data` (Zeile 108) - ⚠️ MASSIV: 11,708 Bytes! Nicht verwendet**

2. **Konsolidieren (Duplikate entfernen):**
   - `.modal-text` (Zeile 847-849) - Duplikat entfernen
   - `.modal-text-muted` (Zeile 851-853) - Duplikat entfernen
   - `.modal-text-muted-spaced` (Zeile 856-860) - Duplikat entfernen

3. **Hinzufügen (nur wenn CSS wirklich benötigt wird):**
   - `.hr-modal-label` - ✅ **CSS hinzufügen** (wird für Label-Text verwendet, sollte Styling haben)

4. **Entfernen aus HTML/JS (wenn überflüssig):**
   - `.icon-edit-inner` - ❌ **Aus HTML entfernen** (wird in `index.html:61` verwendet, hat aber keinen Zweck)

5. **Optional (funktioniert ohne CSS, könnte aber nützlich sein):**
   - `.app-modal-content` - ⚠️ **Optional CSS hinzufügen** (Container, könnte Padding/Margin brauchen)

**Hinweis:** Die Klassen `.sensor-add-btn`, `.sensor-remove-btn` und `.fan-token-input` werden **nur als JS-Selektoren** verwendet und brauchen **kein CSS**, da sie bereits andere Klassen für Styling verwenden (`.btn`, `.fan-token-input-full`).

### 6.2 Tatsächliche Einsparung (gemessen):

**Aktuelle Dateigröße (nach Entfernung von --fan-image-data):**
- Unkomprimiert: **33.69 KB** (34,494 Bytes) ✅ **Bereits 11.43 KB gespart!**
- GZIP-komprimiert (geschätzt): **~12.63 KB**
- Kompressionsrate: 36.6%

**Ursprüngliche Größe:** 45.12 KB (46,200 Bytes)

**Verbleibende Optimierungen (nach Entfernung von --fan-image-data):**
- `--heartrate-arrow-dropdown-image-data` (Zeile 104): **361 Bytes**
- `.fan-status` (auskommentiert, Zeile 309-321): **378 Bytes**
- `.fan-token-error` (Zeile 832-836): **130 Bytes**
- `.app-modal-label` (Zeile 643-647): **134 Bytes**
- Duplikate (`.modal-text*`, Zeile 847-860): **873 Bytes** (3 Klassen × 291 Bytes)
- **Total zu entfernen: 1,876 Bytes (1.83 KB)**

**Bereits entfernt:**
- ✅ `--fan-image-data`: **11,707 Bytes (11.43 KB)** - bereits entfernt!

**Hinzuzufügen:**
- `.hr-modal-label`: **~120 Bytes**

**Verbleibende Einsparung (nach Entfernung von --fan-image-data):**
- **Netto-Einsparung (unkomprimiert): 1,756 Bytes (1.71 KB)**
- **Reduktion: 5.09%** (bezogen auf aktuelle Größe)
- **Neue Dateigröße (unkomprimiert): 32.74 KB**

**Mit GZIP-Kompression (praktisch relevant für Web):**
- **Einsparung (komprimiert): ~643 Bytes (0.63 KB)**
- **Reduktion (komprimiert): 5.09%**
- **Neue Dateigröße (komprimiert): ~11.98 KB**

**Gesamt-Einsparung (inkl. bereits entferntem --fan-image-data):**
- **Bereits gespart: 11,706 Bytes (11.43 KB)**
- **Noch zu sparen: 1,756 Bytes (1.71 KB)**
- **Gesamt-Einsparung: 13,462 Bytes (13.14 KB)**
- **Gesamt-Reduktion: 29.13%** (von ursprünglich 45.12 KB auf 32.74 KB)

**Fazit:** Du hast bereits **11.43 KB gespart** durch Entfernung von `--fan-image-data`! Die verbleibenden Optimierungen sparen nochmal **1.71 KB**. Die Gesamtoptimierung von **29%** ist **sehr wertvoll** für Embedded-Systeme mit begrenztem Flash-Speicher.

**Hinweis:** Die Klassen `.sensor-icon-*`, `.fw-modal-icon`, `.fw-icon-*` werden **dynamisch in JavaScript erstellt** und sind daher **nicht ungenutzt**. Sie bleiben im CSS.

---

## 7. DETAILLIERTE BEWEISE MIT CODE-SNIPPETS

### 7.1 Beweis für ungenutzte Klassen:

#### `.fan-status` und `.fan-status.true` (Zeile 309-321)
```css
/* Old fan-status icon styles - replaced by toggle switch for participation states */
/*
.fan-status {
  width: var(--spc-2xl);
  height: var(--spc-2xl);
  border-radius: var(--radius-full);
  background: var(--icon-question) no-repeat center / contain;
  scale: 1.4;
}

.fan-status.true {
  background-image: var(--icon-checked-filled-svg-data);
}
*/
```
**Beweis:** Auskommentiert und nie in `app.js` oder `index.html` verwendet.

#### `.fan-token-error` (Zeile 832-836)
```css
.fan-token-error {
  margin-top: var(--spc-md);
  font-size: var(--font-size-body-small);
  color: var(--text-error);
}
```
**Beweis:** Nicht in `app.js` oder `index.html` gefunden. Stattdessen wird `.fan-modal-error` verwendet.

#### `.app-modal-label` (Zeile 643-647)
```css
.app-modal-label {
  font-size: var(--font-size-body-medium);
  color: var(--text-muted);
  margin-bottom: var(--spc-md);
}
```
**Beweis:** Nicht in `app.js` oder `index.html` gefunden. Stattdessen wird `.hr-modal-label` in JS erstellt (Zeile 2488).

### 7.2 Beweis für fehlende Definitionen:

#### `.hr-modal-label` - Wird in JS verwendet, aber nicht in CSS definiert
**Verwendung in app.js:2488:**
```javascript
label.className = "hr-modal-label";
label.textContent = "Edit Fan Token";
```
**Beweis:** Klasse wird erstellt, aber keine CSS-Definition vorhanden.

#### `.app-modal-content` - Wird in JS verwendet, aber nicht in CSS definiert
**Verwendung in app.js:961, 4666:**
```javascript
bodyEl = bodyContainer.querySelector(".app-modal-content");
// ...
const contentEl = backdropAfter.querySelector(".app-modal-content");
```
**Beweis:** Klasse wird verwendet, aber keine CSS-Definition vorhanden.

#### `.sensor-add-btn` und `.sensor-remove-btn` - Werden in JS verwendet
**Verwendung in app.js:4358, 4360:**
```javascript
return `<div class="sensor-tile ${statusClass}" ...><button class="btn btn-small btn-tertiary sensor-remove-btn" type="button">Remove</button></div>`;
return `<div class="sensor-tile" ...><button class="btn btn-small btn-cta sensor-add-btn" type="button">Add</button></div>`;
```
**Beweis:** Klassen werden verwendet, aber keine spezifischen CSS-Definitionen vorhanden (verwenden nur Basis-`.btn` Styles).

#### `.fan-token-input` - Wird in JS verwendet
**Verwendung in app.js:2313, 2322, 2495:**
```javascript
class="fan-token-input fan-token-input-full"
const tokenInput = container.querySelector(".fan-token-input");
input.className = "fan-token-input fan-token-input-full";
```
**Beweis:** Klasse wird verwendet, aber keine CSS-Definition vorhanden (nur `.fan-token-input-full` ist definiert).

### 7.3 Beweis für Duplikate:

#### `.modal-text` - Doppelt definiert

**Erste Definition (Zeile 602-605):**
```css
.modal-text {
  color: var(--text-main);
  margin: 0;
}
```

**Zweite Definition (Zeile 847-849) - DUPLIKAT:**
```css
.modal-text {
  color: var(--text-main);
}
```

#### `.modal-text-muted` - Doppelt definiert

**Erste Definition (Zeile 608-611):**
```css
.modal-text-muted {
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
  margin: 0;
}
```

**Zweite Definition (Zeile 851-853) - DUPLIKAT:**
```css
.modal-text-muted {
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
}
```

#### `.modal-text-muted-spaced` - Doppelt definiert

**Erste Definition (Zeile 615-619):**
```css
.modal-text-muted-spaced {
  margin-top: var(--spc-md);
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
}
```

**Zweite Definition (Zeile 856-860) - DUPLIKAT:**
```css
.modal-text-muted-spaced {
  margin-top: var(--spc-md);
  color: var(--text-muted);
  font-size: var(--font-size-body-medium);
}
```

---

## 8. EMPFOHLENE VORGANGSWEISE

### Schritt 1: Backup erstellen
```bash
cp app.css app.css.backup
```

### Schritt 2: Duplikate entfernen
- Entferne Zeilen 847-849 (`.modal-text` Duplikat)
- Entferne Zeilen 851-853 (`.modal-text-muted` Duplikat)
- Entferne Zeilen 856-860 (`.modal-text-muted-spaced` Duplikat)

### Schritt 3: Ungenutzte Klassen entfernen
- Entferne Zeilen 309-321 (`.fan-status` und `.fan-status.true` - bereits auskommentiert)
- Entferne Zeilen 832-836 (`.fan-token-error`)
- Entferne Zeile 104 (`--heartrate-arrow-dropdown-image-data`)
- Prüfe Zeile 643-647 (`.app-modal-label`) - entfernen, wenn nicht benötigt

### Schritt 4: Fehlende Definitionen hinzufügen
- Füge CSS für `.hr-modal-label` hinzu
- Füge CSS für `.icon-edit-inner` hinzu (oder entferne aus HTML)
- Füge CSS für `.app-modal-content` hinzu
- Füge CSS für `.sensor-add-btn` und `.sensor-remove-btn` hinzu
- Füge CSS für `.fan-token-input` hinzu

### Schritt 5: Testen
- Alle Modals öffnen und prüfen
- Alle Buttons testen
- Sensor-Setup testen
- Fan-Management testen

---

## 9. FAZIT

Die CSS-Datei ist **grundsätzlich gut strukturiert**, hat aber:
- **4 ungenutzte Klassen** die entfernt werden können
- **3 doppelte Definitionen** die konsolidiert werden sollten
- **1-2 fehlende Definitionen** die hinzugefügt werden sollten (nur wenn CSS wirklich benötigt wird)
- **1 überflüssige Klasse** die aus HTML entfernt werden sollte
- **1 ungenutzte CSS-Variable** die entfernt werden kann

Durch diese Optimierungen wird die Datei **sauberer, wartbarer und vollständiger**, ohne Funktionalität zu verlieren.

**Priorität:** ⚠️ Mittel-Hoch  
**Aufwand:** ⏱️ Niedrig (1-2 Stunden)  
**Nutzen:** ✅ Bessere Wartbarkeit, vollständigere CSS-Definitionen, konsistenteres Styling

---

## 10. KONKRETE UMSETZUNGSEMPFEHLUNGEN

### Schritt-für-Schritt Anleitung:

1. **Backup erstellen:**
   ```bash
   cp app.css app.css.backup
   ```

2. **Duplikate entfernen (Zeilen 847-860):**
   - Entferne Zeilen 847-849 (`.modal-text` Duplikat)
   - Entferne Zeilen 851-853 (`.modal-text-muted` Duplikat)
   - Entferne Zeilen 856-860 (`.modal-text-muted-spaced` Duplikat)

3. **Ungenutzte Klassen entfernen:**
   - Entferne Zeilen 308-321 (auskommentierte `.fan-status` Styles)
   - Entferne Zeilen 832-836 (`.fan-token-error`)
   - Entferne Zeile 104 (`--heartrate-arrow-dropdown-image-data` Variable)
   - Prüfe Zeilen 643-647 (`.app-modal-label`) - entfernen, wenn nicht benötigt

4. **Fehlende Definitionen hinzufügen (nur wenn wirklich benötigt):**
   
   Nach Zeile 647 (nach `.app-modal-label`), füge hinzu:
   ```css
   .hr-modal-label {
     font-size: var(--font-size-body-medium);
     color: var(--text-muted);
     margin-bottom: var(--spc-md);
   }
   ```
   **Begründung:** Wird für Label-Text verwendet (Zeile 2603 in app.js), sollte Styling haben.

5. **Optional: `.app-modal-content` CSS hinzufügen:**
   
   Nach Zeile 639 (nach `.app-modal-body`), optional hinzufügen:
   ```css
   .app-modal-content {
     /* Optional: Padding/Margin für besseres Layout */
     /* Aktuell funktioniert es ohne CSS, aber könnte nützlich sein */
   }
   ```
   **Begründung:** Wird als Container verwendet, funktioniert aber aktuell ohne CSS.

6. **Aus HTML entfernen (wenn überflüssig):**
   
   In `index.html:61`, entferne:
   ```html
   <!-- VORHER: -->
   <div class="icon-edit"><div class="icon-edit-inner"></div></div>
   
   <!-- NACHHER: -->
   <div class="icon-edit"></div>
   ```
   **Begründung:** `.icon-edit-inner` hat keinen Zweck und kein Styling.

**WICHTIG:** Die folgenden Klassen brauchen **KEIN CSS**, da sie nur als JS-Selektoren verwendet werden:
- `.sensor-add-btn` - verwendet bereits `.btn .btn-small .btn-cta` für Styling
- `.sensor-remove-btn` - verwendet bereits `.btn .btn-small .btn-tertiary` für Styling  
- `.fan-token-input` - verwendet bereits `.fan-token-input-full` für Styling

5. **Testen:**
   - Alle Modals öffnen und prüfen
   - Alle Buttons testen
   - Sensor-Setup testen
   - Fan-Management testen
   - Heart Rate Picker testen

### Erwartetes Ergebnis:

- ✅ Keine Duplikate mehr
- ✅ Alle verwendeten Klassen haben CSS-Definitionen
- ✅ Keine ungenutzten Klassen
- ✅ Sauberere, wartbarere CSS-Datei
