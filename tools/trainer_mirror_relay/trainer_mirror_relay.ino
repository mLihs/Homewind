// trainer_mirror_relay — gate G5 prototype for the Homewind trainer mirror (v0.3).
//
// A spare ESP32-S3 sits between the trainer and Zwift. At setup it reads the trainer's
// complete GATT, stores it, reboots, and then offers the same services under the name
// "homewind-test". Everything is relayed unchanged in both directions; DFU is skipped.
// Question answered: does Zwift accept the mirror as a controllable trainer?
//
// Serial (115200):
//   scan          list BLE devices (5 s)
//   use N         take device N from the last scan: read GATT, store, reboot
//   dump          print the stored GATT table
//   forget        delete stored trainer and table, reboot
//   cps           toggle: relay the trainer's CPS measurement / replace it by 200 W
//   s             status
//
// Prototype rules (not product code):
// - Client calls (connect, discover, subscribe, write) are the blocking NimBLE-Arduino
//   ones, called only from loop(); never from NimBLE callbacks.
// - One ordered FIFO app -> trainer for subscriptions and writes.
// - Write type upstream follows the trainer's properties (Write Request if allowed).
// - Trainer -> app: notifications at once, indications one outstanding per connection.
// - Advertising only while the trainer is connected; trainer lost -> app disconnected.
//
// Board: ESP32-S3 (XIAO). Library: NimBLE-Arduino 2.x. License: MIT (Homewind tools).

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <Preferences.h>
#include <esp_mac.h>
#include "mirror_types.h"

// ---------------------------------------------------------------- table
static MirrorTable gT;
static String gTrainerAddr;
static uint8_t gTrainerAddrType = 0;
// trainer advertising details copied into the mirror's advertising
static uint16_t gAppearance = 0;
static String gSdUuid;           // first service-data UUID (e.g. fc82)
static uint8_t gSd[8];
static uint8_t gSdLen = 0;

// UUID as string that NimBLEUUID(std::string) parses back: "2a63", "6e40fec1-...".
static std::string normUuid(const NimBLEUUID& u) {
  std::string s = u.toString();
  if (s.rfind("0x", 0) == 0) s = s.substr(2);
  for (auto& c : s) c = tolower(c);
  return s;
}

static bool isSkippedService(const NimBLEUUID& u) {
  static const NimBLEUUID skip[] = {
      NimBLEUUID((uint16_t)0x1800), NimBLEUUID((uint16_t)0x1801),   // GAP, GATT (stack provides)
      NimBLEUUID((uint16_t)0xFE59),                                 // Nordic Secure DFU
      NimBLEUUID("8ec90001-f315-4f60-9fb8-838830daea50"),           // Nordic Secure DFU (128-bit)
      NimBLEUUID("00001530-1212-efde-1523-785feabcd123"),           // Nordic legacy DFU
  };
  for (auto& k : skip) if (u == k) return true;
  return false;
}

static bool saveTable() {
  // format: magic, nsvc, nchr, then svc uuids (len+str), then chars
  static uint8_t buf[TABLE_BYTES];
  size_t n = 0;
  auto put = [&](const void* p, size_t l) { if (n + l > sizeof(buf)) return false; memcpy(buf + n, p, l); n += l; return true; };
  uint8_t hdr[3] = {0x4D, gT.nsvc, gT.nchr};
  put(hdr, 3);
  for (int i = 0; i < gT.nsvc; i++) { uint8_t l = strlen(gT.svc[i]); put(&l, 1); put(gT.svc[i], l); }
  for (int i = 0; i < gT.nchr; i++) {
    MChr& c = gT.chr[i];
    uint8_t l = strlen(c.uuid);
    put(&c.svc, 1); put(&l, 1); put(c.uuid, l); put(&c.props, 1); put(&c.vlen, 1);
    if (!put(c.val, c.vlen)) return false;
  }
  Preferences p;
  p.begin("mirror", false);
  p.putBytes("tbl", buf, n);
  p.putString("addr", gTrainerAddr);
  p.putUChar("atype", gTrainerAddrType);
  p.putUShort("appear", gAppearance);
  p.putString("sdu", gSdUuid);
  p.putBytes("sd", gSd, gSdLen);
  p.end();
  Serial.printf("table stored: %u services, %u characteristics, %u bytes\n", gT.nsvc, gT.nchr, (unsigned)n);
  return true;
}

static bool loadTable() {
  Preferences p;
  p.begin("mirror", true);
  size_t n = p.getBytesLength("tbl");
  gTrainerAddr = p.getString("addr", "");
  gTrainerAddrType = p.getUChar("atype", 0);
  gAppearance = p.getUShort("appear", 0);
  gSdUuid = p.getString("sdu", "");
  gSdLen = p.getBytes("sd", gSd, sizeof(gSd));
  if (n == 0 || n > TABLE_BYTES || gTrainerAddr.isEmpty()) { p.end(); return false; }
  static uint8_t buf[TABLE_BYTES];
  p.getBytes("tbl", buf, n);
  p.end();
  size_t o = 0;
  auto get = [&](void* d, size_t l) { if (o + l > n) return false; memcpy(d, buf + o, l); o += l; return true; };
  uint8_t hdr[3];
  if (!get(hdr, 3) || hdr[0] != 0x4D || hdr[1] > MAX_SVC || hdr[2] > MAX_CHR) return false;
  gT.nsvc = hdr[1]; gT.nchr = hdr[2];
  for (int i = 0; i < gT.nsvc; i++) { uint8_t l; get(&l, 1); if (l >= UUID_STR) return false; get(gT.svc[i], l); gT.svc[i][l] = 0; }
  for (int i = 0; i < gT.nchr; i++) {
    MChr& c = gT.chr[i];
    uint8_t l;
    get(&c.svc, 1); get(&l, 1); if (l >= UUID_STR) return false;
    get(c.uuid, l); c.uuid[l] = 0;
    get(&c.props, 1); get(&c.vlen, 1); if (c.vlen > VAL_MAX) return false;
    get(c.val, c.vlen);
  }
  return true;
}

// ---------------------------------------------------------------- runtime state
static NimBLEServer* gServer = nullptr;
static NimBLECharacteristic* gLocal[MAX_CHR] = {nullptr};
static NimBLERemoteCharacteristic* gRemote[MAX_CHR] = {nullptr};
static NimBLEClient* gClient = nullptr;
static uint16_t gApp = BLE_HS_CONN_HANDLE_NONE;
static uint16_t gAppSub[MAX_CHR] = {0};      // app's CCCD value per characteristic
static bool gUpSub[MAX_CHR] = {false};       // upstream subscribed
static bool gIndPending = false;
static uint32_t gIndSentAt = 0;
static bool gReplaceCps = false;
static int gCpsMeasIdx = -1;
static uint32_t gStatUp = 0, gStatDown = 0, gStatDrop = 0;

static QueueHandle_t gDownQ;  // app -> trainer (writes, subscriptions), FIFO
static QueueHandle_t gUpQ;    // trainer -> app (notifications, indications)
static QueueHandle_t gIndQ;   // pending indications to the app (FIFO, max 8)

static void hexLog(const char* tag, int idx, const uint8_t* d, size_t n, uint32_t extra = 0) {
  const char* u = (idx >= 0 && idx < gT.nchr) ? gT.chr[idx].uuid : "-";
  Serial.printf("[%9lu] %-8s %-36s len=%-3u", (unsigned long)millis(), tag, u, (unsigned)n);
  if (extra) Serial.printf(" +%lums", (unsigned long)extra);
  Serial.print(" :");
  for (size_t i = 0; i < n; i++) Serial.printf(" %02X", d[i]);
  Serial.println();
}

// ---------------------------------------------------------------- server callbacks (host task)
class SrvCB : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer* s, NimBLEConnInfo& ci) override {
    gApp = ci.getConnHandle();
    Serial.printf("[%9lu] APP CONNECT conn=%u %s mtu=%u\n", (unsigned long)millis(), gApp,
                  ci.getAddress().toString().c_str(), ci.getMTU());
    // advertising stops on connect; it is not restarted (one app only)
  }
  void onDisconnect(NimBLEServer* s, NimBLEConnInfo& ci, int reason) override {
    Serial.printf("[%9lu] APP DISCONNECT conn=%u reason=0x%02X\n", (unsigned long)millis(), ci.getConnHandle(), reason);
    if (ci.getConnHandle() == gApp) {
      gApp = BLE_HS_CONN_HANDLE_NONE;
      for (int i = 0; i < gT.nchr; i++) {
        if (gAppSub[i]) { DownEvt e{}; e.kind = DOWN_SUB; e.idx = i; e.sub = 0; xQueueSend(gDownQ, &e, 0); }
        gAppSub[i] = 0;
      }
      gIndPending = false;
      xQueueReset(gIndQ);
    }
  }
  void onMTUChange(uint16_t mtu, NimBLEConnInfo& ci) override {
    Serial.printf("[%9lu] APP MTU %u\n", (unsigned long)millis(), mtu);
  }
};

class ChrCB : public NimBLECharacteristicCallbacks {
 public:
  explicit ChrCB(uint8_t idx) : idx_(idx) {}
  void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& ci) override {
    NimBLEAttValue v = c->getValue();
    DownEvt e{};
    e.kind = DOWN_WRITE; e.idx = idx_; e.t = millis();
    e.len = (uint8_t)min((size_t)VAL_MAX, (size_t)v.length());
    memcpy(e.data, v.data(), e.len);
    if (xQueueSend(gDownQ, &e, 0) != pdTRUE) { gStatDrop++; Serial.println("  !! down queue full"); }
  }
  void onSubscribe(NimBLECharacteristic* c, NimBLEConnInfo& ci, uint16_t sub) override {
    gAppSub[idx_] = sub;
    DownEvt e{};
    e.kind = DOWN_SUB; e.idx = idx_; e.sub = sub; e.t = millis();
    xQueueSend(gDownQ, &e, 0);
  }
  void onStatus(NimBLECharacteristic* c, NimBLEConnInfo& ci, int code) override {
    if (gIndPending) { gIndPending = false; }
  }
 private:
  uint8_t idx_;
};

// ---------------------------------------------------------------- upstream notify (host task)
static void onUpstream(int idx, uint8_t* data, size_t len, bool isNotify) {
  UpEvt e{};
  e.idx = idx; e.notify = isNotify; e.t = millis();
  e.len = (uint8_t)min((size_t)VAL_MAX, len);
  memcpy(e.data, data, e.len);
  if (xQueueSend(gUpQ, &e, 0) != pdTRUE) gStatDrop++;
}

// ---------------------------------------------------------------- setup flow: scan / use
static NimBLEScanResults gScan;
static void doScan() {
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setActiveScan(true);
  Serial.println("scanning 5 s ...");
  gScan = s->getResults(5000, false);
  for (int i = 0; i < gScan.getCount(); i++) {
    const NimBLEAdvertisedDevice* d = gScan.getDevice(i);
    Serial.printf("%2d  %s  rssi %d  %s  [", i, d->getAddress().toString().c_str(), d->getRSSI(), d->getName().c_str());
    for (int k = 0; k < d->getServiceUUIDCount(); k++) Serial.printf("%s ", d->getServiceUUID(k).toString().c_str());
    Serial.print("]  raw:");
    const std::vector<uint8_t>& raw = d->getPayload();
    for (uint8_t b : raw) Serial.printf(" %02X", b);
    Serial.println();
  }
}

static void doUse(int n) {
  if (n < 0 || n >= gScan.getCount()) { Serial.println("no such device"); return; }
  const NimBLEAdvertisedDevice* d = gScan.getDevice(n);
  NimBLEClient* c = NimBLEDevice::createClient();
  Serial.printf("connecting to %s ...\n", d->getAddress().toString().c_str());
  if (!c->connect(d)) { Serial.println("connect failed"); NimBLEDevice::deleteClient(c); return; }
  Serial.printf("connected, mtu %u, discovering ...\n", c->getMTU());
  bool disc = c->discoverAttributes();
  Serial.printf("discoverAttributes %s, %u services\n", disc ? "ok" : "FAILED", (unsigned)c->getServices(false).size());
  gT.nsvc = 0; gT.nchr = 0;
  for (NimBLERemoteService* svc : c->getServices(false)) {
    bool skip = isSkippedService(svc->getUUID());
    Serial.printf("service %s%s\n", svc->getUUID().toString().c_str(), skip ? "  (skipped)" : "");
    if (skip || gT.nsvc >= MAX_SVC) continue;
    uint8_t si = gT.nsvc++;
    strlcpy(gT.svc[si], normUuid(svc->getUUID()).c_str(), UUID_STR);
    for (NimBLERemoteCharacteristic* ch : svc->getCharacteristics(false)) {
      if (gT.nchr >= MAX_CHR) { Serial.println("  !! MAX_CHR reached"); break; }
      MChr& m = gT.chr[gT.nchr++];
      m.svc = si;
      strlcpy(m.uuid, normUuid(ch->getUUID()).c_str(), UUID_STR);
      m.props = (ch->canRead() ? P_READ : 0) | (ch->canWrite() ? P_WRITE : 0) | (ch->canWriteNoResponse() ? P_WNR : 0) |
                (ch->canNotify() ? P_NOTIFY : 0) | (ch->canIndicate() ? P_INDICATE : 0);
      m.vlen = 0;
      if (ch->canRead()) {
        NimBLEAttValue v = ch->readValue();
        m.vlen = (uint8_t)min((size_t)VAL_MAX, (size_t)v.length());
        memcpy(m.val, v.data(), m.vlen);
      }
      Serial.printf("  chr %s props %c%c%c%c%c value(%u)", m.uuid, m.props & P_READ ? 'R' : '-',
                    m.props & P_WRITE ? 'W' : '-', m.props & P_WNR ? 'w' : '-', m.props & P_NOTIFY ? 'N' : '-',
                    m.props & P_INDICATE ? 'I' : '-', m.vlen);
      for (int k = 0; k < m.vlen; k++) Serial.printf(" %02X", m.val[k]);
      Serial.println();
    }
  }
  gAppearance = d->haveAppearance() ? d->getAppearance() : 0;
  gSdLen = 0; gSdUuid = "";
  if (d->getServiceDataCount() > 0) {
    std::string sd = d->getServiceData(0);
    gSdUuid = normUuid(d->getServiceDataUUID(0)).c_str();
    gSdLen = (uint8_t)min(sd.size(), sizeof(gSd));
    memcpy(gSd, sd.data(), gSdLen);
  }
  Serial.printf("advertising copied: appearance 0x%04X, service data %s (%u B)\n", gAppearance,
                gSdUuid.length() ? gSdUuid.c_str() : "-", gSdLen);
  gTrainerAddr = d->getAddress().toString().c_str();
  gTrainerAddrType = d->getAddress().getType();
  c->disconnect();
  NimBLEDevice::deleteClient(c);
  if (saveTable()) { Serial.println("rebooting into mirror mode ..."); delay(300); ESP.restart(); }
}

// ---------------------------------------------------------------- mirror build
static uint32_t toNimbleProps(uint8_t p) {
  uint32_t r = 0;
  if (p & P_READ) r |= NIMBLE_PROPERTY::READ;
  if (p & P_WRITE) r |= NIMBLE_PROPERTY::WRITE;
  if (p & P_WNR) r |= NIMBLE_PROPERTY::WRITE_NR;
  if (p & P_NOTIFY) r |= NIMBLE_PROPERTY::NOTIFY;
  if (p & P_INDICATE) r |= NIMBLE_PROPERTY::INDICATE;
  return r;
}

static NimBLECharacteristic* gHrm = nullptr;

static void buildMirror() {
  gServer = NimBLEDevice::createServer();
  gServer->setCallbacks(new SrvCB());
  NimBLEService* svcs[MAX_SVC] = {nullptr};
  for (int i = 0; i < gT.nsvc; i++) svcs[i] = gServer->createService(NimBLEUUID(gT.svc[i]));
  for (int i = 0; i < gT.nchr; i++) {
    MChr& m = gT.chr[i];
    gLocal[i] = svcs[m.svc]->createCharacteristic(NimBLEUUID(m.uuid), toNimbleProps(m.props), VAL_MAX);
    gLocal[i]->setCallbacks(new ChrCB(i));
    if (m.vlen) gLocal[i]->setValue(m.val, m.vlen);
    if (!strcmp(m.uuid, "2a63")) gCpsMeasIdx = i;
  }
  for (int i = 0; i < gT.nsvc; i++) svcs[i]->start();
  // HR service (simulated) so Zwift sees "one device with HR"; the trainer has none
  NimBLEService* hrs = gServer->createService(NimBLEUUID((uint16_t)0x180D));
  gHrm = hrs->createCharacteristic(NimBLEUUID((uint16_t)0x2A37), NIMBLE_PROPERTY::NOTIFY);
  hrs->start();
  // Start the GATT server now: NimBLE resets GATT on start(), which is only allowed
  // while no link exists (the trainer link comes later).
  bool ok = gServer->start();
  Serial.printf("mirror built: %u services, %u characteristics (+HR), server start %s\n", gT.nsvc, gT.nchr, ok ? "ok" : "FAILED");
}

static void startAdvertising() {
  // Like the trainer: advertising = flags + service data + appearance + 128-bit control
  // service; scan response = 16-bit service list (+ HR) + name.
  NimBLEAdvertising* adv = NimBLEDevice::getAdvertising();
  NimBLEAdvertisementData ad, sr;
  ad.setFlags(BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP);
  if (gSdLen) ad.setServiceData(NimBLEUUID(std::string(gSdUuid.c_str())), gSd, gSdLen);
  if (gAppearance) ad.setAppearance(gAppearance);
  std::vector<NimBLEUUID> u16;
  bool have128 = false;
  for (int i = 0; i < gT.nsvc; i++) {
    NimBLEUUID u(gT.svc[i]);
    if (u.bitSize() == 16) {
      if (u == NimBLEUUID((uint16_t)0x180F)) continue;  // battery: not advertised
      u16.push_back(u);
    } else if (!have128 && String(gT.svc[i]).startsWith("6e40fec1")) { ad.addServiceUUID(u); have128 = true; }
  }
  u16.push_back(NimBLEUUID((uint16_t)0x180D));
  sr.setCompleteServices16(u16);
  sr.setName("homewind-test");
  bool okA = adv->setAdvertisementData(ad), okS = adv->setScanResponseData(sr);
  adv->start();
  Serial.printf("[%9lu] advertising as homewind-test (%u x 16-bit%s, appearance 0x%04X, sd %s) %s\n",
                (unsigned long)millis(), (unsigned)u16.size(), have128 ? " + FE-C 128-bit" : "", gAppearance,
                gSdUuid.length() ? gSdUuid.c_str() : "-", (okA && okS) ? "" : "!! payload too long");
}

// ---------------------------------------------------------------- trainer link
static bool connectTrainer() {
  if (!gClient) gClient = NimBLEDevice::createClient();
  NimBLEAddress a(std::string(gTrainerAddr.c_str()), gTrainerAddrType);
  if (!gClient->connect(a)) return false;
  Serial.printf("[%9lu] TRAINER CONNECT mtu=%u\n", (unsigned long)millis(), gClient->getMTU());
  // map table entries to remote characteristics by (service uuid, char uuid, occurrence)
  for (int i = 0; i < gT.nchr; i++) gRemote[i] = nullptr;
  for (int i = 0; i < gT.nchr; i++) {
    NimBLERemoteService* s = gClient->getService(NimBLEUUID(gT.svc[gT.chr[i].svc]));
    if (!s) continue;
    int occ = 0;
    for (int k = 0; k < i; k++) if (gT.chr[k].svc == gT.chr[i].svc && !strcmp(gT.chr[k].uuid, gT.chr[i].uuid)) occ++;
    int seen = 0;
    for (NimBLERemoteCharacteristic* ch : s->getCharacteristics(true)) {
      if (ch->getUUID() == NimBLEUUID(gT.chr[i].uuid)) { if (seen++ == occ) { gRemote[i] = ch; break; } }
    }
    if (!gRemote[i]) Serial.printf("  !! not found upstream: %s\n", gT.chr[i].uuid);
  }
  for (int i = 0; i < gT.nchr; i++) gUpSub[i] = false;
  return true;
}

static void setUpstreamSub(int i, uint16_t sub) {
  NimBLERemoteCharacteristic* r = gRemote[i];
  if (!r) return;
  if (sub == 0) {
    if (gUpSub[i]) { r->unsubscribe(true); gUpSub[i] = false; Serial.printf("  upstream unsubscribe %s\n", gT.chr[i].uuid); }
    return;
  }
  bool notif = (sub & 0x01) != 0;
  int idx = i;
  bool ok = r->subscribe(notif, [idx](NimBLERemoteCharacteristic*, uint8_t* d, size_t l, bool n) { onUpstream(idx, d, l, n); }, true);
  gUpSub[i] = ok;
  Serial.printf("  upstream %s %s %s\n", notif ? "notify" : "indicate", gT.chr[i].uuid, ok ? "ok" : "FAILED");
}

// ---------------------------------------------------------------- loop work
static void processDown() {
  DownEvt e;
  while (xQueueReceive(gDownQ, &e, 0) == pdTRUE) {
    if (!gClient || !gClient->isConnected()) { hexLog("DROP-NOTR", e.idx, e.data, e.len); continue; }
    if (e.kind == DOWN_SUB) { setUpstreamSub(e.idx, e.sub); continue; }
    NimBLERemoteCharacteristic* r = gRemote[e.idx];
    if (!r) { hexLog("DROP-MAP", e.idx, e.data, e.len); continue; }
    bool withResp = (gT.chr[e.idx].props & P_WRITE) != 0;  // Write Request if the trainer allows it
    uint32_t t0 = millis();
    bool ok = r->writeValue(e.data, e.len, withResp);
    hexLog(ok ? (withResp ? "APP>TR" : "APP>TRc") : "WR-FAIL", e.idx, e.data, e.len, t0 - e.t);
    gStatDown++;
  }
}

static void processUp() {
  // pending indications first
  if (gIndPending && millis() - gIndSentAt > 30000) { Serial.println("  !! indication timeout 30 s"); gIndPending = false; }
  UpEvt e;
  if (!gIndPending && xQueueReceive(gIndQ, &e, 0) == pdTRUE && gApp != BLE_HS_CONN_HANDLE_NONE) {
    bool ok = gLocal[e.idx]->indicate(e.data, e.len, gApp);
    hexLog(ok ? "TR>APPi" : "IND-FAIL", e.idx, e.data, e.len, millis() - e.t);
    if (ok) { gIndPending = true; gIndSentAt = millis(); }
  }
  while (xQueueReceive(gUpQ, &e, 0) == pdTRUE) {
    gStatUp++;
    if (e.idx == gCpsMeasIdx && gReplaceCps && e.len >= 4) { e.data[2] = 200; e.data[3] = 0; }  // test: 200 W
    if (gApp == BLE_HS_CONN_HANDLE_NONE || !(gAppSub[e.idx])) continue;
    if (e.notify) {
      bool ok = gLocal[e.idx]->notify(e.data, e.len, gApp);
      hexLog(ok ? "TR>APP" : "NTF-FAIL", e.idx, e.data, e.len, millis() - e.t);
    } else {
      if (xQueueSend(gIndQ, &e, 0) != pdTRUE) {
        Serial.println("  !! indication queue full -> disconnect app");
        gServer->disconnect(gApp);
      }
    }
  }
}

static bool gMirror = false;

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\ntrainer_mirror_relay v0.3 — scan | use N | dump | forget | cps | s");
  gDownQ = xQueueCreate(32, sizeof(DownEvt));
  gUpQ = xQueueCreate(32, sizeof(UpEvt));
  gIndQ = xQueueCreate(8, sizeof(UpEvt));
  NimBLEDevice::init("homewind-test");
  NimBLEDevice::setMTU(247);
  gMirror = loadTable();
  if (gMirror) {
    // own static random address, stable across boots
    uint8_t mac[6], a[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) a[i] = mac[5 - i];
    a[0] ^= 0x5A; a[5] |= 0xC0;
    NimBLEDevice::setOwnAddr(a);
    NimBLEDevice::setOwnAddrType(BLE_OWN_ADDR_RANDOM);
    Serial.printf("trainer %s (type %u); building mirror\n", gTrainerAddr.c_str(), gTrainerAddrType);
    buildMirror();
  } else {
    Serial.println("no trainer stored: type 'scan', then 'use N'");
  }
}

void loop() {
  static uint32_t lastTry = 0, lastHr = 0, lastHeap = 0;
  if (gMirror) {
    bool trainerUp = gClient && gClient->isConnected();
    if (!trainerUp) {
      if (gApp != BLE_HS_CONN_HANDLE_NONE) {
        Serial.println("trainer lost -> disconnect app");
        gServer->disconnect(gApp);
      }
      NimBLEDevice::stopAdvertising();
      if (millis() - lastTry > 3000) {
        lastTry = millis();
        if (connectTrainer()) {
          for (int i = 0; i < gT.nchr; i++) if (gAppSub[i]) setUpstreamSub(i, gAppSub[i]);
          if (gApp == BLE_HS_CONN_HANDLE_NONE) startAdvertising();
        }
      }
    } else if (gApp == BLE_HS_CONN_HANDLE_NONE && !NimBLEDevice::getAdvertising()->isAdvertising()) {
      startAdvertising();
    }
    processDown();
    processUp();
    if (gHrm && gApp != BLE_HS_CONN_HANDLE_NONE && millis() - lastHr > 1000) {
      lastHr = millis();
      uint8_t hr[2] = {0x00, 120};
      gHrm->notify(hr, 2, gApp);
    }
  }
  if (millis() - lastHeap > 10000) {
    lastHeap = millis();
    Serial.printf("[%9lu] heap internal free %u largest %u | up %lu down %lu drop %lu\n", (unsigned long)millis(),
                  heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                  (unsigned long)gStatUp, (unsigned long)gStatDown, (unsigned long)gStatDrop);
  }

  if (!Serial.available()) return;
  String cmd = Serial.readStringUntil('\n');
  cmd.trim();
  if (cmd == "scan") doScan();
  else if (cmd.startsWith("use ")) doUse(cmd.substring(4).toInt());
  else if (cmd == "forget") { Preferences p; p.begin("mirror", false); p.clear(); p.end(); delay(200); ESP.restart(); }
  else if (cmd == "dump") {
    for (int i = 0; i < gT.nsvc; i++) {
      Serial.printf("service %d %s\n", i, gT.svc[i]);
      for (int k = 0; k < gT.nchr; k++) if (gT.chr[k].svc == i) {
        MChr& m = gT.chr[k];
        Serial.printf("  chr %2d %-36s props %02X value(%u)", k, m.uuid, m.props, m.vlen);
        for (int j = 0; j < m.vlen; j++) Serial.printf(" %02X", m.val[j]);
        Serial.println();
      }
    }
  }
  else if (cmd == "cps") { gReplaceCps = !gReplaceCps; Serial.printf("CPS replace (200 W) %s\n", gReplaceCps ? "ON" : "off"); }
  else if (cmd == "s") {
    Serial.printf("mirror %d | trainer %s %s | app %u | services %u chars %u | cps replace %d\n", gMirror,
                  gTrainerAddr.c_str(), (gClient && gClient->isConnected()) ? "connected" : "down", gApp, gT.nsvc,
                  gT.nchr, gReplaceCps);
  }
}
