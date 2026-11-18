// Types for trainer_face_test.ino (separate header so Arduino's prototype
// generator sees them before the generated function prototypes).
#pragma once
#include <NimBLEDevice.h>

// Write events: NimBLE host task -> loop()
struct WriteEvt { uint8_t which; uint16_t conn; uint8_t len; uint8_t data[20]; uint32_t t; };

// Per-connection indication state for one characteristic (one outstanding).
static const int MAXC = 4;
struct IndSlot {
  NimBLECharacteristic* chr;
  uint16_t conn[MAXC];
  bool subscribed[MAXC];
  bool pending[MAXC];
  uint32_t sentAt[MAXC];
  // one queued response per connection (FTMS expects one response per write;
  // the tool does not supersede, so a second write while pending waits here)
  uint8_t qbuf[MAXC][20];
  uint8_t qlen[MAXC];
  uint32_t dueAt[MAXC];
};
