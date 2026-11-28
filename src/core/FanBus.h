#pragma once

/**
 * Thin Homewind façade over the active fan driver (P1: Xiaomi / SmartMiFanAsync).
 * Call sites use FanBus only; driver selection is fixed at boot.
 *
 * Xiaomi's manager lives in FanCommon's single claimable arena (claimed in
 * begin(), released in end()). Only one driver may claim it; call end() before
 * a warm re-begin or when shutting down for maintenance.
 */

#include <Arduino.h>
#include <FanCommon.h>
#include <SmartMiFanAsync.h>

enum class FanDriverType : uint8_t {
  None = 0,
  Xiaomi = 1,
  Dyson = 2
};

/** Driver-neutral slot snapshot (no SmartMiFanInfo in public Homewind APIs). */
struct FanSlotView {
  IPAddress ip;
  char model[24];
  bool ready;
  bool used;
  FanErr lastError;
  FanParticipationState participation;

  FanSlotView()
      : ip(0, 0, 0, 0),
        ready(false),
        used(false),
        lastError(FanErr::OK),
        participation(FanParticipationState::INACTIVE) {
    model[0] = '\0';
  }
};

namespace FanBus {

bool select(FanDriverType type);
FanDriverType selected();

/** Own the UDP socket; maps to SmartMiFan::begin for Xiaomi. */
bool begin();
void end();
void update();
void clear();
void prepareForSleep(bool closeSocket);
void setEventCallback(EventCallback cb);
void setHandshakeTtl(uint32_t seconds);

bool add(uint8_t index, const char* ipStr, const char* tokenHex,
         const char* model = nullptr);
bool remove(uint8_t index);
FanMask used();
FanMask participating();
/** Xiaomi-only detail; prefer `slot()` at Homewind boundaries. */
bool info(uint8_t index, SmartMiFanInfo& out);
bool slot(uint8_t index, FanSlotView& out);
/** Fill used slots into `out[0..cap)`; returns count written. */
size_t copyUsedSlots(FanSlotView* out, size_t cap);
FanParticipationState participation(uint8_t index);

bool connect(uint32_t discoveryMs = 3000);
bool discover(FanMask slots, uint32_t durationMs = 3000);
bool connecting();
bool discovering();
void cancel();

void setEnabled(uint8_t index, bool enabled);
void setSoftActive(uint8_t index, bool soft);

CmdResult setPower(uint8_t index, bool on);
CmdResult setSpeed(uint8_t index, uint8_t pct);
CmdResult handshake(uint8_t index);
CmdAccept setPowerAll(bool on, Scope scope = Scope::PARTICIPATING);
CmdAccept setSpeedAll(uint8_t pct, Scope scope = Scope::PARTICIPATING);
bool commandPending();
bool commandPending(CmdKind kind);

}  // namespace FanBus
