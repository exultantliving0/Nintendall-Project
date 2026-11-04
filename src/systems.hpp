#pragma once
#include <string>
#include <vector>

// Every system Nintendall knows how to recognize.
enum class System {
    Unknown,
    NES,
    FDS,
    SNES,
    N64,
    GB,
    GBC,
    GBA,
    NDS,
    GameCube,
    Wii,
    WiiU,
    N3DS,
};

struct SystemInfo {
    System id;
    const char* key;       // short id used in file names, e.g. "nes"
    const char* name;      // short display name, e.g. "NES"
    const char* fullName;  // long display name
    std::vector<std::string> extensions;  // lowercase, including the dot
    const char* core;      // the bundled libretro core that runs this system
    const char* emulator;  // its display name
    bool needsGpu;         // core draws with the GPU (not supported yet)
    const char* note;      // extra requirement (BIOS, keys...) or nullptr
};

const SystemInfo& systemInfo(System s);
const std::vector<System>& allSystems();
System systemFromExtension(const std::string& ext);  // ext lowercase with dot
bool isKnownGameExtension(const std::string& ext);
