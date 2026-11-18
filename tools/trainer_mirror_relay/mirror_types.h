// Types for trainer_mirror_relay.ino (own header so Arduino's prototype generator
// sees them before the generated prototypes).
#pragma once
#include <Arduino.h>

static const int MAX_SVC = 16;
static const int MAX_CHR = 48;
static const int UUID_STR = 40;
static const int VAL_MAX = 64;           // attribute value cap (prototype; see design 2.2.4)
static const int TABLE_BYTES = 3600;

enum : uint8_t { P_READ = 0x01, P_WRITE = 0x02, P_WNR = 0x04, P_NOTIFY = 0x08, P_INDICATE = 0x10 };

struct MChr {
  uint8_t svc;
  char uuid[UUID_STR];
  uint8_t props;
  uint8_t vlen;
  uint8_t val[VAL_MAX];
};

struct MirrorTable {
  uint8_t nsvc = 0, nchr = 0;
  char svc[MAX_SVC][UUID_STR];
  MChr chr[MAX_CHR];
};

enum : uint8_t { DOWN_WRITE = 1, DOWN_SUB = 2 };
struct DownEvt { uint8_t kind; uint8_t idx; uint16_t sub; uint8_t len; uint8_t data[VAL_MAX]; uint32_t t; };
struct UpEvt { uint8_t idx; bool notify; uint8_t len; uint8_t data[VAL_MAX]; uint32_t t; };
