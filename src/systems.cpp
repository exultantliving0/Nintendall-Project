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
         {".z64", ".n64", ".v64", ".ndd"}, "mupen64plus_next", "Mupen64Plus-Next", true, nullptr},
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
         "azahar", "Azahar", true, "Needs decrypted games"},
    };
    return t;
}

const SystemInfo& systemInfo(System s) {
    for (const auto& i : table())
        if (i.id == s) return i;
    return table()[0];
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
