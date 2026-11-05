#include "selfupdate.hpp"

namespace fs = std::filesystem;

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

namespace {

std::wstring widen(const std::string& s) {
    std::wstring w(size_t(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), nullptr, 0)), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), int(s.size()), w.data(), int(w.size()));
    return w;
}

bool launch(const fs::path& exe, const std::wstring& args) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    const std::wstring dir = exe.parent_path().wstring();
    if (!CreateProcessW(exe.wstring().c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi))
        return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
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

bool startInstallingUpdate(const fs::path& setup, const fs::path& programDir, std::string& error) {
    // Copy this program and its DLLs to the temp folder and run that copy as the helper.
    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    const fs::path program = fs::path(self);
    const fs::path helperDir = updateFolder() / "helper";
    std::error_code ec;
    fs::create_directories(helperDir, ec);
    const fs::path helper = helperDir / "nintendall-update.exe";
    fs::copy_file(program, helper, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "Couldn't prepare the update (" + ec.message() + ")";
        return false;
    }
    for (const auto& f : fs::directory_iterator(programDir, ec))
        if (f.path().extension() == ".dll") fs::copy_file(f.path(), helperDir / f.path().filename(), fs::copy_options::overwrite_existing, ec);

    const std::wstring args = L"--finish-update \"" + setup.wstring() + L"\" " + std::to_wstring(GetCurrentProcessId()) +
                              L" \"" + program.wstring() + L"\"";
    if (!launch(helper, args)) {
        error = "Couldn't start the update";
        return false;
    }
    return true;
}

int finishUpdate(const std::string& setup, unsigned long pid, const std::string& program) {
    // 1. Wait for Nintendall to close (its files can't be replaced while it runs).
    if (HANDLE app = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) {
        WaitForSingleObject(app, 60000);
        CloseHandle(app);
    }
    // 2. Run the setup silently. "runas": Windows asks for permission (installing in Program Files).
    const std::wstring file = widen(setup);
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = file.c_str();
    sei.lpParameters = L"/S";
    sei.nShow = SW_SHOWNORMAL;
    if (ShellExecuteExW(&sei) && sei.hProcess) {
        WaitForSingleObject(sei.hProcess, INFINITE);
        CloseHandle(sei.hProcess);
    }
    // 3. Start Nintendall again (the new one, or the old one if the install was refused). It
    //    says which, and removes the temp files.
    launch(fs::path(widen(program)), L"");
    return 0;
}

void removeOldUpdateFiles() {
    std::error_code ec;
    fs::remove_all(updateFolder(), ec);  // a helper that's still closing keeps its file: removed next time
}

#else  // Linux / Mac: no installer, so the new version is downloaded from the release page.

bool canUpdateItself(const fs::path&, bool) { return false; }
fs::path updateFolder() { return fs::temp_directory_path() / "Nintendall-update"; }
bool startInstallingUpdate(const fs::path&, const fs::path&, std::string& error) {
    error = "Updating by itself only works on Windows";
    return false;
}
int finishUpdate(const std::string&, unsigned long, const std::string&) { return 1; }
void removeOldUpdateFiles() {}

#endif
