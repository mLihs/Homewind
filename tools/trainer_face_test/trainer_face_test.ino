// trainer_face_test — gate G5 test sketch for the Homewind trainer proxy (v0.2).
//
// Question: does Zwift (Apple TV, iPad) accept an ESP32 as a controllable
// trainer, and which "face" works under which name?
//
//   F  : release-1 layout: FTMS (0x1826) + Cycling Power + Heart Rate, "homewind-test"
//   P1 : Cycling Power with Wahoo control point a026e005 + HR, "homewind-test"
//   P2 : like P1 + DIS "Wahoo Fitness", name "KICKR TEST"     (comparison only, never ship)
//   T1 : Tacx FE-C over BLE (6E40FEC1) + Cycling Power + HR, "homewind-test"
//   T2 : like T1 + DIS "Tacx", name "Tacx Neo TEST"           (comparison only, never ship)
//
// Every variant uses its own static random BLE address, so iOS/tvOS/Zwift
// caches of one run cannot leak into another. No trainer is needed: a
// loopback model turns ERG targets and grades into simulated power.
//
// Logged on USB serial (115200): every write (hex), every response with the
// result of indicate(), subscriptions, connects. The session doubles as a
// capture of what Zwift sends.
//
// Serial commands:
//   f / p / k / t / n   select variant F / P1 / P2 / T1 / T2 (stored, reboot)
//   s                   status
//   d                   response delay: 0 -> 2000 -> 6000 ms -> 0
//   x                   drop responses on/off
//   l                   FTMS: send Machine Status 0xFF (control permission lost)
//
// Indications follow the BLE rule: at most one outstanding per connection
// and characteristic; the next waits for the confirmation (onStatus) or the
// 30 s ATT timeout. Indications are only sent to subscribed connections.
//
// Board: any ESP32-S3 (XIAO ESP32-S3). Library: NimBLE-Arduino 2.x.
// License: MIT (Homewind tools). Written from the Bluetooth SIG FTMS/CPS/HRS
// specs and public facts; no third-party code.

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "trainer_face_types.h"

// ---------------------------------------------------------------- variant
enum Variant : uint8_t { V_FTMS = 0, V_WAHOO_HW = 1, V_WAHOO_KICKR = 2, V_FEC_HW = 3, V_FEC_TACX = 4 };
static Variant gVariant = V_FTMS;
static const char* variantName(Variant v) {
  switch (v) {
    case V_FTMS: return "F (FTMS, homewind-test)";
    case V_WAHOO_HW: return "P1 (Wahoo ext, homewind-test)";
    case V_WAHOO_KICKR: return "P2 (Wahoo ext, KICKR TEST)";
    case V_FEC_HW: return "T1 (Tacx FE-C over BLE, homewind-test)";
    default: return "T2 (Tacx FE-C over BLE, Tacx Neo TEST)";
  }
}

// ---------------------------------------------------------------- UUIDs
static const NimBLEUUID UUID_FTMS((uint16_t)0x1826);
static const NimBLEUUID UUID_FTMS_FEATURE((uint16_t)0x2ACC);
static const NimBLEUUID UUID_IBD((uint16_t)0x2AD2);
static const NimBLEUUID UUID_PWR_RANGE((uint16_t)0x2AD8);
static const NimBLEUUID UUID_FTMS_CP((uint16_t)0x2AD9);
static const NimBLEUUID UUID_FTMS_STATUS((uint16_t)0x2ADA);

static const NimBLEUUID UUID_CPS((uint16_t)0x1818);
static const NimBLEUUID UUID_CPM((uint16_t)0x2A63);
static const NimBLEUUID UUID_CP_FEATURE((uint16_t)0x2A65);
static const NimBLEUUID UUID_SENSOR_LOC((uint16_t)0x2A5D);
static const NimBLEUUID UUID_WAHOO_CP("a026e005-0a7d-4ab3-97fa-f1500f9feb8b");
static const NimBLEUUID UUID_FEC_SVC("6e40fec1-b5a3-f393-e0a9-e50e24dcca9e");
static const NimBLEUUID UUID_FEC_RX("6e40fec2-b5a3-f393-e0a9-e50e24dcca9e");  // trainer -> app (notify)
static const NimBLEUUID UUID_FEC_TX("6e40fec3-b5a3-f393-e0a9-e50e24dcca9e");  // app -> trainer (write)

static const NimBLEUUID UUID_HRS((uint16_t)0x180D);
static const NimBLEUUID UUID_HRM((uint16_t)0x2A37);

static const NimBLEUUID UUID_DIS((uint16_t)0x180A);
static const NimBLEUUID UUID_MANUF((uint16_t)0x2A29);
static const NimBLEUUID UUID_MODEL((uint16_t)0x2A24);

// ---------------------------------------------------------------- state
static NimBLEServer* gServer = nullptr;
static NimBLECharacteristic *gIbd = nullptr, *gFtmsCp = nullptr, *gFtmsStatus = nullptr;
static NimBLECharacteristic *gCpm = nullptr, *gWahooCp = nullptr, *gFecRx = nullptr, *gHrm = nullptr;

enum Mode : uint8_t { M_IDLE, M_ERG, M_SIM };
struct Model {
  Mode mode = M_IDLE;
  int16_t targetW = 0;
  float gradePct = 0;
  float powerW = 150;
  float cadence = 85;
  uint16_t crankRevs = 0, lastCrankT = 0;
  float crankAcc = 0;
  uint16_t owner = BLE_HS_CONN_HANDLE_NONE;
} gM;

static uint32_t gConnCount = 0, gWriteCount = 0;
static uint32_t gDelayMs = 0;
static bool gDrop = false;
static uint8_t gFecLastCmd = 0xFF, gFecSeq = 0;

static QueueHandle_t gQ;
static const uint8_t W_FTMS = 1, W_WAHOO = 2, W_FEC = 3;
static IndSlot gInd[2];  // [0] FTMS CP or Wahoo CP, [1] unused spare

static void hexLog(const char* tag, uint16_t conn, const uint8_t* d, size_t n) {
  Serial.printf("[%9lu] %-10s conn=%u len=%u :", (unsigned long)millis(), tag, conn, (unsigned)n);
  for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
}

static int slotIdx(IndSlot& s, uint16_t conn, bool create) {
  for (int i = 0; i < MAXC; i++) if (s.conn[i] == conn) return i;
  if (!create) return -1;
  for (int i = 0; i < MAXC; i++) if (s.conn[i] == BLE_HS_CONN_HANDLE_NONE) { s.conn[i] = conn; return i; }
  return -1;
}
static void slotReset(IndSlot& s, int i) {
  s.conn[i] = BLE_HS_CONN_HANDLE_NONE; s.subscribed[i] = s.pending[i] = false; s.qlen[i] = 0;
}

// Queue a response (indication) for conn; sent from loop() when allowed.
static void queueResponse(IndSlot& s, uint16_t conn, const uint8_t* d, uint8_t n, const char* tag) {
  int i = slotIdx(s, conn, true);
  if (i < 0) return;
  if (gDrop) { hexLog("RSP-DROP", conn, d, n); return; }
  if (s.qlen[i]) Serial.printf("  !! previous response for conn=%u still queued, replaced\n", conn);
  memcpy(s.qbuf[i], d, n); s.qlen[i] = n; s.dueAt[i] = millis() + gDelayMs;
  (void)tag;
}
static void pumpIndications(IndSlot& s, const char* tag) {
  if (!s.chr) return;
  uint32_t now = millis();
  for (int i = 0; i < MAXC; i++) {
    if (s.conn[i] == BLE_HS_CONN_HANDLE_NONE) continue;
    if (s.pending[i] && now - s.sentAt[i] > 30000) {
      Serial.printf("  !! indication timeout conn=%u (30 s)\n", s.conn[i]);
      s.pending[i] = false;
    }
    if (!s.qlen[i] || s.pending[i] || (int32_t)(now - s.dueAt[i]) < 0) continue;
    if (!s.subscribed[i]) {
      hexLog("RSP-NOSUB", s.conn[i], s.qbuf[i], s.qlen[i]);  // app never enabled indications
      s.qlen[i] = 0;
      continue;
    }
    bool ok = s.chr->indicate(s.qbuf[i], s.qlen[i], s.conn[i]);
    hexLog(ok ? tag : "RSP-FAIL", s.conn[i], s.qbuf[i], s.qlen[i]);
    if (ok) { s.pending[i] = true; s.sentAt[i] = now; }
    s.qlen[i] = 0;
  }
}

// ---------------------------------------------------------------- callbacks
class ServerCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    gConnCount++;
    Serial.printf("[%9lu] CONNECT    conn=%u addr=%s\n", (unsigned long)millis(),
                  ci.getConnHandle(), ci.getAddress().toString().c_str());
    NimBLEDevice::startAdvertising();  // a second app may connect (logged; control refused)
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    Serial.printf("[%9lu] DISCONNECT conn=%u reason=0x%02X\n", (unsigned long)millis(),
                  ci.getConnHandle(), reason);
    if (gM.owner == ci.getConnHandle()) gM.owner = BLE_HS_CONN_HANDLE_NONE;  // free at once (tool)
    int i = slotIdx(gInd[0], ci.getConnHandle(), false);
    if (i >= 0) slotReset(gInd[0], i);
    NimBLEDevice::startAdvertising();
  }
};

class ChrCB : public NimBLECharacteristicCallbacks {
 public:
  explicit ChrCB(uint8_t which) : which_(which) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    NimBLEAttValue v = c->getValue();
    WriteEvt e{};
    e.which = which_; e.conn = ci.getConnHandle(); e.t = millis();
    e.len = (uint8_t)min((size_t)sizeof(e.data), (size_t)v.length());
    memcpy(e.data, v.data(), e.len);
    if (xQueueSend(gQ, &e, 0) != pdTRUE) Serial.println("  !! write queue full, write lost");
  }
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& ci, uint16_t sub) override {
    Serial.printf("[%9lu] SUBSCRIBE  conn=%u %s sub=%u\n", (unsigned long)millis(), ci.getConnHandle(),
                  c->getUUID().toString().c_str(), sub);
    if (c == gInd[0].chr) {
      int i = slotIdx(gInd[0], ci.getConnHandle(), true);
      if (i >= 0) gInd[0].subscribed[i] = (sub & 0x02) != 0;  // bit 1 = indications
    }
  }
  void onStatus(NimBLECharacteristic* c, NimBLEConnInfo& ci, int code) override {
    if (c == gInd[0].chr) {
      int i = slotIdx(gInd[0], ci.getConnHandle(), false);
      if (i >= 0 && gInd[0].pending[i]) {
        gInd[0].pending[i] = false;
        Serial.printf("[%9lu] IND-STATUS conn=%u code=%d%s\n", (unsigned long)millis(),
                      ci.getConnHandle(), code, code == BLE_HS_EDONE ? " (confirmed)" : "");
      }
    }
  }
 private:
  uint8_t which_;
};

// ---------------------------------------------------------------- FTMS
static void ftmsRespond(uint16_t conn, uint8_t op, uint8_t result) {
  uint8_t r[3] = {0x80, op, result};
  queueResponse(gInd[0], conn, r, 3, "FTMS-RSP");
}
static void ftmsStatus(const uint8_t* d, size_t n) {
  if (gFtmsStatus) { gFtmsStatus->notify(d, n); hexLog("FTMS-STAT", 0, d, n); }
}

static void handleFtms(const WriteEvt& e) {
  hexLog("FTMS-CP", e.conn, e.data, e.len);
  if (e.len < 1) return;
  uint8_t op = e.data[0];
  // results: 0x01 success, 0x02 not supported, 0x03 invalid parameter, 0x05 control not permitted
  if (op == 0x00) {
    if (gM.owner != BLE_HS_CONN_HANDLE_NONE && gM.owner != e.conn) { ftmsRespond(e.conn, op, 0x05); return; }
    gM.owner = e.conn; ftmsRespond(e.conn, op, 0x01); return;
  }
  if (gM.owner != e.conn) { ftmsRespond(e.conn, op, 0x05); return; }
  switch (op) {
    case 0x01:  // reset
      gM.mode = M_IDLE; gM.owner = BLE_HS_CONN_HANDLE_NONE;
      ftmsRespond(e.conn, op, 0x01);
      { uint8_t s[1] = {0x01}; ftmsStatus(s, 1); }
      break;
    case 0x05: {  // target power (sint16 W)
      if (e.len < 3) { ftmsRespond(e.conn, op, 0x03); break; }
      int16_t w = (int16_t)(e.data[1] | (e.data[2] << 8));
      gM.mode = M_ERG; gM.targetW = constrain(w, 0, 1000);
      Serial.printf("  -> ERG %d W (+%lu ms after write)\n", gM.targetW, (unsigned long)(millis() - e.t));
      ftmsRespond(e.conn, op, 0x01);
      uint8_t s[3] = {0x08, e.data[1], e.data[2]}; ftmsStatus(s, 3);
      break;
    }
    case 0x07: ftmsRespond(e.conn, op, 0x01); { uint8_t s[1] = {0x04}; ftmsStatus(s, 1); } break;
    case 0x08: ftmsRespond(e.conn, op, 0x01);
               { uint8_t s[2] = {0x02, e.len > 1 ? e.data[1] : (uint8_t)0x01}; ftmsStatus(s, 2); } break;
    case 0x11: {  // indoor bike simulation: wind sint16 0.001 m/s, grade sint16 0.01 %, crr uint8 0.0001, cw uint8 0.01
      if (e.len < 7) { ftmsRespond(e.conn, op, 0x03); break; }
      int16_t wind = (int16_t)(e.data[1] | (e.data[2] << 8));
      int16_t grade = (int16_t)(e.data[3] | (e.data[4] << 8));
      gM.mode = M_SIM; gM.gradePct = constrain(grade / 100.0f, -15.0f, 20.0f);  // clamp, never reject
      Serial.printf("  -> SIM grade %.2f %% wind %.3f m/s crr %.4f cw %.2f\n", grade / 100.0f,
                    wind / 1000.0f, e.data[5] / 10000.0f, e.data[6] / 100.0f);
      ftmsRespond(e.conn, op, 0x01);
      uint8_t s[7] = {0x12, e.data[1], e.data[2], e.data[3], e.data[4], e.data[5], e.data[6]};
      ftmsStatus(s, 7);
      break;
    }
    default: ftmsRespond(e.conn, op, 0x02);  // includes 0x04 resistance: not offered in R1
  }
}

// ---------------------------------------------------------------- Wahoo
// Facts (public, REPORTED): 0x20 unlock (EE FC), 0x40 resistance, 0x41 level,
// 0x42 ERG uint16 W, 0x43 sim (weight, crr, cw), 0x44 crr, 0x45 wind coeff,
// 0x46 grade, 0x47 wind speed, 0x48 wheel circumference. Response format is
// UNKNOWN: the tool answers [0x01, opcode]; the logged requests are the point.
static void handleWahoo(const WriteEvt& e) {
  hexLog("WAHOO-CP", e.conn, e.data, e.len);
  if (e.len < 1) return;
  uint8_t op = e.data[0];
  if (gM.owner == BLE_HS_CONN_HANDLE_NONE) gM.owner = e.conn;
  if (op == 0x42 && e.len >= 3) {
    gM.mode = M_ERG; gM.targetW = constrain((int)(e.data[1] | (e.data[2] << 8)), 0, 1000);
    Serial.printf("  -> ERG %d W\n", gM.targetW);
  } else if (op == 0x46 && e.len >= 3) {
    gM.mode = M_SIM;
    uint16_t raw = e.data[1] | (e.data[2] << 8);
    // two encodings are reported; print both, decide from the capture
    Serial.printf("  -> grade raw 0x%04X | as (raw-32768)/100 = %.2f %% | as sint16/100 = %.2f %%\n",
                  raw, ((int)raw - 32768) / 100.0f, (int16_t)raw / 100.0f);
  }
  uint8_t r[2] = {0x01, op};
  queueResponse(gInd[0], e.conn, r, 2, "WAHOO-RSP");
}

// ---------------------------------------------------------------- Tacx FE-C over BLE
// Frame (REPORTED): A4 09 4E 05 <8-byte ANT page> <checksum>, checksum = XOR of
// all preceding bytes incl. A4. App pages: 49 (0x31) target power, 51 (0x33)
// track resistance, 50 (0x32) wind, 55 (0x37) user config, 70 (0x46) request.
static uint8_t gFecEvt = 0;
static void fecSend(const uint8_t page[8]) {
  uint8_t f[13] = {0xA4, 0x09, 0x4E, 0x05};
  memcpy(f + 4, page, 8);
  uint8_t x = 0;
  for (int i = 0; i < 12; i++) x ^= f[i];
  f[12] = x;
  gFecRx->notify(f, sizeof(f));
}
static void fecAnswerRequest(uint8_t reqPage) {
  if (reqPage == 0x47) {        // page 71 command status (REPORTED layout)
    uint8_t pg[8] = {0x47, gFecLastCmd, gFecSeq, 0x00 /*pass*/, 0xFF, 0xFF, 0xFF, 0xFF};
    fecSend(pg); hexLog("FEC-P71", 0, pg, 8);
  } else if (reqPage == 0x36) { // page 54 capabilities: max resistance 0xFFFF, basic+power+sim
    uint8_t pg[8] = {0x36, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};
    fecSend(pg); hexLog("FEC-P54", 0, pg, 8);
  } else {
    Serial.printf("  -> request for page %u not answered by the tool\n", reqPage);
  }
}
static void handleFec(const WriteEvt& e) {
  hexLog("FEC-TX", e.conn, e.data, e.len);
  if (e.len < 13 || e.data[0] != 0xA4) return;
  uint8_t x = 0, sum = 0;
  for (int i = 0; i < 12; i++) x ^= e.data[i];
  for (int i = 1; i < 12; i++) sum += e.data[i];  // sum variant excludes the sync byte
  Serial.printf("  checksum: xor %s, sum(1..11) %s\n", x == e.data[12] ? "match" : "no",
                sum == e.data[12] ? "match" : "no");
  const uint8_t* p = e.data + 4;
  if (gM.owner == BLE_HS_CONN_HANDLE_NONE) gM.owner = e.conn;
  if (p[0] != 0x46) { gFecLastCmd = p[0]; gFecSeq++; }
  switch (p[0]) {
    case 0x31: {  // page 49 target power, 0.25 W, bytes 6-7
      uint16_t q = p[6] | (p[7] << 8);
      gM.mode = M_ERG; gM.targetW = constrain(q / 4, 0, 1000);
      Serial.printf("  -> page 49 ERG %d W\n", gM.targetW);
      break;
    }
    case 0x33: {  // page 51 grade, 0.01 %, offset -200 %, bytes 5-6; crr byte 7 (5e-5)
      uint16_t g = p[5] | (p[6] << 8);
      gM.mode = M_SIM; gM.gradePct = constrain(g / 100.0f - 200.0f, -15.0f, 20.0f);
      Serial.printf("  -> page 51 grade %.2f %% crr raw %u\n", g / 100.0f - 200.0f, p[7]);
      break;
    }
    case 0x32: Serial.println("  -> page 50 wind"); break;
    case 0x37: Serial.println("  -> page 55 user config (weights, wheel)"); break;
    case 0x46: Serial.printf("  -> page 70 request page %u\n", p[6]); fecAnswerRequest(p[6]); break;
    default: Serial.printf("  -> page %u\n", p[0]);
  }
}
static void sendFecData() {
  int16_t w = (int16_t)gM.powerW;
  static uint16_t accP = 0;
  accP += w;
  gFecEvt++;
  if (gFecEvt & 1) {  // page 25 trainer data
    uint8_t pg[8] = {0x19, gFecEvt, (uint8_t)gM.cadence, (uint8_t)(accP & 0xFF), (uint8_t)(accP >> 8),
                     (uint8_t)(w & 0xFF), (uint8_t)((w >> 8) & 0x0F), 0x30};
    fecSend(pg);
  } else {            // page 16 general FE data: type 25 = trainer
    uint16_t sp = (uint16_t)((10.0f + gM.powerW * 0.1f) / 3.6f * 1000);
    uint8_t pg[8] = {0x10, 0x19, (uint8_t)(millis() / 250), 0x00, (uint8_t)(sp & 0xFF), (uint8_t)(sp >> 8), 0xFF, 0x30};
    fecSend(pg);
  }
}

// ---------------------------------------------------------------- GATT setup
static void setOwnRandomAddress() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  uint8_t a[6];  // NimBLE byte order: a[0] = LSB, a[5] = MSB
  for (int i = 0; i < 6; i++) a[i] = mac[5 - i];
  a[0] ^= (uint8_t)(0x10 + gVariant);  // differs per variant
  a[5] |= 0xC0;                         // static random address
  NimBLEDevice::setOwnAddr(a);
  NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
}

static void setupGatt() {
  const bool ftms = (gVariant == V_FTMS);
  const bool wahoo = (gVariant == V_WAHOO_HW || gVariant == V_WAHOO_KICKR);
  const bool fec = (gVariant == V_FEC_HW || gVariant == V_FEC_TACX);
  const bool vendor = (gVariant == V_WAHOO_KICKR || gVariant == V_FEC_TACX);
  const char* name = (gVariant == V_WAHOO_KICKR) ? "KICKR TEST"
                   : (gVariant == V_FEC_TACX) ? "Tacx Neo TEST" : "homewind-test";
  NimBLEDevice::init(name);
  setOwnRandomAddress();
  NimBLEDevice::setPower(9);
  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new ServerCB());
  for (int i = 0; i < MAXC; i++) slotReset(gInd[0], i);

  if (vendor) {  // identity only for the comparison variants
    NimBLEService* dis = gServer->createService(UUID_DIS);
    dis->createCharacteristic(UUID_MANUF, NIMBLE_PROPERTY::READ)->setValue(gVariant == V_WAHOO_KICKR ? "Wahoo Fitness" : "Tacx");
    dis->createCharacteristic(UUID_MODEL, NIMBLE_PROPERTY::READ)->setValue("trainer-face-test");
    dis->start();
  }

  NimBLEService* hrs = gServer->createService(UUID_HRS);
  gHrm = hrs->createCharacteristic(UUID_HRM, NIMBLE_PROPERTY::NOTIFY);
  gHrm->setCallbacks(new ChrCB(0));
  hrs->start();

  NimBLEService* cps = gServer->createService(UUID_CPS);
  gCpm = cps->createCharacteristic(UUID_CPM, NIMBLE_PROPERTY::NOTIFY);
  gCpm->setCallbacks(new ChrCB(0));
  uint32_t cpFeature = (1u << 3);  // crank revolution data
  cps->createCharacteristic(UUID_CP_FEATURE, NIMBLE_PROPERTY::READ)->setValue(cpFeature);
  uint8_t loc = 0x0D;
  cps->createCharacteristic(UUID_SENSOR_LOC, NIMBLE_PROPERTY::READ)->setValue(loc);
  if (wahoo) {
    gWahooCp = cps->createCharacteristic(UUID_WAHOO_CP, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE, 20);
    gWahooCp->setCallbacks(new ChrCB(W_WAHOO));
    gInd[0].chr = gWahooCp;
  }
  cps->start();

  if (fec) {
    NimBLEService* t = gServer->createService(UUID_FEC_SVC);
    gFecRx = t->createCharacteristic(UUID_FEC_RX, NIMBLE_PROPERTY::NOTIFY);
    gFecRx->setCallbacks(new ChrCB(0));
    NimBLECharacteristic* tx = t->createCharacteristic(UUID_FEC_TX, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR, 20);
    tx->setCallbacks(new ChrCB(W_FEC));
    t->start();
  }

  if (ftms) {  // release-1 layout: no resistance target, no training status, no resistance range
    NimBLEService* f = gServer->createService(UUID_FTMS);
    uint8_t feat[8] = {0};
    uint32_t mf = (1u << 1) | (1u << 14), tf = (1u << 3) | (1u << 13);  // cadence, power | power target, sim
    memcpy(feat, &mf, 4); memcpy(feat + 4, &tf, 4);
    f->createCharacteristic(UUID_FTMS_FEATURE, NIMBLE_PROPERTY::READ)->setValue(feat, 8);
    gIbd = f->createCharacteristic(UUID_IBD, NIMBLE_PROPERTY::NOTIFY);
    gIbd->setCallbacks(new ChrCB(0));
    int16_t pr[3] = {0, 1000, 1};
    f->createCharacteristic(UUID_PWR_RANGE, NIMBLE_PROPERTY::READ)->setValue((uint8_t*)pr, 6);
    gFtmsCp = f->createCharacteristic(UUID_FTMS_CP, NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::INDICATE, 20);
    gFtmsCp->setCallbacks(new ChrCB(W_FTMS));
    gInd[0].chr = gFtmsCp;
    gFtmsStatus = f->createCharacteristic(UUID_FTMS_STATUS, NIMBLE_PROPERTY::NOTIFY);
    gFtmsStatus->setCallbacks(new ChrCB(0));
    f->start();
  }

  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad, sr;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  if (ftms) {
    ad.setCompleteServices16({UUID_FTMS, UUID_HRS, UUID_CPS});
    uint8_t sd[3] = {0x01, 0x20, 0x00};  // machine available, indoor bike
    ad.setServiceData(UUID_FTMS, sd, sizeof(sd));
  } else {
    ad.setCompleteServices16({UUID_HRS, UUID_CPS});
    if (fec) ad.addServiceUUID(UUID_FEC_SVC);
  }
  sr.setName(name);
  adv->setAdvertisementData(ad);
  adv->setScanResponseData(sr);
  adv->start();
  Serial.printf("Advertising as \"%s\" addr %s, variant %s\n", name,
                NimBLEDevice::getAddress().toString().c_str(), variantName(gVariant));
}

// ---------------------------------------------------------------- loopback model
static void stepModel(float dt) {
  float target = (gM.mode == M_ERG) ? gM.targetW
               : (gM.mode == M_SIM) ? constrain(150.0f + 25.0f * gM.gradePct, 40.0f, 600.0f) : 150.0f;
  float maxStep = 60.0f * dt;
  gM.powerW += constrain(target - gM.powerW, -maxStep, maxStep);
  gM.crankAcc += gM.cadence / 60.0f * dt;
  while (gM.crankAcc >= 1.0f) {
    gM.crankAcc -= 1.0f; gM.crankRevs++;
    gM.lastCrankT = (uint16_t)((millis() * 1024UL) / 1000UL);
  }
}

static void sendData() {
  int16_t p = (int16_t)gM.powerW;
  uint8_t cpm[8];
  uint16_t fl = 0x0020;
  memcpy(cpm, &fl, 2); memcpy(cpm + 2, &p, 2);
  memcpy(cpm + 4, &gM.crankRevs, 2); memcpy(cpm + 6, &gM.lastCrankT, 2);
  gCpm->notify(cpm, sizeof(cpm));
  uint8_t hr[2] = {0x00, (uint8_t)(110 + gM.powerW / 10)};  // flags: uint8 bpm
  gHrm->notify(hr, sizeof(hr));
  if (gIbd) {
    uint8_t ibd[8];
    uint16_t f = (1u << 2) | (1u << 6);
    uint16_t speed = (uint16_t)((10.0f + gM.powerW * 0.1f) * 100);
    uint16_t cad = (uint16_t)(gM.cadence * 2);
    memcpy(ibd, &f, 2); memcpy(ibd + 2, &speed, 2); memcpy(ibd + 4, &cad, 2); memcpy(ibd + 6, &p, 2);
    gIbd->notify(ibd, sizeof(ibd));
  }
  if (gFecRx) sendFecData();
}

// ---------------------------------------------------------------- Arduino
void setup() {
  Serial.begin(115200);
  delay(1500);
  Preferences pr;
  pr.begin("tft", true);
  gVariant = (Variant)pr.getUChar("v", V_FTMS);
  pr.end();
  if (gVariant > V_FEC_TACX) gVariant = V_FTMS;
  gQ = xQueueCreate(16, sizeof(WriteEvt));
  Serial.println("\ntrainer_face_test v0.2 — f/p/k/t/n variant, s status, d delay, x drop, l lost-control");
  setupGatt();
}

void loop() {
  WriteEvt e;
  while (xQueueReceive(gQ, &e, 0) == pdTRUE) {
    gWriteCount++;
    if (e.which == W_FTMS) handleFtms(e);
    else if (e.which == W_WAHOO) handleWahoo(e);
    else if (e.which == W_FEC) handleFec(e);
  }
  pumpIndications(gInd[0], gVariant == V_FTMS ? "FTMS-RSP" : "WAHOO-RSP");

  static uint32_t last = millis();
  uint32_t now = millis();
  if (now - last >= 250) {
    stepModel((now - last) / 1000.0f);
    last = now;
    if (gServer->getConnectedCount() > 0) sendData();
  }

  if (!Serial.available()) return;
  char c = Serial.read();
  Variant nv = gVariant;
  switch (c) {
    case 'f': nv = V_FTMS; break;
    case 'p': nv = V_WAHOO_HW; break;
    case 'k': nv = V_WAHOO_KICKR; break;
    case 't': nv = V_FEC_HW; break;
    case 'n': nv = V_FEC_TACX; break;
    case 's':
      Serial.printf("variant %s | conns %u (total %lu) | writes %lu | owner %u | mode %u target %d W grade %.2f %% | power %.0f W | delay %lu ms drop %d\n",
                    variantName(gVariant), gServer->getConnectedCount(), (unsigned long)gConnCount,
                    (unsigned long)gWriteCount, gM.owner, gM.mode, gM.targetW, gM.gradePct, gM.powerW,
                    (unsigned long)gDelayMs, gDrop);
      return;
    case 'd': gDelayMs = gDelayMs == 0 ? 2000 : gDelayMs == 2000 ? 6000 : 0;
              Serial.printf("response delay %lu ms\n", (unsigned long)gDelayMs); return;
    case 'x': gDrop = !gDrop; Serial.printf("drop responses %s\n", gDrop ? "ON" : "off"); return;
    case 'l': if (gFtmsStatus) { uint8_t s[1] = {0xFF}; ftmsStatus(s, 1); gM.owner = BLE_HS_CONN_HANDLE_NONE; }
              return;
    default: return;
  }
  if (nv != gVariant) {
    Preferences w;
    w.begin("tft", false); w.putUChar("v", nv); w.end();
    Serial.printf("Switching to %s, rebooting...\n", variantName(nv));
    delay(200);
    ESP.restart();
  }
}
