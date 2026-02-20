/**
 * @file Crc16.h
 * @brief CRC16-CCITT calculation utility
 * 
 * Centralized CRC16-CCITT implementation (polynomial 0x1021, initial value 0xFFFF).
 * Used for data integrity verification in NVS storage.
 */

#ifndef HOMEWIND_CRC16_H
#define HOMEWIND_CRC16_H

#include <stdint.h>
#include <stddef.h>

namespace Crc16 {

/**
 * @brief Calculate CRC16-CCITT checksum
 * 
 * Standard CRC16-CCITT with polynomial 0x1021 and initial value 0xFFFF.
 * Used for data integrity verification in NVS storage.
 * 
 * @param data Data buffer
 * @param len Data length in bytes
 * @return CRC16 value
 */
uint16_t calcCRC16(const uint8_t* data, size_t len);

} // namespace Crc16

#endif // HOMEWIND_CRC16_H
