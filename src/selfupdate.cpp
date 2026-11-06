#include "selfupdate.hpp"

namespace fs = std::filesystem;

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {

// Windows' description of an error code, e.g. "Access is denied. (5)".
std::string windowsError(DWORD code) {
    wchar_t* buf = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   code, 0, reinterpret_cast<wchar_t*>(&buf), 0, nullptr);
    std::string text;
    if (buf) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        text.resize(size_t(n > 0 ? n - 1 : 0));
        WideCharToMultiByte(CP_UTF8, 0, buf, -1, text.data(), n, nullptr, nullptr);
        LocalFree(buf);
    }
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return (text.empty() ? "Windows error" : text) + " (" + std::to_string(code) + ")";
}

}  // namespace

bool canUpdateItself(const fs::path& programDir, bool portable) {
    std::error_code ec;
    return !portable && fs::exists(programDir / "Uninstall.exe", ec);
}

fs::path updateFolder() {
    wchar_t buf[MAX_PATH + 1] = {};
    const DWORD n = GetTempPathW(MAX_PATH + 1, buf);
    return (n > 0 ? fs::path(buf) : fs::temp_directory_path()) / "Nintendall-update";
}

InstallStart startInstallingUpdate(const fs::path& setup, const fs::path& programDir, std::string& error) {
    // /D= must come last and isn't quoted (NSIS rule), even with spaces in the folder.
    std::wstring dir = programDir.wstring();
    while (!dir.empty() && (dir.back() == L'\\' || dir.back() == L'/')) dir.pop_back();
    const std::wstring params = L"/S /RESTART /WAITPID=" + std::to_wstring(GetCurrentProcessId()) + L" /D=" + dir;
    const std::wstring file = setup.wstring();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";  // Windows asks for permission (installing in Program Files)
    sei.lpFile = file.c_str();
    sei.lpParameters = params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei)) return InstallStart::Started;
    const DWORD code = GetLastError();
    if (code == ERROR_CANCELLED) return InstallStart::Refused;
    error = "Couldn't start the update: " + windowsError(code);
    return InstallStart::Failed;
}

void removeOldUpdateFiles() {
    std::error_code ec;
    fs::remove_all(updateFolder(), ec);  // a setup that's still finishing keeps its file: removed next time
}

#else  // Linux / Mac: no installer, so the new version is downloaded from the release page.

bool canUpdateItself(const fs::path&, bool) { return false; }
fs::path updateFolder() { return fs::temp_directory_path() / "Nintendall-update"; }
InstallStart startInstallingUpdate(const fs::path&, const fs::path&, std::string& error) {
    error = "Updating by itself only works on Windows";
    return InstallStart::Failed;
}
void removeOldUpdateFiles() {}

#endif
