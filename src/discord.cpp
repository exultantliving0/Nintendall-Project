#include "discord.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

// Discord's IPC: frames of [opcode, length] (32-bit little-endian) followed by JSON.
enum Op : uint32_t { kHandshake = 0, kFrame = 1, kClose = 2, kPing = 3, kPong = 4 };

int processId() {
#if defined(_WIN32)
    return int(GetCurrentProcessId());
#else
    return int(getpid());
#endif
}

// The connection to the Discord app: a named pipe on Windows, a Unix socket elsewhere.
class Pipe {
public:
    ~Pipe() { close(); }
    bool isOpen() const {
#if defined(_WIN32)
        return h_ != INVALID_HANDLE_VALUE;
#else
        return fd_ >= 0;
#endif
    }

    // Discord listens on discord-ipc-0 (or -1... if several Discord apps run).
    bool open() {
        close();
#if defined(_WIN32)
        for (int i = 0; i < 10; ++i) {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(i);
            h_ = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h_ != INVALID_HANDLE_VALUE) return true;
        }
        return false;
#else
        std::vector<std::string> dirs;
        for (const char* var : {"XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP"})
            if (const char* v = std::getenv(var); v && *v) dirs.push_back(v);
        dirs.push_back("/tmp");
        // Discord installed as a Flatpak or Snap puts its socket in a folder of its own.
        const char* subdirs[] = {"", "app/com.discordapp.Discord/", "app/com.discordapp.DiscordCanary/", "snap.discord/",
                                 "snap.discord-canary/", ".flatpak/dev.vencord.Vesktop/xdg-run/"};
        for (const std::string& d : dirs)
            for (const char* sub : subdirs)
                for (int i = 0; i < 10; ++i) {
                    const std::string path = d + (d.back() == '/' ? "" : "/") + sub + "discord-ipc-" + std::to_string(i);
                    sockaddr_un addr{};
                    if (path.size() >= sizeof addr.sun_path) continue;
                    addr.sun_family = AF_UNIX;
                    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
                    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (fd < 0) return false;
#if defined(__APPLE__)
                    int one = 1;
                    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
                    if (connect(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof addr) == 0) {
                        fd_ = fd;
                        return true;
                    }
                    ::close(fd);
                }
        return false;
#endif
    }

    void close() {
#if defined(_WIN32)
        if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
#else
        if (fd_ >= 0) ::close(fd_);
        fd_ = -1;
#endif
    }

    bool write(const void* data, size_t size) {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
#if defined(_WIN32)
            DWORD n = 0;
            if (!WriteFile(h_, p, DWORD(size), &n, nullptr) || n == 0) return false;
#else
#if defined(MSG_NOSIGNAL)
            const ssize_t n = send(fd_, p, size, MSG_NOSIGNAL);
#else
            const ssize_t n = send(fd_, p, size, 0);
#endif
            if (n <= 0) return false;
#endif
            p += n;
            size -= size_t(n);
        }
        return true;
    }

    // 1 = bytes are waiting, 0 = none (after waiting up to timeoutMs), -1 = closed.
    int waitReadable(int timeoutMs) {
#if defined(_WIN32)
        const auto until = Clock::now() + std::chrono::milliseconds(timeoutMs);
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
            if (avail > 0) return 1;
            if (Clock::now() >= until) return 0;
            Sleep(15);
        }
#else
        pollfd p{fd_, POLLIN, 0};
        const int r = poll(&p, 1, timeoutMs);
        if (r < 0) return -1;
        if (r == 0) return 0;
        if (p.revents & POLLIN) return 1;
        return -1;  // hung up
#endif
    }

    bool readExact(void* data, size_t size, int timeoutMs) {
        char* p = static_cast<char*>(data);
        while (size > 0) {
            if (waitReadable(timeoutMs) != 1) return false;
#if defined(_WIN32)
            DWORD n = 0;
            if (!ReadFile(h_, p, DWORD(size), &n, nullptr) || n == 0) return false;
#else
            const ssize_t n = recv(fd_, p, size, 0);
            if (n <= 0) return false;
#endif
            p += n;
            size -= size_t(n);
        }
        return true;
    }

private:
#if defined(_WIN32)
    HANDLE h_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
};

bool sendFrame(Pipe& pipe, uint32_t op, const std::string& json) {
    std::string buf(8, '\0');
    const uint32_t len = uint32_t(json.size());
    for (int i = 0; i < 4; ++i) {
        buf[size_t(i)] = char((op >> (8 * i)) & 0xFF);
        buf[size_t(4 + i)] = char((len >> (8 * i)) & 0xFF);
    }
    buf += json;
    return pipe.write(buf.data(), buf.size());
}

bool readFrame(Pipe& pipe, uint32_t& op, std::string& json, int timeoutMs) {
    unsigned char h[8];
    if (!pipe.readExact(h, 8, timeoutMs)) return false;
    op = uint32_t(h[0]) | uint32_t(h[1]) << 8 | uint32_t(h[2]) << 16 | uint32_t(h[3]) << 24;
    const uint32_t len = uint32_t(h[4]) | uint32_t(h[5]) << 8 | uint32_t(h[6]) << 16 | uint32_t(h[7]) << 24;
    if (len > 1 << 20) return false;
    json.assign(len, '\0');
    return len == 0 || pipe.readExact(json.data(), len, timeoutMs);
}

std::string quote(const std::string& s) {
    std::string out = "\"";
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20) {
            char b[8];
            std::snprintf(b, sizeof b, "\\u%04x", c);
            out += b;
        } else {
            out += ch;
        }
    }
    return out + "\"";
}

// Discord takes 2 to 128 characters per text: longer ones are cut (on a whole UTF-8 letter).
std::string fit(std::string s) {
    if (s.size() > 128) {
        s.resize(125);
        while (!s.empty() && (static_cast<unsigned char>(s.back()) & 0xC0) == 0x80) s.pop_back();
        if (!s.empty() && static_cast<unsigned char>(s.back()) >= 0xC0) s.pop_back();
        s += "...";
    }
    if (s.size() == 1) s += ' ';
    return s;
}

std::string activityJson(const DiscordStatus::Activity& a) {
    std::string j = "{\"type\":0,\"instance\":false";
    if (!a.details.empty()) j += ",\"details\":" + quote(fit(a.details));
    if (!a.state.empty()) j += ",\"state\":" + quote(fit(a.state));
    if (a.startTime > 0) j += ",\"timestamps\":{\"start\":" + std::to_string(a.startTime) + "}";
    std::string assets;
    auto add = [&](const char* key, const std::string& v) {
        if (v.empty()) return;
        assets += std::string(assets.empty() ? "" : ",") + "\"" + key + "\":" + quote(key[std::strlen(key) - 1] == 't' ? fit(v) : v);
    };
    add("large_image", a.largeImage);
    add("large_text", a.largeText);
    add("small_image", a.smallImage);
    add("small_text", a.smallText);
    if (!assets.empty()) j += ",\"assets\":{" + assets + "}";
    return j + "}";
}

}  // namespace

struct DiscordStatus::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::string appId;
    Activity activity;
    bool changed = false, stop = false;

    void run();
};

void DiscordStatus::Shared::run() {
    Pipe pipe;
    auto nextTry = Clock::now();
    bool sentOne = false;
    Activity sent;
    unsigned nonce = 0;

    // Reads what Discord sent, answering its pings. With a nonce: waits for Discord's answer to that
    // command. False = the connection is gone.
    auto drain = [&](const std::string& nonceText) {
        for (;;) {
            if (nonceText.empty()) {
                const int r = pipe.waitReadable(0);
                if (r <= 0) return r == 0;
            }
            uint32_t op = 0;
            std::string json;
            if (!readFrame(pipe, op, json, 5000) || op == kClose) return false;
            if (op == kPing && !sendFrame(pipe, kPong, json)) return false;
            if (op == kFrame && !nonceText.empty() && json.find(nonceText) != std::string::npos) {
                if (json.find("\"evt\":\"ERROR\"") != std::string::npos)
                    std::fprintf(stderr, "Discord status: %s\n", json.c_str());
                return true;
            }
        }
    };

    for (;;) {
        Activity want;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait_for(lock, std::chrono::seconds(5), [&] { return stop || changed; });
            if (stop) break;
            changed = false;
            want = activity;
        }
        if (pipe.isOpen() && !drain("")) {  // Discord closed
            pipe.close();
            sentOne = false;
        }
        if (!pipe.isOpen()) {
            if (Clock::now() < nextTry) continue;
            nextTry = Clock::now() + std::chrono::seconds(15);
            if (!pipe.open()) continue;
            uint32_t op = 0;
            std::string json;
            if (!sendFrame(pipe, kHandshake, "{\"v\":1,\"client_id\":" + quote(appId) + "}") ||
                !readFrame(pipe, op, json, 5000) || op != kFrame || json.find("READY") == std::string::npos) {
                if (op == kClose) std::fprintf(stderr, "Discord status: Discord said no: %s\n", json.c_str());
                pipe.close();
                nextTry = Clock::now() + std::chrono::seconds(op == kClose ? 120 : 15);
                continue;
            }
            std::fprintf(stderr, "Discord status: connected\n");
        }
        if (sentOne && sent == want) continue;
        const std::string n = "nintendall-" + std::to_string(++nonce);
        const std::string cmd = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(processId()) +
                                ",\"activity\":" + activityJson(want) + "},\"nonce\":" + quote(n) + "}";
        if (sendFrame(pipe, kFrame, cmd) && drain("\"" + n + "\"")) {
            sent = want;
            sentOne = true;
        } else {
            pipe.close();
            sentOne = false;
            nextTry = Clock::now() + std::chrono::seconds(5);
            std::lock_guard<std::mutex> lock(mutex);
            changed = true;  // try again when reconnected
        }
    }
    pipe.close();
}

DiscordStatus::~DiscordStatus() { stop(); }

void DiscordStatus::start(const std::string& appId) {
    if (shared_ || appId.empty()) return;
    shared_ = std::make_shared<Shared>();
    shared_->appId = appId;
    haveLast_ = false;
    // The thread keeps its own reference, so stopping never waits for Discord.
    std::thread([s = shared_] { s->run(); }).detach();
}

void DiscordStatus::stop() {
    if (!shared_) return;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->stop = true;
    }
    shared_->wake.notify_all();
    shared_.reset();
}

void DiscordStatus::set(const Activity& a) {
    if (!shared_ || (haveLast_ && a == last_)) return;
    last_ = a;
    haveLast_ = true;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->activity = a;
        shared_->changed = true;
    }
    shared_->wake.notify_all();
}
