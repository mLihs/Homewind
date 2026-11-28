// Host tests for tools/dyson_probe MQTT helpers + stage mapping (P0).
// On macOS with a broken default SDK symlink, pin an older sysroot, e.g.:
//   SDK=/Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
//   xcrun clang++ -std=c++17 -Wall -Wextra -Werror -isysroot "$SDK" \
//     -Itools/dyson_probe tests/dyson_mqtt_min_test.cpp \
//     -o /tmp/dyson_mqtt_min_test && /tmp/dyson_mqtt_min_test

#include "dyson_map.h"
#include "mqtt_min.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

static void testRemainingLengthRoundTrip() {
  uint8_t enc[4];
  for (size_t n : {0u, 1u, 127u, 128u, 200u, 16383u}) {
    const size_t el = mqttmin::encodeRemainingLength(enc, sizeof(enc), n);
    CHECK(el > 0);
    size_t value = 0, next = 0;
    // Pretend packet: type byte + remaining length
    uint8_t buf[8];
    buf[0] = 0x10;
    memcpy(buf + 1, enc, el);
    CHECK(mqttmin::decodeRemainingLength(buf, 1 + el, 1, value, next));
    CHECK(value == n);
    CHECK(next == 1 + el);
  }
  // Buffer too small
  CHECK(mqttmin::encodeRemainingLength(enc, 0, 128) == 0);
}

static void testPctToStage() {
  CHECK(dysonmap::pctToStage(0) == 0);
  CHECK(dysonmap::pctToStage(1) == 1);
  CHECK(dysonmap::pctToStage(10) == 1);
  CHECK(dysonmap::pctToStage(11) == 2);
  CHECK(dysonmap::pctToStage(95) == 10);
  CHECK(dysonmap::pctToStage(100) == 10);
  CHECK(dysonmap::pctToStage(50) == 5);
  CHECK(dysonmap::pctToStage(9) == 1);
}

static void testStageNeedsPublish() {
  CHECK(dysonmap::stageNeedsPublish(255, 5) == true);   // after reconnect
  CHECK(dysonmap::stageNeedsPublish(5, 5) == false);    // skip wire; product still okMask
  CHECK(dysonmap::stageNeedsPublish(5, 6) == true);
  CHECK(dysonmap::stageNeedsPublish(0, 0) == false);
  CHECK(dysonmap::stageNeedsPublish(0, 1) == true);
}

static void testConnectWith88CharPassword() {
  // 88-char Base64-looking MQTT password (length matters for remaining length > 127)
  char pass[89];
  for (int i = 0; i < 88; i++) pass[i] = (i % 2) ? 'A' : 'a';
  pass[88] = 0;

  uint8_t pkt[320];
  const size_t n = mqttmin::buildConnect(pkt, sizeof(pkt), "hw-aabbccdd-0",
                                         "NK6-EU-P0PROBE01", pass, 30);
  CHECK(n > 0);
  CHECK(pkt[0] == 0x10);
  size_t rem = 0, pos = 0;
  CHECK(mqttmin::decodeRemainingLength(pkt, n, 1, rem, pos));
  CHECK(rem > 127);  // forces multi-byte remaining length
  CHECK(pos == 3);   // 1 type + 2 length bytes for values 128..16383
  CHECK(1 + (pos - 1) + rem == n);
  CHECK(pkt[pos + 6] == 4);     // protocol level 3.1.1
  CHECK(pkt[pos + 7] == 0xC2);  // user + password + clean session (TP09: CONNACK 3 without)

  // Client id must fit MQTT 3.1 limit habitually used on Dyson (≤23)
  CHECK(std::strlen("hw-aabbccdd-0") <= 23);

  // Tiny buffer must fail cleanly
  uint8_t tiny[16];
  CHECK(mqttmin::buildConnect(tiny, sizeof(tiny), "hw-aabbccdd-0", "NK6-EU-P0PROBE01",
                              pass, 30) == 0);
}

static void testPublishQos1AndPoll() {
  const char* topic = "438/NK6-EU-P0PROBE01/command";
  const char* body =
      "{\"msg\":\"STATE-SET\",\"data\":{\"fpwr\":\"ON\",\"auto\":\"OFF\",\"fnsp\":\"0005\"}}";
  uint8_t pkt[320];
  const size_t n = mqttmin::buildPublishQos1(
      pkt, sizeof(pkt), topic, reinterpret_cast<const uint8_t*>(body), std::strlen(body), 7);
  CHECK(n > 0);
  CHECK(pkt[0] == 0x32);

  // Fake PUBACK
  uint8_t ack[4] = {0x40, 0x02, 0x00, 0x07};
  mqttmin::RxEvent ev;
  const size_t used = mqttmin::poll(ack, sizeof(ack), ev);
  CHECK(used == 4);
  CHECK(ev.kind == mqttmin::RxKind::PubAck);
  CHECK(ev.packetId == 7);

  // CONNACK accepted
  uint8_t ca[4] = {0x20, 0x02, 0x00, 0x00};
  CHECK(mqttmin::poll(ca, 4, ev) == 4);
  CHECK(ev.kind == mqttmin::RxKind::ConnAck);
  CHECK(ev.connackCode == 0);

  // CONNACK bad user
  uint8_t cf[4] = {0x20, 0x02, 0x00, 0x04};
  CHECK(mqttmin::poll(cf, 4, ev) == 4);
  CHECK(ev.connackCode == 4);

  uint8_t ping[2] = {0xD0, 0x00};
  CHECK(mqttmin::poll(ping, 2, ev) == 2);
  CHECK(ev.kind == mqttmin::RxKind::PingResp);

  uint8_t pr[2], di[2];
  CHECK(mqttmin::buildPingReq(pr, 2) == 2);
  CHECK(pr[0] == 0xC0);
  CHECK(mqttmin::buildDisconnect(di, 2) == 2);
  CHECK(di[0] == 0xE0);
}

static void testPollIncomplete() {
  uint8_t partial[2] = {0x20, 0x02};  // missing payload
  mqttmin::RxEvent ev;
  CHECK(mqttmin::poll(partial, 2, ev) == 0);
  CHECK(ev.kind == mqttmin::RxKind::None);
}

int main() {
  testRemainingLengthRoundTrip();
  testPctToStage();
  testStageNeedsPublish();
  testConnectWith88CharPassword();
  testPublishQos1AndPoll();
  testPollIncomplete();
  if (g_failures) {
    std::printf("%d failure(s)\n", g_failures);
    return 1;
  }
  std::printf("dyson_mqtt_min_test: ok\n");
  return 0;
}
