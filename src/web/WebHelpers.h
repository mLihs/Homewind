/**
 * @file WebHelpers.h
 * @brief Common web server helper functions
 * 
 * Shared utilities for HTTP request handling, used by ApiActions and ApiSettings.
 */

#ifndef HOMEWIND_WEB_HELPERS_H
#define HOMEWIND_WEB_HELPERS_H

#include <Arduino.h>
#include <ESPAsyncWebServer.h>

namespace WebHelpers {

/**
 * @brief Send 503 Service Unavailable response during maintenance mode
 * 
 * @param request HTTP request object
 */
void sendMaintenance503(AsyncWebServerRequest* request);

/**
 * @brief Get string parameter from HTTP request
 * 
 * Checks query params first, then form params.
 * 
 * @param request HTTP request object
 * @param name Parameter name
 * @param out Output string (set on success)
 * @param required If true, sends 400 error if parameter missing
 * @return true if parameter found, false otherwise
 */
bool getStringParam(AsyncWebServerRequest* request, const char* name, String& out, bool required = false);

/**
 * @brief Get string parameter from HTTP request into fixed-size buffer (no heap allocation)
 * 
 * Checks query params first, then form params.
 * Copies parameter value directly into provided buffer.
 * 
 * @param request HTTP request object
 * @param name Parameter name
 * @param buffer Output buffer (null-terminated on success)
 * @param bufferSize Size of output buffer
 * @param required If true, returns false if parameter missing (caller handles error)
 * @return true if parameter found and fits in buffer, false otherwise
 * 
 * @note This function avoids String allocations - use for frequently called handlers
 */
bool getParamToBuffer(AsyncWebServerRequest* request, const char* name, 
                      char* buffer, size_t bufferSize, bool required = false);

/**
 * @brief Get integer parameter from HTTP request (no heap allocation)
 * 
 * Parses parameter value as integer.
 * 
 * @param request HTTP request object
 * @param name Parameter name
 * @param out Output integer (set on success)
 * @param required If true, returns false if parameter missing
 * @return true if parameter found and valid integer, false otherwise
 */
bool getIntParam(AsyncWebServerRequest* request, const char* name, 
                 int& out, bool required = false);

/**
 * @brief Get boolean parameter from HTTP request (no heap allocation)
 * 
 * Accepts "true", "1", "yes" as true; "false", "0", "no" as false.
 * 
 * @param request HTTP request object
 * @param name Parameter name
 * @param out Output boolean (set on success)
 * @param required If true, returns false if parameter missing
 * @return true if parameter found and valid boolean, false otherwise
 */
bool getBoolParam(AsyncWebServerRequest* request, const char* name, 
                  bool& out, bool required = false);

} // namespace WebHelpers

#endif // HOMEWIND_WEB_HELPERS_H
