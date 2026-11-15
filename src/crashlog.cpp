#include "crashlog.hpp"

#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <mutex>
#include <sstream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path g_file;     // last-run.txt
std::mutex g_mutex;  // (steps can come from more than one thread)
const char* const kClosedNormally = "closed normally";

// The step, also kept in plain memory for the crash handler (which shouldn't allocate).
char g_step[512] = "starting";

void writeFile(const std::string& text) {
    if (g_file.empty()) return;
    std::ofstream f(g_file, std::ios::binary | std::ios::trunc);
    f << text;
}

#if defined(_WIN32)
wchar_t g_crashPath[1024] = {};

// Appends text to last-run.txt with plain Windows calls (safe while crashing).
void crashWrite(const char* text) {
    if (!g_crashPath[0]) return;
    HANDLE h = CreateFileW(g_crashPath, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(h, text, DWORD(std::strlen(text)), &written, nullptr);
    CloseHandle(h);
}

// "dolphin_libretro.dll+0x1234ab" for an address.
void describeAddress(const void* addr, char* out, size_t size) {
    HMODULE mod = nullptr;
    char path[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(addr), &mod) &&
        mod)
        GetModuleFileNameA(mod, path, MAX_PATH);
    const char* name = std::strrchr(path, '\\');
    name = name ? name + 1 : path;
    std::snprintf(out, size, "%s+0x%llx", name,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(addr) - reinterpret_cast<uintptr_t>(mod)));
}

LONG WINAPI onCrash(EXCEPTION_POINTERS* ep) {
    static volatile LONG once = 0;
    if (InterlockedExchange(&once, 1) != 0) return EXCEPTION_CONTINUE_SEARCH;
    char line[600], where[300];
    describeAddress(ep->ExceptionRecord->ExceptionAddress, where, sizeof where);
    std::snprintf(line, sizeof line, "\ncrash: exception 0x%08lx at %s (thread %lu)\ncalls:\n",
                  static_cast<unsigned long>(ep->ExceptionRecord->ExceptionCode), where,
                  static_cast<unsigned long>(GetCurrentThreadId()));
    crashWrite(line);
    void* frames[48];
    const USHORT n = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < n; ++i) {
        describeAddress(frames[i], where, sizeof where);
        std::snprintf(line, sizeof line, "  %s\n", where);
        crashWrite(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// An error nothing caught (a C++ exception): written down before closing.
[[noreturn]] void onTerminate() {
    std::string what = "an error nothing caught";
    if (const std::exception_ptr e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& x) {
            what += std::string(": ") + x.what();
        } catch (...) {
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        writeFile(std::string(g_step) + "\ncrash: " + what + "\n");
    }
    std::fprintf(stderr, "Crash: %s\n", what.c_str());
    std::abort();
}

}  // namespace

std::string crashLogStart(const fs::path& dataDir) {
    std::error_code ec;
    fs::create_directories(dataDir, ec);
    g_file = dataDir / "last-run.txt";
    std::string previous;
    {
        std::ifstream f(g_file, std::ios::binary);
        std::stringstream s;
        s << f.rdbuf();
        previous = s.str();
    }
    if (!previous.empty() && previous.rfind(kClosedNormally, 0) != 0) {
        // Kept for whoever looks into it (the next crash replaces it).
        std::ofstream(dataDir / "crash.txt", std::ios::binary | std::ios::trunc) << previous;
    } else {
        previous.clear();
    }
#if defined(_WIN32)
    const std::wstring w = g_file.wstring();
    if (w.size() < sizeof g_crashPath / sizeof g_crashPath[0]) std::wcscpy(g_crashPath, w.c_str());
    SetUnhandledExceptionFilter(onCrash);
#endif
    std::set_terminate(onTerminate);
    crashStep("starting Nintendall");
    // The first line (what it was doing) is enough to tell the player.
    if (const size_t nl = previous.find('\n'); nl != std::string::npos) previous.resize(nl);
    return previous;
}

void crashStep(const std::string& what) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::snprintf(g_step, sizeof g_step, "%s", what.c_str());
    writeFile(what + "\n");
}

void crashLogEnd() {
    std::lock_guard<std::mutex> lock(g_mutex);
    writeFile(std::string(kClosedNormally) + "\n");
}
