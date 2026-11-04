#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

#include "systems.hpp"

// Result of looking at a file (or folder) and working out which system it's for.
struct Detection {
    System system = System::Unknown;
    bool fromHeader = false;  // true = proven by the file's contents, false = guessed from extension/folder
    std::string detail;       // human readable reason, e.g. "iNES header"
    std::string zipEntry;     // if the game is inside a .zip, the entry to extract
    std::filesystem::path launchPath;  // file to hand to the core (differs for Wii U folders)
};

Detection detectGame(const std::filesystem::path& path);

// Lowercase file extension including the dot (".nes").
std::string lowerExt(const std::filesystem::path& p);
