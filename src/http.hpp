#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Downloads a URL (http or https) into `body`. Returns the HTTP status code (200 = OK), or 0 if
// the server couldn't be reached (no internet). Blocking: call it from a background thread.
//   Windows: WinHTTP.  Linux / Mac: the curl command.
int httpGet(const std::string& url, std::vector<uint8_t>& body);

// Percent-encodes a URL path part ("Super Mario Bros. (World)" -> "Super%20Mario%20Bros.%20%28World%29").
std::string urlEncode(const std::string& s);
