/**
 * @file TextUtil.cpp
 * @brief Implementation of bounded text building utilities
 */

#include "TextUtil.h"
#include "DebugLog.h"

bool appendf(char* dst, size_t dstSize, size_t& used, const char* fmt, ...) {
  if (!dst || dstSize == 0 || used >= dstSize) {
    return false;
  }
  
  size_t available = dstSize - used;
  if (available < 2) { // Need at least space for NUL
    return false;
  }
  
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(dst + used, available, fmt, args);
  va_end(args);
  
  if (written < 0) {
    // Encoding error
    return false;
  }
  
  if (static_cast<size_t>(written) >= available) {
    // Truncated - ensure NUL termination
    dst[dstSize - 1] = '\0';
    used = dstSize - 1;
    // Log truncation once (not spam) - helps detect buffer size issues
    static bool truncationLogged = false;
    if (!truncationLogged) {
      HW_DEBUG_PRINTLN("[TextUtil] Warning: appendf truncated output (buffer may be too small)");
      truncationLogged = true;
    }
    return false; // Indicate truncation
  }
  
  used += static_cast<size_t>(written);
  return true;
}

size_t truncate_ascii(char* dst, size_t dstSize, const char* src, size_t maxChars, bool addEllipsis) {
  if (!dst || dstSize == 0) {
    return 0;
  }
  
  if (!src) {
    dst[0] = '\0';
    return 0;
  }
  
  // Calculate max length considering ellipsis
  size_t maxLen = maxChars;
  if (addEllipsis && maxLen > 0) {
    maxLen--; // Reserve space for ellipsis
  }
  
  // Find actual length to copy (up to maxLen)
  size_t srcLen = strlen(src);
  size_t copyLen = (srcLen > maxLen) ? maxLen : srcLen;
  
  // Ensure we don't exceed buffer
  if (copyLen >= dstSize) {
    copyLen = dstSize - 1;
  }
  
  // Copy characters
  memcpy(dst, src, copyLen);
  size_t written = copyLen;
  
  // Add ellipsis if truncated
  if (addEllipsis && srcLen > maxLen && written < dstSize - 1) {
    dst[written] = '…'; // UTF-8 ellipsis (0xE2 0x80 0xA6)
    written++;
  }
  
  // Ensure NUL termination
  if (written >= dstSize) {
    written = dstSize - 1;
  }
  dst[written] = '\0';
  
  return written;
}

const char* ipToBuffer(const IPAddress& ip, char* buffer, size_t bufferSize) {
  if (!buffer || bufferSize < 8) {  // Minimum "0.0.0.0\0"
    static const char empty[] = "";
    return empty;
  }
  
  // Format IP directly without String allocation
  // IPAddress provides operator[] to access octets
  snprintf(buffer, bufferSize, "%u.%u.%u.%u", 
           ip[0], ip[1], ip[2], ip[3]);
  
  return buffer;
}

