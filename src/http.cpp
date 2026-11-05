#include "http.hpp"

#include <cstdio>
#include <fstream>

namespace fs = std::filesystem;

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

namespace {
// Receives the body as it arrives; returning false stops the download.
using Sink = std::function<bool(const uint8_t* data, size_t size, uint64_t total)>;
}  // namespace

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

// GET with WinHTTP (it follows redirects, e.g. GitHub downloads -> its file servers).
// Returns the status code; 0 if the server couldn't be reached or the sink stopped it.
int fetch(const std::string& url, const Sink& sink) {
    const std::wstring wurl = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof parts;
    wchar_t host[256] = {}, path[4096] = {};
    parts.lpszHostName = host;
    parts.dwHostNameLength = 256;
    parts.lpszUrlPath = path;
    parts.dwUrlPathLength = 4096;
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
                DWORD total = 0;
                len = sizeof total;
                if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                         WINHTTP_HEADER_NAME_BY_INDEX, &total, &len, WINHTTP_NO_HEADER_INDEX))
                    total = 0;
                std::vector<uint8_t> buf;
                for (;;) {
                    DWORD avail = 0;
                    if (!WinHttpQueryDataAvailable(req, &avail)) {  // connection lost
                        status = 0;
                        break;
                    }
                    if (avail == 0) break;  // finished
                    buf.resize(avail);
                    DWORD got = 0;
                    if (!WinHttpReadData(req, buf.data(), avail, &got)) {
                        status = 0;
                        break;
                    }
                    if (status == 200 && !sink(buf.data(), got, total)) {
                        status = 0;
                        break;
                    }
                }
            }
            WinHttpCloseHandle(req);
        }
        WinHttpCloseHandle(connect);
    }
    WinHttpCloseHandle(session);
    return status;
}

}  // namespace

int httpGet(const std::string& url, std::vector<uint8_t>& body) {
    body.clear();
    const int status = fetch(url, [&](const uint8_t* d, size_t n, uint64_t) {
        body.insert(body.end(), d, d + n);
        return true;
    });
    if (status != 200) body.clear();
    return status;
}

int httpDownload(const std::string& url, const fs::path& file,
                 const std::function<bool(uint64_t, uint64_t)>& progress) {
    int status = 0;
    uint64_t got = 0, size = 0;
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) return 0;
        status = fetch(url, [&](const uint8_t* d, size_t n, uint64_t total) {
            out.write(reinterpret_cast<const char*>(d), std::streamsize(n));
            got += n;
            size = total;
            return bool(out) && progress(got, total);
        });
        out.close();
        if (!out) status = 0;
    }
    if (status == 200 && size != 0 && got != size) status = 0;  // cut off
    std::error_code ec;
    if (status != 200) fs::remove(file, ec);
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

// Linux / Mac don't update themselves (no installer), so this is the simple way.
int httpDownload(const std::string& url, const fs::path& file,
                 const std::function<bool(uint64_t, uint64_t)>& progress) {
    std::vector<uint8_t> body;
    const int status = httpGet(url, body);
    if (status != 200 || !progress(body.size(), body.size())) return 0;
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(body.data()), std::streamsize(body.size()));
    return out ? status : 0;
}

#endif
