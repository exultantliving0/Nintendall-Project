#include "systems.hpp"

// One bundled emulator per system. Every core here has a license that allows
// commercial distribution (GPL, MPL or MIT); see THIRD_PARTY_LICENSES.md.
// Keep in sync with tools/bundle_cores.py.
static const std::vector<SystemInfo>& table() {
    static const std::vector<SystemInfo> t = {
        {System::Unknown, "unknown", "?", "Unknown", {}, "", "", false, nullptr},
        {System::NES, "nes", "NES", "Nintendo Entertainment System",
         {".nes", ".unf", ".unif"}, "mesen", "Mesen", false, nullptr},
        {System::FDS, "fds", "FDS", "Famicom Disk System",
         {".fds"}, "mesen", "Mesen", false, "Needs disksys.rom in the system folder"},
        {System::SNES, "snes", "SNES", "Super Nintendo",
         {".sfc", ".smc", ".swc", ".fig", ".bs"}, "bsnes", "bsnes", false, nullptr},
        {System::N64, "n64", "N64", "Nintendo 64",
         {".z64", ".n64", ".v64", ".ndd"}, "mupen64plus_next", "Mupen64Plus-Next", false, nullptr},
        {System::GB, "gb", "GB", "Game Boy",
         {".gb"}, "sameboy", "SameBoy", false, nullptr},
        {System::GBC, "gbc", "GBC", "Game Boy Color",
         {".gbc", ".cgb"}, "sameboy", "SameBoy", false, nullptr},
        {System::GBA, "gba", "GBA", "Game Boy Advance",
         {".gba", ".agb"}, "mgba", "mGBA", false, nullptr},
        {System::NDS, "nds", "NDS", "Nintendo DS",
         {".nds", ".dsi", ".srl", ".ids"}, "melondsds", "melonDS DS", false, nullptr},
        {System::GameCube, "gamecube", "GC", "GameCube",
         {".gcm", ".gcz", ".tgc", ".dol", ".rvz", ".wia", ".ciso", ".iso", ".elf"}, "dolphin", "Dolphin", true, nullptr},
        {System::Wii, "wii", "Wii", "Wii",
         {".wbfs", ".wad"}, "dolphin", "Dolphin", true, nullptr},
        {System::WiiU, "wiiu", "WiiU", "Wii U",
         {".wud", ".wux", ".wua", ".rpx", ".wuhb"}, "cemu", "Cemu", true, "Needs your own console keys (keys.txt)"},
        {System::N3DS, "3ds", "3DS", "Nintendo 3DS",
         {".3ds", ".cci", ".cxi", ".cia", ".3dsx", ".app", ".zcci", ".zcxi", ".zcia", ".z3dsx"},
         "azahar", "Azahar", false, "Needs decrypted games"},
    };
    return t;
}

const SystemInfo& systemInfo(System s) {
    for (const auto& i : table())
        if (i.id == s) return i;
    return table()[0];
}

// RetroPad ids (see libretro.h): B=0 Y=1 Select=2 Start=3 Up=4 Down=5 Left=6 Right=7
// A=8 X=9 L=10 R=11 L2=12 R2=13 L3=14 R3=15
namespace {
constexpr int kB = 0, kY = 1, kSelect = 2, kStart = 3, kUp = 4, kDown = 5, kLeft = 6, kRight = 7, kA = 8, kX = 9,
              kL = 10, kR = 11, kL2 = 12, kR2 = 13, kL3 = 14, kR3 = 15;
}

const std::vector<ButtonLabel>& systemButtons(System s) {
    static const std::vector<ButtonLabel> nes = {
        {kUp, "Up"}, {kDown, "Down"}, {kLeft, "Left"}, {kRight, "Right"},
        {kA, "A"},   {kB, "B"},       {kStart, "Start"}, {kSelect, "Select"},
    };
    static const std::vector<ButtonLabel> fds = {
        {kUp, "Up"}, {kDown, "Down"},   {kLeft, "Left"},     {kRight, "Right"},
        {kA, "A"},   {kB, "B"},         {kStart, "Start"},   {kSelect, "Select"},
        {kR, "Flip Disk Side"},
    };
    static const std::vector<ButtonLabel> gba = {
        {kUp, "Up"}, {kDown, "Down"}, {kLeft, "Left"},   {kRight, "Right"},
        {kA, "A"},   {kB, "B"},       {kL, "L"},         {kR, "R"},
        {kStart, "Start"}, {kSelect, "Select"},
    };
    static const std::vector<ButtonLabel> snes = {
        {kUp, "Up"}, {kDown, "Down"}, {kLeft, "Left"}, {kRight, "Right"},
        {kA, "A"},   {kB, "B"},       {kX, "X"},       {kY, "Y"},
        {kL, "L"},   {kR, "R"},       {kStart, "Start"}, {kSelect, "Select"},
    };
    static const std::vector<ButtonLabel> n3ds = {
        {kUp, "D-Pad Up"}, {kDown, "D-Pad Down"}, {kLeft, "D-Pad Left"}, {kRight, "D-Pad Right"},
        {16, "Circle Pad Up"}, {17, "Circle Pad Down"}, {18, "Circle Pad Left"}, {19, "Circle Pad Right"},
        {kA, "A"}, {kB, "B"}, {kX, "X"}, {kY, "Y"},
        {kL, "L"}, {kR, "R"}, {kL2, "ZL"}, {kR2, "ZR"},
        {kStart, "Start"}, {kSelect, "Select"}, {kL3, "Home"},
    };
    // Mupen64Plus-Next: RetroPad B = N64 A, Y = N64 B, L2 = Z, right stick = C buttons.
    static const std::vector<ButtonLabel> n64 = {
        {kUp, "D-Pad Up"}, {kDown, "D-Pad Down"}, {kLeft, "D-Pad Left"}, {kRight, "D-Pad Right"},
        {16, "Control Stick Up"}, {17, "Control Stick Down"}, {18, "Control Stick Left"}, {19, "Control Stick Right"},
        {kB, "A"}, {kY, "B"}, {kL2, "Z"}, {kL, "L"}, {kR, "R"}, {kStart, "Start"},
        {20, "C-Up"}, {21, "C-Down"}, {22, "C-Left"}, {23, "C-Right"},
    };
    static const std::vector<ButtonLabel> all = {
        {kUp, "Up"}, {kDown, "Down"}, {kLeft, "Left"}, {kRight, "Right"},
        {kA, "A"},   {kB, "B"},       {kX, "X"},       {kY, "Y"},
        {kL, "L"},   {kR, "R"},       {kL2, "L2"},     {kR2, "R2"},
        {kL3, "L3"}, {kR3, "R3"},     {kStart, "Start"}, {kSelect, "Select"},
    };
    switch (s) {
    case System::NES:
    case System::GB:
    case System::GBC: return nes;
    case System::FDS: return fds;
    case System::GBA: return gba;
    case System::SNES:
    case System::NDS: return snes;
    case System::N3DS: return n3ds;
    case System::N64: return n64;
    default: return all;  // no game yet, or systems that aren't playable yet
    }
}

int systemPlayers(System s) {
    switch (s) {
    case System::NES:
    case System::FDS:
    case System::SNES: return 2;
    case System::GB:
    case System::GBC:
    case System::GBA:
    case System::NDS:
    case System::N3DS:
    case System::WiiU: return 1;
    default: return 4;
    }
}

const std::vector<System>& allSystems() {
    static const std::vector<System> all = {
        System::NES, System::FDS, System::SNES, System::N64,      System::GB,  System::GBC,
        System::GBA, System::NDS, System::GameCube, System::Wii, System::WiiU, System::N3DS,
    };
    return all;
}

System systemFromExtension(const std::string& ext) {
    for (const auto& i : table())
        for (const auto& e : i.extensions)
            if (e == ext) return i.id;
    return System::Unknown;
}

bool isKnownGameExtension(const std::string& ext) {
    if (ext == ".zip") return true;
    return systemFromExtension(ext) != System::Unknown;
}
