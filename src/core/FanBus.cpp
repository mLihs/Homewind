#include "../app/Config.h"

#if HW_ENABLE_FANS

#include "FanBus.h"
#include <WiFiUdp.h>
#include <string.h>

namespace {

FanDriverType gType = FanDriverType::None;
WiFiUDP gUdp;
bool gBegun = false;

bool isXiaomi() {
  return gType == FanDriverType::Xiaomi;
}

}  // namespace

namespace FanBus {

bool select(FanDriverType type) {
  if (gBegun) return false;
  if (type != FanDriverType::Xiaomi && type != FanDriverType::None) {
    // P1: Dyson not wired yet
    return false;
  }
  gType = type;
  return true;
}

FanDriverType selected() {
  return gType;
}

bool begin() {
  if (gType == FanDriverType::None) {
    gType = FanDriverType::Xiaomi;  // P1 default until fan_type NVS lands in P2
  }
  if (!isXiaomi()) return false;
  if (gBegun) return true;
  if (!SmartMiFan::begin(gUdp)) return false;
  gBegun = true;
  return true;
}

void end() {
  if (!gBegun) return;
  if (isXiaomi()) SmartMiFan::end();
  gBegun = false;
}

void update() {
  if (!gBegun || !isXiaomi()) return;
  SmartMiFan::update();
}

void clear() {
  if (isXiaomi()) SmartMiFan::clear();
}

void prepareForSleep(bool closeSocket) {
  if (isXiaomi()) SmartMiFan::prepareForSleep(closeSocket);
}

void setEventCallback(EventCallback cb) {
  if (isXiaomi()) SmartMiFan::setEventCallback(cb);
}

void setHandshakeTtl(uint32_t seconds) {
  if (isXiaomi()) SmartMiFan::setHandshakeTtl(seconds);
}

bool add(uint8_t index, const char* ipStr, const char* tokenHex, const char* model) {
  return isXiaomi() && SmartMiFan::add(index, ipStr, tokenHex, model);
}

bool remove(uint8_t index) {
  return isXiaomi() && SmartMiFan::remove(index);
}

FanMask used() {
  return isXiaomi() ? SmartMiFan::used() : static_cast<FanMask>(0);
}

FanMask participating() {
  return isXiaomi() ? SmartMiFan::participating() : static_cast<FanMask>(0);
}

bool info(uint8_t index, SmartMiFanInfo& out) {
  return isXiaomi() && SmartMiFan::info(index, out);
}

bool slot(uint8_t index, FanSlotView& out) {
  SmartMiFanInfo info;
  if (!FanBus::info(index, info)) return false;
  out.ip = info.ip;
  strncpy(out.model, info.model, sizeof(out.model) - 1);
  out.model[sizeof(out.model) - 1] = '\0';
  out.ready = info.ready;
  out.used = info.used;
  out.lastError = miioToFanErr(info.lastError);
  out.participation = info.participation;
  return true;
}

size_t copyUsedSlots(FanSlotView* out, size_t cap) {
  if (!out || cap == 0) return 0;
  size_t n = 0;
  for (uint8_t i = 0; i < FAN_MAX_SLOTS && n < cap; ++i) {
    FanSlotView v;
    if (!slot(i, v) || !v.used) continue;
    out[n++] = v;
  }
  return n;
}

FanParticipationState participation(uint8_t index) {
  return isXiaomi() ? SmartMiFan::participation(index)
                    : FanParticipationState::INACTIVE;
}

bool connect(uint32_t discoveryMs) {
  return isXiaomi() && SmartMiFan::connect(discoveryMs);
}

bool discover(FanMask slots, uint32_t durationMs) {
  return isXiaomi() && SmartMiFan::discover(slots, durationMs);
}

bool connecting() {
  return isXiaomi() && SmartMiFan::connecting();
}

bool discovering() {
  return isXiaomi() && SmartMiFan::discovering();
}

void cancel() {
  if (isXiaomi()) SmartMiFan::cancel();
}

void setEnabled(uint8_t index, bool enabled) {
  if (isXiaomi()) SmartMiFan::setEnabled(index, enabled);
}

void setSoftActive(uint8_t index, bool soft) {
  if (isXiaomi()) SmartMiFan::setSoftActive(index, soft);
}

CmdResult setPower(uint8_t index, bool on) {
  return isXiaomi() ? SmartMiFan::setPower(index, on) : CmdResult::REJECTED;
}

CmdResult setSpeed(uint8_t index, uint8_t pct) {
  return isXiaomi() ? SmartMiFan::setSpeed(index, pct) : CmdResult::REJECTED;
}

CmdResult handshake(uint8_t index) {
  return isXiaomi() ? SmartMiFan::handshake(index) : CmdResult::REJECTED;
}

CmdAccept setPowerAll(bool on, Scope scope) {
  if (!isXiaomi()) return CmdAccept{CmdResult::REJECTED, 0};
  return SmartMiFan::setPowerAll(on, scope);
}

CmdAccept setSpeedAll(uint8_t pct, Scope scope) {
  if (!isXiaomi()) return CmdAccept{CmdResult::REJECTED, 0};
  return SmartMiFan::setSpeedAll(pct, scope);
}

bool commandPending() {
  return isXiaomi() && SmartMiFan::commandPending();
}

bool commandPending(CmdKind kind) {
  return isXiaomi() && SmartMiFan::commandPending(kind);
}

}  // namespace FanBus

#endif  // HW_ENABLE_FANS
