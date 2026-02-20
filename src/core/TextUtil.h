/**
 * @file TextUtil.h
 * @brief Bounded text building utilities for deterministic HTTP responses
 * 
 * Provides fixed-size buffer helpers to avoid unbounded String concatenation
 * and heap fragmentation in HTTP handlers.
 */

#ifndef HOMEWIND_TEXT_UTIL_H
#define HOMEWIND_TEXT_UTIL_H

#include <Arduino.h>
#include <stdarg.h>

/**
 * Append formatted text to a fixed buffer
 * @param dst Destination buffer
 * @param dstSize Total size of destination buffer
 * @param used Current used size (will be updated)
 * @param fmt printf-style format string
 * @param ... Format arguments
 * @return true if appended successfully, false if truncated
 */
bool appendf(char* dst, size_t dstSize, size_t& used, const char* fmt, ...);

/**
 * Truncate ASCII string safely to max length
 * @param dst Destination buffer
 * @param dstSize Size of destination buffer
 * @param src Source string
 * @param maxChars Maximum characters to copy (excluding ellipsis)
 * @param addEllipsis If true, add "…" if truncated
 * @return Number of bytes written (excluding NUL terminator)
 */
size_t truncate_ascii(char* dst, size_t dstSize, const char* src, size_t maxChars, bool addEllipsis);

/**
 * Format IPAddress to string buffer (no heap allocation)
 * 
 * Replaces ip.toString().c_str() pattern which creates temporary String.
 * 
 * @param ip IPAddress to format
 * @param buffer Output buffer (must be at least 16 bytes for "xxx.xxx.xxx.xxx\0")
 * @param bufferSize Size of output buffer
 * @return Pointer to buffer (for use in printf-style calls)
 * 
 * @example
 *   char ipBuf[16];
 *   HW_DEBUG_PRINTF("IP: %s\n", ipToBuffer(myIp, ipBuf, sizeof(ipBuf)));
 */
const char* ipToBuffer(const IPAddress& ip, char* buffer, size_t bufferSize);

#endif // HOMEWIND_TEXT_UTIL_H

