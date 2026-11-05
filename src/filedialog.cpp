#include "filedialog.hpp"

#include <algorithm>
#include <cstdio>
#include <set>
#include <vector>

#include "systems.hpp"

// Every extension Nintendall can open, without dots, e.g. {"nes", "sfc", ..., "zip"}.
static std::vector<std::string> gameExtensions() {
    std::set<std::string> exts{"zip"};
    for (System s : allSystems())
        for (const auto& e : systemInfo(s).extensions) exts.insert(e.substr(1));
    return {exts.begin(), exts.end()};
}

#if defined(_WIN32)

#include <SDL_syswm.h>
#include <windows.h>
#include <commdlg.h>

static std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

static std::string narrow(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::string openFileDialog(SDL_Window* parent, const std::string& title, const std::string& kind,
                           const std::vector<std::string>& exts, bool& unavailable) {
    unavailable = false;
    HWND owner = nullptr;
    SDL_SysWMinfo info;
    SDL_VERSION(&info.version);
    if (parent && SDL_GetWindowWMInfo(parent, &info)) owner = info.info.win.window;

    // Filter string: "Games\0*.nes;*.sfc;...\0All files\0*.*\0\0"
    std::string patterns;
    for (const auto& e : exts) patterns += (patterns.empty() ? "*." : ";*.") + e;
    std::wstring filter = widen(kind);
    filter.push_back(L'\0');
    filter += widen(patterns);
    filter.push_back(L'\0');
    filter += L"All files";
    filter.push_back(L'\0');
    filter += L"*.*";
    filter.push_back(L'\0');
    filter.push_back(L'\0');

    std::vector<wchar_t> buf(32768, L'\0');
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = owner;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = buf.data();
    ofn.nMaxFile = DWORD(buf.size());
    const std::wstring wtitle = widen(title);
    ofn.lpstrTitle = wtitle.c_str();
    ofn.Flags = OFN_EXPLORER | OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_HIDEREADONLY;
    if (!GetOpenFileNameW(&ofn)) return "";
    return narrow(buf.data());
}

#else

static std::string runAndRead(const std::string& cmd, int& status) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) {
        status = -1;
        return out;
    }
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0) out.append(buf, n);
    status = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

static bool hasCommand(const char* name) {
    int status = 0;
    return !runAndRead(std::string("command -v ") + name + " 2>/dev/null", status).empty();
}

// Titles and labels are our own fixed text (no quotes), so they're safe inside the shell command.
std::string openFileDialog(SDL_Window*, const std::string& title, const std::string& kind,
                           const std::vector<std::string>& exts, bool& unavailable) {
    unavailable = false;
    int status = 0;
#if defined(__APPLE__)
    const std::string path = runAndRead(
        "osascript -e 'POSIX path of (choose file with prompt \"" + title + "\")' 2>/dev/null", status);
    return status == 0 ? path : "";
#else
    std::string patterns;
    for (const auto& e : exts) {
        std::string upper = e;
        std::transform(upper.begin(), upper.end(), upper.begin(), ::toupper);
        patterns += " *." + e + " *." + upper;
    }
    if (hasCommand("zenity")) {
        const std::string path = runAndRead("zenity --file-selection --title='" + title + "' --file-filter='" + kind +
                                                " |" + patterns + "' --file-filter='All files | *' 2>/dev/null",
                                            status);
        return status == 0 ? path : "";
    }
    if (hasCommand("kdialog")) {
        const std::string path =
            runAndRead("kdialog --title '" + title + "' --getopenfilename . '" + patterns.substr(1) + "|" + kind +
                           "' 2>/dev/null",
                       status);
        return status == 0 ? path : "";
    }
    unavailable = true;
    return "";
#endif
}

#endif

std::string openGameDialog(SDL_Window* parent, bool& unavailable) {
    return openFileDialog(parent, "Open Game", "Games", gameExtensions(), unavailable);
}
