#include "http.hpp"

#include <cstdio>

std::string urlEncode(const std::string& s) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (const unsigned char c : s) {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out += char(c);
        } else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>

namespace {
std::wstring widen(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
    return w;
}
}  // namespace

int httpGet(const std::string& url, std::vector<uint8_t>& body) {
    body.clear();
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[2048] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(wurl.c_str(), 0, 0, &parts)) return 0;

    int status = 0;
    HINTERNET session = WinHttpOpen(L"Nintendall/" NINTENDALL_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                    WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return 0;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    if (HINTERNET connect = WinHttpConnect(session, host, parts.nPort, 0)) {
        const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
        if (HINTERNET req = WinHttpOpenRequest(connect, L"GET", path, nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, flags)) {
            if (WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                WinHttpReceiveResponse(req, nullptr)) {
                DWORD code = 0, len = sizeof code;
                WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX);
                status = int(code);
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
                    const size_t at = body.size();
                    body.resize(at + avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(req, body.data() + at, avail, &got)) {
                        status = 0;
                        break;
                    }
                    body.resize(at + got);
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    if (status != 200) body.clear();
    return status;
}

#else

int httpGet(const std::string& url, std::vector<uint8_t>& body) {
    body.clear();
    // Only plain URL characters reach the shell (callers percent-encode names).
    for (const char c : url)
        if (c == '\'' || c == '\\' || c < 32) return 0;
    // -w prints the status code after the body; it's split off below.
    const std::string cmd = "curl -sL --max-time 30 --connect-timeout 10 -A 'Nintendall/" NINTENDALL_VERSION
                            "' -w '\\n%{http_code}' '" + url + "' 2>/dev/null";
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return 0;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) body.insert(body.end(), buf, buf + n);
    pclose(p);
    // Last line: the status code.
    size_t nl = body.size();
    while (nl > 0 && body[nl - 1] != '\n') --nl;
    int status = 0;
    for (size_t i = nl; i < body.size(); ++i)
        if (body[i] >= '0' && body[i] <= '9') status = status * 10 + (body[i] - '0');
    body.resize(nl > 0 ? nl - 1 : 0);
    if (status != 200) body.clear();
    return status;
}

#endif
