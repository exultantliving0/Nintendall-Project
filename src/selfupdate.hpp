#pragma once
#include <filesystem>
#include <string>

// "Update Now" (Windows, installed with setup.exe): the new setup file is downloaded and checked,
// then a helper takes over. The helper is a copy of nintendall.exe (with its DLLs) run from the
// temp folder, so the installed files aren't in use. It waits for Nintendall to close, runs the
// setup silently (Windows asks for permission, as for any install in Program Files), then
// starts Nintendall again.

// True if this copy of Nintendall can update itself (installed on Windows, not portable).
bool canUpdateItself(const std::filesystem::path& programDir, bool portable);
// Where the update is downloaded: %TEMP%\Nintendall-update.
std::filesystem::path updateFolder();
// Starts the helper. On success Nintendall must quit right away.
bool startInstallingUpdate(const std::filesystem::path& setup, const std::filesystem::path& programDir,
                           std::string& error);
// The helper's job (nintendall --finish-update <setup> <pid> <nintendall.exe>).
int finishUpdate(const std::string& setup, unsigned long pid, const std::string& program);
// Deletes what a finished update left in the temp folder (at startup).
void removeOldUpdateFiles();
