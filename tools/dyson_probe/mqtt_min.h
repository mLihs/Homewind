// Minimal MQTT 3.1.1 client helpers for dyson_probe (P0) and host tests.
// CONNECT / PUBLISH QoS1 / PINGREQ / DISCONNECT + parse CONNACK / PUBACK / PINGRESP.
// No heap String; caller supplies buffers. Host-portable (no Arduino.h).

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

namespace mqttmin {

enum class RxKind : uint8_t { None, ConnAck, PubAck, PingResp, Other, Malformed };

struct RxEvent {
  RxKind kind = RxKind::None;
  uint8_t connackCode = 0;  // 0 = accepted
  uint16_t packetId = 0;
};

inline size_t encodeRemainingLength(uint8_t* out, size_t cap, size_t n) {
  size_t used = 0;
  do {
    if (used >= cap) return 0;
    uint8_t digit = n % 128;
    n /= 128;
    if (n > 0) digit |= 0x80;
    out[used++] = digit;
  } while (n > 0);
  return used;
}

inline bool decodeRemainingLength(const uint8_t* buf, size_t len, size_t offset,
                                  size_t& value, size_t& next) {
  size_t multiplier = 1;
  value = 0;
  size_t pos = offset;
  for (;;) {
    if (pos >= len) return false;
    uint8_t encoded = buf[pos++];
    value += (encoded & 127) * multiplier;
    if ((encoded & 128) == 0) break;
    multiplier *= 128;
    if (multiplier > 128UL * 128UL * 128UL) return false;
  }
  next = pos;
  return true;
}

inline size_t putStr(uint8_t* out, size_t cap, size_t at, const char* s) {
  const size_t n = strlen(s);
  if (at + 2 + n > cap) return 0;
  out[at] = (uint8_t)((n >> 8) & 0xff);
  out[at + 1] = (uint8_t)(n & 0xff);
  memcpy(out + at + 2, s, n);
  return 2 + n;
}

// Build CONNECT (MQTT 3.1.1, username+password). Returns bytes written or 0.
inline size_t buildConnect(uint8_t* out, size_t cap, const char* clientId,
                           const char* username, const char* password,
                           uint16_t keepaliveSec) {
  // Variable header + payload first into a side area at end? Use two-pass length.
  uint8_t vh[256];
  size_t v = 0;
  // Protocol Name "MQTT"
  v += putStr(vh, sizeof(vh), v, "MQTT");
  if (v == 0 || v + 4 > sizeof(vh)) return 0;
  vh[v++] = 4;     // level 3.1.1
  vh[v++] = 0xC0;  // username + password
  vh[v++] = (uint8_t)((keepaliveSec >> 8) & 0xff);
  vh[v++] = (uint8_t)(keepaliveSec & 0xff);
  size_t a = putStr(vh, sizeof(vh), v, clientId);
  if (!a) return 0;
  v += a;
  a = putStr(vh, sizeof(vh), v, username);
  if (!a) return 0;
  v += a;
  a = putStr(vh, sizeof(vh), v, password);
  if (!a) return 0;
  v += a;

  uint8_t rl[4];
  const size_t rln = encodeRemainingLength(rl, sizeof(rl), v);
  if (!rln || 1 + rln + v > cap) return 0;
  out[0] = 0x10;  // CONNECT
  memcpy(out + 1, rl, rln);
  memcpy(out + 1 + rln, vh, v);
  return 1 + rln + v;
}

inline size_t buildPublishQos1(uint8_t* out, size_t cap, const char* topic,
                               const uint8_t* payload, size_t payloadLen,
                               uint16_t packetId) {
  const size_t tlen = strlen(topic);
  const size_t body = 2 + tlen + 2 + payloadLen;  // topic + id + payload
  uint8_t rl[4];
  const size_t rln = encodeRemainingLength(rl, sizeof(rl), body);
  if (!rln || 1 + rln + body > cap) return 0;
  size_t o = 0;
  out[o++] = 0x32;  // PUBLISH qos1
  memcpy(out + o, rl, rln);
  o += rln;
  out[o++] = (uint8_t)((tlen >> 8) & 0xff);
  out[o++] = (uint8_t)(tlen & 0xff);
  memcpy(out + o, topic, tlen);
  o += tlen;
  out[o++] = (uint8_t)((packetId >> 8) & 0xff);
  out[o++] = (uint8_t)(packetId & 0xff);
  if (payloadLen) {
    memcpy(out + o, payload, payloadLen);
    o += payloadLen;
  }
  return o;
}

inline size_t buildPingReq(uint8_t* out, size_t cap) {
  if (cap < 2) return 0;
  out[0] = 0xC0;
  out[1] = 0x00;
  return 2;
}

inline size_t buildDisconnect(uint8_t* out, size_t cap) {
  if (cap < 2) return 0;
  out[0] = 0xE0;
  out[1] = 0x00;
  return 2;
}

// Feed bytes; when a full packet is present, fill ev and return bytes consumed.
inline size_t poll(const uint8_t* buf, size_t len, RxEvent& ev) {
  ev = RxEvent{};
  if (len < 2) return 0;
  size_t rem = 0, pos = 0;
  if (!decodeRemainingLength(buf, len, 1, rem, pos)) return 0;
  const size_t total = pos + rem;
  if (len < total) return 0;
  const uint8_t type = buf[0] >> 4;
  if (type == 2 && rem >= 2) {  // CONNACK
    ev.kind = RxKind::ConnAck;
    ev.connackCode = buf[pos + 1];
  } else if (type == 4 && rem >= 2) {  // PUBACK
    ev.kind = RxKind::PubAck;
    ev.packetId = (uint16_t)((buf[pos] << 8) | buf[pos + 1]);
  } else if (type == 13) {
    ev.kind = RxKind::PingResp;
  } else {
    ev.kind = RxKind::Other;
  }
  return total;
}

}  // namespace mqttmin
