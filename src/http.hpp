#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// Downloads a URL (http or https) into `body`. Returns the HTTP status code (200 = OK), or 0 if
// the server couldn't be reached (no internet). Blocking: call it from a background thread.
//   Windows: WinHTTP.  Linux / Mac: the curl command.
int httpGet(const std::string& url, std::vector<uint8_t>& body);

// Downloads a URL straight into a file (for big files), calling progress(bytes so far, total size
// or 0 if unknown) as it goes; progress returning false stops it. Returns the HTTP status code
// (200 = OK, the file is complete), or 0 if it failed or was stopped (the file is then removed).
int httpDownload(const std::string& url, const std::filesystem::path& file,
                 const std::function<bool(uint64_t, uint64_t)>& progress);

// Percent-encodes a URL path part ("Super Mario Bros. (World)" -> "Super%20Mario%20Bros.%20%28World%29").
std::string urlEncode(const std::string& s);
