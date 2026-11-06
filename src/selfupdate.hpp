#pragma once
#include <filesystem>
#include <string>

// "Update Now" (Windows, installed with setup.exe): the new setup file is downloaded and checked,
// then started with Windows' permission prompt (installing in Program Files needs it):
//   setup.exe /S /RESTART /WAITPID=<Nintendall's process> /D=<install folder>
// Nintendall then closes. The setup waits for it to be gone, installs silently into the same
// folder and starts the new Nintendall (see installer/nintendall.nsi).

// True if this copy of Nintendall can update itself (installed on Windows, not portable).
bool canUpdateItself(const std::filesystem::path& programDir, bool portable);
// Where the update is downloaded: %TEMP%\Nintendall-update.
std::filesystem::path updateFolder();

enum class InstallStart { Started, Refused, Failed };
// Starts the setup. Started: Nintendall must quit right away. Refused: the permission prompt was
// answered No. Failed: `error` says why (with Windows' own reason).
InstallStart startInstallingUpdate(const std::filesystem::path& setup, const std::filesystem::path& programDir,
                                   std::string& error);
// Deletes what a finished update left in the temp folder (at startup).
void removeOldUpdateFiles();
