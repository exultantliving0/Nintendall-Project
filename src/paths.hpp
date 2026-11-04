#pragma once
#include <filesystem>
#include <string>
#include <vector>

// Where Nintendall keeps things on disk.
struct Paths {
    std::filesystem::path base;         // folder containing the executable
    std::filesystem::path data;         // user data (saves, states, config...)
    std::filesystem::path saves;        // in-game battery saves (.srm)
    std::filesystem::path states;       // save states
    std::filesystem::path system;       // BIOS / firmware / keys for cores
    std::filesystem::path config;       // per-core option files
    std::filesystem::path screenshots;
    std::filesystem::path cache;        // temporary files (games extracted from zips)
    std::vector<std::filesystem::path> coreDirs;  // where the bundled emulator cores live
    std::filesystem::path bundledSystem;          // read-only data shipped for some cores (Dolphin's Sys folder)

    void init();
    // Full path of an installed core, or empty if it isn't installed.
    std::filesystem::path findCore(const std::string& coreName) const;
    // The "system" folder a core should use: the bundled one if it needs our shipped data, else the user's.
    std::filesystem::path systemDirFor(const std::string& coreName) const;
};

// File name of a libretro core for this platform, e.g. "mesen_libretro.so".
std::string coreFileName(const std::string& coreName);
