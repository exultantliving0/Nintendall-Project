#pragma once
#include <string>

// Checking GitHub for a newer Nintendall (once, when Nintendall starts).
struct UpdateInfo {
    bool available = false;  // a newer version is out
    std::string version;     // its version, e.g. "0.11.0"
    std::string pageUrl;     // its release page (with the download)
    std::string notes;       // the start of its release notes, as plain text
};

// Asks GitHub for the newest release. Blocking (a few seconds at most): run it on a thread.
// Not finding anything (no internet, GitHub busy) just means no update.
UpdateInfo checkForUpdate(const std::string& currentVersion);

// True if version a ("0.11.0", "v0.11.0") is newer than b.
bool isNewerVersion(const std::string& a, const std::string& b);
