/**
 * @file Crc16.cpp
 * @brief CRC16-CCITT calculation utility implementation
 */

#include "Crc16.h"

namespace Crc16 {

/**
 * @brief CRC16-CCITT calculation (polynomial 0x1021)
 * 
 * Standard CRC16-CCITT with initial value 0xFFFF.
 * Used for data integrity verification in NVS storage.
 */
uint16_t calcCRC16(const uint8_t* data, size_t len) {
  uint16_t crc = 0xFFFF;
  
  for (size_t i = 0; i < len; i++) {
    crc ^= (uint16_t)data[i] << 8;
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x8000) {
        crc = (crc << 1) ^ 0x1021;
      } else {
        crc <<= 1;
      }
    }
  }
  
  return crc;
}

} // namespace Crc16
