// dyson_probe — Homewind P0: Dyson local MQTT on ESP32-S3 (heap + protocol go/no-go).
//
// Pattern: tools/trainer_mirror_relay. Not product code.
// AsyncTCP callbacks only enqueue bytes; MQTT/state runs in loop().
//
// Serial 115200 (newline):
//   wifi <ssid> <pass>     join Wi-Fi
//   host <ip> [port]       broker (default 1883); use Mac IP for fake broker
//   serial <sn>            Dyson serial (= MQTT username)
//   product <ptype>        e.g. 438 / 527 / 438K
//   sticker <pw>           sticker password → SHA-512 + Base64 MQTT password
//   cred <pw>              raw MQTT password (88 chars from extractor)
//   client <id>            MQTT client id (default hw-<mac8>-0, ≤23)
//   connect                TCP + MQTT CONNECT
//   disc                   DISCONNECT + close
//   off                    stage 0 / power off
//   stage <1-10>           set fan stage
//   pct <0-100>            map percent → stage (concept §5.4)
//   ping                   PINGREQ
//   heap                   free / largest internal
//   s                      status
//   help
//
// Board: ESP32-S3 (XIAO or Touch). Needs AsyncTCP (already in Homewind deps).

#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <mbedtls/sha512.h>
#include <mbedtls/base64.h>
#include <esp_mac.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mqtt_min.h"
#include "dyson_map.h"

// ---------------- config (set via serial) ----------------
static char gSsid[33] = "";
static char gWifiPass[65] = "";
static char gHost[48] = "192.168.178.20";
static uint16_t gPort = 1883;
static char gSerial[24] = "NK6-EU-P0PROBE01";
static char gProduct[12] = "438";
static char gMqttPass[96] = "";  // up to 88 + NUL
static char gClientId[24] = "";

// ---------------- rx queue (async_tcp task → loop) ----------------
static constexpr size_t kRxQ = 4096;
static uint8_t gRxQ[kRxQ];
static volatile size_t gRxHead = 0;
static volatile size_t gRxTail = 0;
static portMUX_TYPE gRxMux = portMUX_INITIALIZER_UNLOCKED;

static AsyncClient gClient;
static bool gTcpUp = false;
static bool gMqttUp = false;
static uint16_t gPktId = 1;
static uint16_t gAwaitPubAck = 0;
static uint32_t gLastPingMs = 0;
static uint32_t gLastRxMs = 0;
static uint8_t gLastStage = 255;  // 255 = unknown
static uint8_t gParseBuf[512];
static size_t gParseLen = 0;

static void logf(const char* fmt, ...) {
  char line[192];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  Serial.println(line);
}

static void enqueueRx(const uint8_t* data, size_t len) {
  portENTER_CRITICAL(&gRxMux);
  for (size_t i = 0; i < len; i++) {
    size_t next = (gRxHead + 1) % kRxQ;
    if (next == gRxTail) break;  // drop on overflow
    gRxQ[gRxHead] = data[i];
    gRxHead = next;
  }
  portEXIT_CRITICAL(&gRxMux);
}

static size_t dequeueRx(uint8_t* out, size_t cap) {
  size_t n = 0;
  portENTER_CRITICAL(&gRxMux);
  while (n < cap && gRxTail != gRxHead) {
    out[n++] = gRxQ[gRxTail];
    gRxTail = (gRxTail + 1) % kRxQ;
  }
  portEXIT_CRITICAL(&gRxMux);
  return n;
}

static void defaultClientId() {
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  snprintf(gClientId, sizeof(gClientId), "hw-%02x%02x%02x%02x-0", mac[2], mac[3], mac[4],
           mac[5]);
}

static bool stickerToMqttPass(const char* sticker, char* out, size_t outCap) {
  uint8_t digest[64];
  if (mbedtls_sha512((const unsigned char*)sticker, strlen(sticker), digest, 0) != 0) {
    return false;
  }
  size_t olen = 0;
  if (mbedtls_base64_encode((unsigned char*)out, outCap - 1, &olen, digest, sizeof(digest)) !=
      0) {
    return false;
  }
  out[olen] = 0;
  return olen == 88;
}

static bool tcpSend(const uint8_t* data, size_t len) {
  if (!gClient.connected()) return false;
  const size_t n = gClient.add((const char*)data, len);
  gClient.send();
  return n == len;
}

static void buildStateSet(char* out, size_t cap, uint8_t stage) {
  // Minimal JSON; time placeholder (SNTP optional later).
  if (stage == 0) {
    snprintf(out, cap,
             "{\"msg\":\"STATE-SET\",\"time\":\"2026-01-01T00:00:00Z\","
             "\"mode-reason\":\"LAPP\",\"data\":{\"fpwr\":\"OFF\",\"auto\":\"OFF\"}}");
  } else {
    snprintf(out, cap,
             "{\"msg\":\"STATE-SET\",\"time\":\"2026-01-01T00:00:00Z\","
             "\"mode-reason\":\"LAPP\",\"data\":{\"fpwr\":\"ON\",\"auto\":\"OFF\","
             "\"fnsp\":\"%04u\"}}",
             (unsigned)stage);
  }
}

static void cmdStage(uint8_t stage, bool force) {
  if (!gMqttUp) {
    logf("err: not connected");
    return;
  }
  if (!force && !dysonmap::stageNeedsPublish(gLastStage, stage)) {
    logf("skip same stage=%u (would okMask immediately in product)", (unsigned)stage);
    return;
  }
  char topic[64];
  snprintf(topic, sizeof(topic), "%s/%s/command", gProduct, gSerial);
  char body[192];
  buildStateSet(body, sizeof(body), stage);
  uint8_t pkt[320];
  const uint16_t id = gPktId++;
  if (gPktId == 0) gPktId = 1;
  const size_t n =
      mqttmin::buildPublishQos1(pkt, sizeof(pkt), topic, (const uint8_t*)body, strlen(body), id);
  if (!n || !tcpSend(pkt, n)) {
    logf("err: publish failed");
    return;
  }
  gAwaitPubAck = id;
  gLastStage = stage;
  logf("PUBLISH stage=%u id=%u topic=%s", (unsigned)stage, (unsigned)id, topic);
}

static void doDisconnect() {
  if (gClient.connected()) {
    uint8_t pkt[4];
    size_t n = mqttmin::buildDisconnect(pkt, sizeof(pkt));
    if (n) tcpSend(pkt, n);
    delay(20);
    gClient.close();
  }
  gTcpUp = false;
  gMqttUp = false;
  gAwaitPubAck = 0;
  logf("disconnected");
}

static void onData(void*, AsyncClient*, void* data, size_t len) {
  enqueueRx((const uint8_t*)data, len);
}

static void onConnect(void*, AsyncClient*) {
  gTcpUp = true;
  gLastRxMs = millis();
  logf("tcp up → MQTT CONNECT");
  uint8_t pkt[320];
  const size_t n = mqttmin::buildConnect(pkt, sizeof(pkt), gClientId, gSerial, gMqttPass, 30);
  if (!n || !tcpSend(pkt, n)) {
    logf("err: CONNECT build/send failed (pass len=%u)", (unsigned)strlen(gMqttPass));
    gClient.close();
  }
}

static void onDisconnect(void*, AsyncClient*) {
  gTcpUp = false;
  gMqttUp = false;
  logf("tcp down");
}

static void onError(void*, AsyncClient*, int8_t err) {
  logf("tcp err %d", (int)err);
}

static void startConnect() {
  if (WiFi.status() != WL_CONNECTED) {
    logf("err: wifi not connected");
    return;
  }
  if (gMqttPass[0] == 0) {
    logf("err: set sticker <pw> or cred <pw> first");
    return;
  }
  if (gClientId[0] == 0) defaultClientId();
  doDisconnect();
  gParseLen = 0;
  gClient.onData(onData);
  gClient.onConnect(onConnect);
  gClient.onDisconnect(onDisconnect);
  gClient.onError(onError);
  logf("tcp connect %s:%u as %s user=%s", gHost, (unsigned)gPort, gClientId, gSerial);
  if (!gClient.connect(gHost, gPort)) {
    logf("err: connect() rejected");
  }
}

static void printHeap() {
  logf("heap free=%u largest=%u min_ever=%u", (unsigned)ESP.getFreeHeap(),
       (unsigned)ESP.getMaxAllocHeap(), (unsigned)ESP.getMinFreeHeap());
}

static void printStatus() {
  logf("wifi=%s ip=%s tcp=%d mqtt=%d host=%s:%u",
       WiFi.status() == WL_CONNECTED ? "up" : "down",
       WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString().c_str() : "-", (int)gTcpUp,
       (int)gMqttUp, gHost, (unsigned)gPort);
  logf("serial=%s product=%s client=%s pass_len=%u last_stage=%u await_ack=%u", gSerial,
       gProduct, gClientId, (unsigned)strlen(gMqttPass), (unsigned)gLastStage,
       (unsigned)gAwaitPubAck);
  printHeap();
}

static void handleLine(char* line) {
  while (*line == ' ') line++;
  if (*line == 0) return;
  char* cmd = strtok(line, " \t");
  if (!cmd) return;

  if (!strcmp(cmd, "help")) {
    logf("wifi|host|serial|product|sticker|cred|client|connect|disc|off|stage|pct|ping|heap|s");
  } else if (!strcmp(cmd, "wifi")) {
    const char* ssid = strtok(nullptr, " \t");
    const char* pass = strtok(nullptr, "");
    if (!ssid || !pass) {
      logf("usage: wifi <ssid> <pass>");
      return;
    }
    strncpy(gSsid, ssid, sizeof(gSsid) - 1);
    strncpy(gWifiPass, pass, sizeof(gWifiPass) - 1);
    WiFi.mode(WIFI_STA);
    WiFi.begin(gSsid, gWifiPass);
    logf("wifi joining %s …", gSsid);
  } else if (!strcmp(cmd, "host")) {
    const char* ip = strtok(nullptr, " \t");
    const char* port = strtok(nullptr, " \t");
    if (!ip) {
      logf("usage: host <ip> [port]");
      return;
    }
    strncpy(gHost, ip, sizeof(gHost) - 1);
    if (port) gPort = (uint16_t)atoi(port);
  } else if (!strcmp(cmd, "serial")) {
    const char* s = strtok(nullptr, " \t");
    if (!s) return;
    strncpy(gSerial, s, sizeof(gSerial) - 1);
  } else if (!strcmp(cmd, "product")) {
    const char* s = strtok(nullptr, " \t");
    if (!s) return;
    strncpy(gProduct, s, sizeof(gProduct) - 1);
  } else if (!strcmp(cmd, "sticker")) {
    const char* s = strtok(nullptr, "");
    if (!s) return;
    while (*s == ' ') s++;
    if (!stickerToMqttPass(s, gMqttPass, sizeof(gMqttPass))) {
      logf("err: sha512/base64 failed");
      gMqttPass[0] = 0;
    } else {
      logf("mqtt pass ready len=%u", (unsigned)strlen(gMqttPass));
    }
  } else if (!strcmp(cmd, "cred")) {
    const char* s = strtok(nullptr, " \t");
    if (!s) return;
    strncpy(gMqttPass, s, sizeof(gMqttPass) - 1);
    logf("mqtt pass set len=%u", (unsigned)strlen(gMqttPass));
  } else if (!strcmp(cmd, "client")) {
    const char* s = strtok(nullptr, " \t");
    if (!s) return;
    strncpy(gClientId, s, sizeof(gClientId) - 1);
  } else if (!strcmp(cmd, "connect")) {
    startConnect();
  } else if (!strcmp(cmd, "disc")) {
    doDisconnect();
  } else if (!strcmp(cmd, "off")) {
    cmdStage(0, true);
  } else if (!strcmp(cmd, "stage")) {
    const char* s = strtok(nullptr, " \t");
    if (!s) return;
    int v = atoi(s);
    if (v < 1 || v > 10) {
      logf("usage: stage 1..10");
      return;
    }
    cmdStage((uint8_t)v, false);
  } else if (!strcmp(cmd, "pct")) {
    const char* s = strtok(nullptr, " \t");
    if (!s) return;
    int v = atoi(s);
    if (v < 0 || v > 100) {
      logf("usage: pct 0..100");
      return;
    }
    const uint8_t st = dysonmap::pctToStage((uint8_t)v);
    logf("pct %d → stage %u", v, (unsigned)st);
    cmdStage(st, false);
  } else if (!strcmp(cmd, "ping")) {
    uint8_t pkt[4];
    size_t n = mqttmin::buildPingReq(pkt, sizeof(pkt));
    if (n && tcpSend(pkt, n)) {
      gLastPingMs = millis();
      logf("PINGREQ");
    }
  } else if (!strcmp(cmd, "heap")) {
    printHeap();
  } else if (!strcmp(cmd, "s")) {
    printStatus();
  } else {
    logf("unknown '%s' (help)", cmd);
  }
}

static void pumpMqtt() {
  uint8_t tmp[256];
  const size_t n = dequeueRx(tmp, sizeof(tmp));
  if (n) {
    gLastRxMs = millis();
    if (gParseLen + n > sizeof(gParseBuf)) {
      logf("err: parse buffer overflow — reset");
      gParseLen = 0;
    } else {
      memcpy(gParseBuf + gParseLen, tmp, n);
      gParseLen += n;
    }
  }
  while (gParseLen > 0) {
    mqttmin::RxEvent ev;
    const size_t used = mqttmin::poll(gParseBuf, gParseLen, ev);
    if (!used) break;
    memmove(gParseBuf, gParseBuf + used, gParseLen - used);
    gParseLen -= used;
    if (ev.kind == mqttmin::RxKind::ConnAck) {
      if (ev.connackCode == 0) {
        gMqttUp = true;
        gLastStage = 255;
        logf("CONNACK 0 — MQTT ready");
        printHeap();
      } else {
        gMqttUp = false;
        logf("CONNACK %u — AUTH/fail (no retry)", (unsigned)ev.connackCode);
      }
    } else if (ev.kind == mqttmin::RxKind::PubAck) {
      logf("PUBACK id=%u%s", (unsigned)ev.packetId,
           ev.packetId == gAwaitPubAck ? " (match)" : "");
      if (ev.packetId == gAwaitPubAck) gAwaitPubAck = 0;
    } else if (ev.kind == mqttmin::RxKind::PingResp) {
      logf("PINGRESP");
    }
  }

  if (gMqttUp && (millis() - gLastPingMs) > 25000) {
    uint8_t pkt[4];
    size_t pn = mqttmin::buildPingReq(pkt, sizeof(pkt));
    if (pn && tcpSend(pkt, pn)) gLastPingMs = millis();
  }
  if (gMqttUp && gLastRxMs && (millis() - gLastRxMs) > 45000) {
    logf("keepalive timeout — closing");
    doDisconnect();
  }
}

static char gLine[160];
static size_t gLineLen = 0;

void setup() {
  Serial.begin(115200);
  delay(200);
  defaultClientId();
  logf("dyson_probe P0 — help for commands");
  printHeap();
}

void loop() {
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\r') continue;
    if (c == '\n') {
      gLine[gLineLen] = 0;
      handleLine(gLine);
      gLineLen = 0;
    } else if (gLineLen + 1 < sizeof(gLine)) {
      gLine[gLineLen++] = c;
    }
  }
  if (WiFi.status() == WL_CONNECTED && gSsid[0] &&
      (millis() / 1000) % 30 == 0) {
    // quiet
  }
  static wl_status_t last = WL_IDLE_STATUS;
  wl_status_t st = WiFi.status();
  if (st != last) {
    last = st;
    if (st == WL_CONNECTED) logf("wifi up %s", WiFi.localIP().toString().c_str());
    if (st == WL_DISCONNECTED || st == WL_CONNECT_FAILED) logf("wifi status %d", (int)st);
  }
  pumpMqtt();
  delay(1);
}
