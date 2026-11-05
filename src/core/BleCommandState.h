/**
 * @file BleCommandState.h
 * @brief Arduino-free BLE command queues and reconcile state (design §7.1, §7.2)
 *
 * Shared by BLERelayManager and tests/ble_command_state_test.cpp. No locking
 * here: BLERelayManager guards the queues with _bleMux; the reconciler runs on
 * the loop task only.
 */

#ifndef HOMEWIND_BLE_COMMAND_STATE_H
#define HOMEWIND_BLE_COMMAND_STATE_H

#include <cstdint>
#include <cstring>
#include <strings.h>

namespace hw {

// Same bit values as BikeSensor::SensorType.
constexpr uint8_t kBleTypeHR = 0x01;
constexpr uint8_t kBleTypeCSC = 0x02;
// Slot 0 = HR, slot 1 = CSC: reconcile and NVS restore apply HR first.
constexpr uint8_t kBleSlots = 2;

inline int bleSlotOf(uint8_t typeBit) {
  return typeBit == kBleTypeHR ? 0 : (typeBit == kBleTypeCSC ? 1 : -1);
}
inline uint8_t bleTypeOfSlot(uint8_t slot) { return slot == 0 ? kBleTypeHR : kBleTypeCSC; }

// "xx:xx:xx:xx:xx:xx" (hex, either case) and addrType -1/0/1: the inputs
// BikeSensor::addByMac() accepts, so a false from it can only mean duplicate.
inline bool bleAddressValid(const char* mac, int8_t addrType) {
  if (!mac || addrType < -1 || addrType > 1) return false;
  for (int i = 0; i < 17; ++i) {
    const char c = mac[i];
    if (i % 3 == 2) {
      if (c != ':') return false;
    } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
      return false;
    }
  }
  return mac[17] == '\0';
}

// ---------------------------------------------------------------------------
// Sensor command queue (tagged union, capacity 8)
// ---------------------------------------------------------------------------
enum class BleCmdKind : uint8_t { SET_FROM_DISCOVERY, DELETE, CONNECT, DISCONNECT, RELOAD };

struct BleCommand {
  BleCmdKind kind;
  union {
    struct { uint32_t generation; uint8_t index; bool force; } set;
    char name[64];   // DELETE / CONNECT / DISCONNECT
  };
};

class BleCommandQueue {
public:
  static constexpr uint8_t kCapacity = 8;
  bool push(const BleCommand& cmd) {
    if (_count >= kCapacity) return false;
    _items[(_head + _count) % kCapacity] = cmd;
    _count++;
    return true;
  }
  bool pop(BleCommand& out) {
    if (_count == 0) return false;
    out = _items[_head];
    _head = static_cast<uint8_t>((_head + 1) % kCapacity);
    _count--;
    return true;
  }
  void clear() { _head = 0; _count = 0; }
  uint8_t size() const { return _count; }

private:
  BleCommand _items[kCapacity] = {};
  uint8_t _head = 0;
  uint8_t _count = 0;
};

// ---------------------------------------------------------------------------
// Discovery START/STOP: two-entry ordered queue. Reachable contents are
// [], [START], [STOP] and [STOP, START]; STOP cancels a START not yet issued.
// ---------------------------------------------------------------------------
enum class BleDiscOp : uint8_t { START, STOP };

class BleDiscoveryQueue {
public:
  void requestStart(uint8_t mask) {
    if (_count > 0 && _ops[_count - 1] == BleDiscOp::START) {
      _mask = mask;
      return;
    }
    _ops[_count++] = BleDiscOp::START;
    _mask = mask;
  }
  // open: a discovery is running or its STOP has not seen DISCOVERY_DONE yet.
  void requestStop(bool open) {
    if (_count > 0 && _ops[_count - 1] == BleDiscOp::START) _count--;
    if (_count > 0) return;          // [STOP] remains
    if (open) _ops[_count++] = BleDiscOp::STOP;
  }
  bool peek(BleDiscOp& op) const {
    if (_count == 0) return false;
    op = _ops[0];
    return true;
  }
  void pop() {
    if (_count == 0) return;
    _ops[0] = _ops[1];
    _count--;
  }
  uint8_t mask() const { return _mask; }
  void clear() { _count = 0; }
  uint8_t size() const { return _count; }

private:
  BleDiscOp _ops[2] = {BleDiscOp::START, BleDiscOp::START};
  uint8_t _count = 0;
  uint8_t _mask = 0;
};

// ---------------------------------------------------------------------------
// Desired/applied reconcile with duplicate-address handling (§7.1)
// ---------------------------------------------------------------------------
class BleLibraryPort {
public:
  virtual ~BleLibraryPort() {}
  virtual bool connected(uint8_t typeBit) = 0;
  virtual bool add(uint8_t typeBit, const char* mac, int8_t addrType, const char* name) = 0;
  virtual void remove(uint8_t typeBit) = 0;
};

class BleReconciler {
public:
  static constexpr size_t kNameLen = 64;
  static constexpr size_t kMacLen = 18;

  // force: true = replace a connected sensor now; false = apply on disconnect.
  void setTarget(uint8_t typeBit, const char* name, const char* mac, int8_t addrType,
                 bool enabled, bool force) {
    const int i = bleSlotOf(typeBit);
    if (i < 0) return;
    Target& t = _target[i];
    copy(t.name, name, kNameLen);
    copy(t.mac, mac, kMacLen);
    t.addrType = addrType;
    t.enabled = enabled;
    if (force) t.force = true;   // cleared after apply
    t.dirty = true;
  }

  bool pending() const { return _target[0].dirty || _target[1].dirty; }
  bool duplicate(uint8_t typeBit) const {
    const int i = bleSlotOf(typeBit);
    return i >= 0 && _dup[i];
  }
  bool added(uint8_t typeBit) const {
    const int i = bleSlotOf(typeBit);
    return i >= 0 && _applied[i].added;
  }

  // One pass in slot order (HR before CSC). A duplicate is retried only when
  // the other slot's applied identity changes.
  void run(BleLibraryPort& port) {
    for (uint8_t pass = 0; pass < 3; ++pass) {
      _retryFlagged = false;
      for (uint8_t i = 0; i < kBleSlots; ++i) step(i, port);
      if (!_retryFlagged) break;
    }
  }

  // Maintenance: the library slots are removed directly.
  void forgetApplied() {
    for (uint8_t i = 0; i < kBleSlots; ++i) {
      _applied[i] = Applied();
      _target[i] = Target();
      _dup[i] = false;
    }
  }

private:
  struct Target {
    bool enabled = false;
    bool force = false;
    bool dirty = false;
    int8_t addrType = -1;
    char name[kNameLen] = {};
    char mac[kMacLen] = {};
  };
  struct Applied {
    bool added = false;
    int8_t addrType = -1;
    char mac[kMacLen] = {};
  };

  Target _target[kBleSlots];
  Applied _applied[kBleSlots];
  bool _dup[kBleSlots] = {false, false};
  bool _retryFlagged = false;

  static void copy(char* dst, const char* src, size_t len) {
    if (!src) { dst[0] = '\0'; return; }
    strncpy(dst, src, len - 1);
    dst[len - 1] = '\0';
  }

  void identityChanged(uint8_t slot) {
    for (uint8_t j = 0; j < kBleSlots; ++j) {
      if (j != slot && _dup[j] && _target[j].enabled) {
        _target[j].dirty = true;
        _retryFlagged = true;
      }
    }
  }

  void step(uint8_t i, BleLibraryPort& port) {
    Target& t = _target[i];
    Applied& a = _applied[i];
    if (!t.dirty) return;
    const uint8_t type = bleTypeOfSlot(i);
    const bool conn = a.added && port.connected(type);

    if (!t.enabled) {
      if (a.added) {
        if (conn && !t.force) return;    // keep dirty until the link ends
        port.remove(type);
        a = Applied();
        identityChanged(i);
      }
      _dup[i] = false;
      t.dirty = false;
      t.force = false;
      return;
    }

    if (!bleAddressValid(t.mac, t.addrType)) {   // never offered to the library
      t.dirty = false;
      return;
    }
    const bool same = a.added && strcasecmp(a.mac, t.mac) == 0 && a.addrType == t.addrType;
    if (same) {
      t.dirty = false;
      t.force = false;
      return;
    }
    if (conn && !t.force) return;        // apply-on-disconnect

    if (port.add(type, t.mac, t.addrType, t.name)) {
      a.added = true;
      a.addrType = t.addrType;
      copy(a.mac, t.mac, kMacLen);
      _dup[i] = false;
      identityChanged(i);
    } else {
      if (a.added) {
        port.remove(type);
        a = Applied();
        identityChanged(i);
      }
      _dup[i] = true;
    }
    t.dirty = false;
    t.force = false;
  }
};

}  // namespace hw

#endif  // HOMEWIND_BLE_COMMAND_STATE_H
