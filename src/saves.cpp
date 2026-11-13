#include "saves.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>

#include "library.hpp"

namespace fs = std::filesystem;

namespace {

std::string lowerCase(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool readAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// Copies `src` into `ram` at `offset` (as much as fits).
void put(std::vector<uint8_t>& ram, size_t offset, const std::vector<uint8_t>& src) {
    if (offset >= ram.size()) return;
    const size_t n = std::min(src.size(), ram.size() - offset);
    std::copy(src.begin(), src.begin() + std::ptrdiff_t(n), ram.begin() + std::ptrdiff_t(offset));
}

// The N64 emulator's save memory holds every kind of N64 save, one after the other (the same
// layout as RetroArch's .srm): EEPROM, the four Controller Paks, SRAM, FlashRAM.
constexpr size_t kN64Eeprom = 0, kN64Mempak = 0x800, kN64MempakSize = 0x8000, kN64Sram = 0x20800,
                 kN64Flash = 0x28800, kN64Total = 0x48800;

// DeSmuME's .dsv: the save, then a footer that starts with this line.
const char kDesmumeFooter[] = "|<--Snip above here to create a raw sav by excluding this DeSmuME savedata footer:";

}  // namespace

void createSaveFolders(const fs::path& savesRoot) {
    std::error_code ec;
    for (const HomeConsole& c : homeConsoles()) fs::create_directories(savesRoot / fs::u8path(c.folder), ec);
}

fs::path savesFolderFor(const fs::path& savesRoot, const fs::path& gamesRoot, const fs::path& game, System system) {
    std::error_code ec;
    const fs::path rel = game.empty() || gamesRoot.empty() ? fs::path() : fs::relative(game, gamesRoot, ec);
    if (!ec && !rel.empty() && *rel.begin() != ".." && std::distance(rel.begin(), rel.end()) >= 2)
        for (const HomeConsole& c : homeConsoles())
            if (lowerCase(rel.begin()->u8string()) == lowerCase(c.folder)) return savesRoot / fs::u8path(c.folder);
    if (system == System::FDS) system = System::NES;  // Famicom Disk System games go with the NES ones
    for (const HomeConsole& c : homeConsoles())
        if (c.system == system) return savesRoot / fs::u8path(c.folder);
    return savesRoot;
}

std::vector<fs::path> importSave(const fs::path& dir, const std::string& name, System system, std::vector<uint8_t>& ram) {
    // Other emulators' save files for this console, by extension.
    std::vector<std::string> exts;
    switch (system) {
    case System::NES:
    case System::FDS:
    case System::GB:
    case System::GBC:
    case System::GBA: exts = {".sav", ".sa1"}; break;
    case System::SNES: exts = {".sav"}; break;  // other emulators' .srm is the same as Nintendall's own
    case System::NDS: exts = {".sav", ".dsv"}; break;
    case System::N64: exts = {".eep", ".sra", ".fla", ".mpk", ".mpk1", ".mpk2", ".mpk3", ".mpk4"}; break;
    default: return {};
    }
    std::vector<fs::path> found;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (!e.is_regular_file(ec2)) continue;
        const fs::path& p = e.path();
        if (p.stem().u8string() != name) continue;
        if (std::find(exts.begin(), exts.end(), lowerCase(p.extension().u8string())) != exts.end()) found.push_back(p);
    }
    if (found.empty() || ram.empty()) return {};
    // In the order of `exts`: a .sav is used before a .dsv / .sa1 if both are there.
    auto rank = [&](const fs::path& p) {
        return std::find(exts.begin(), exts.end(), lowerCase(p.extension().u8string())) - exts.begin();
    };
    std::sort(found.begin(), found.end(), [&](const fs::path& a, const fs::path& b) { return rank(a) < rank(b); });

    std::vector<fs::path> used;
    std::vector<uint8_t> data;
    if (system == System::N64) {
        if (ram.size() < kN64Total) return {};
        for (const fs::path& p : found) {
            if (!readAll(p, data) || data.empty()) continue;
            const std::string ext = lowerCase(p.extension().u8string());
            if (ext == ".eep") put(ram, kN64Eeprom, std::vector<uint8_t>(data.begin(), data.begin() + std::ptrdiff_t(std::min<size_t>(data.size(), 0x800))));
            else if (ext == ".sra") put(ram, kN64Sram, std::vector<uint8_t>(data.begin(), data.begin() + std::ptrdiff_t(std::min<size_t>(data.size(), 0x8000))));
            else if (ext == ".fla") put(ram, kN64Flash, std::vector<uint8_t>(data.begin(), data.begin() + std::ptrdiff_t(std::min<size_t>(data.size(), 0x20000))));
            else {
                // .mpk / .mpk1 is player 1's Controller Pak, .mpk2 player 2's...
                const int pak = ext.size() == 5 ? ext[4] - '1' : 0;
                put(ram, kN64Mempak + size_t(pak) * kN64MempakSize,
                    std::vector<uint8_t>(data.begin(), data.begin() + std::ptrdiff_t(std::min(data.size(), kN64MempakSize))));
            }
            used.push_back(p);
        }
        return used;
    }

    // Other consoles: one file.
    const fs::path& p = found.front();
    if (!readAll(p, data) || data.empty()) return {};
    if (lowerCase(p.extension().u8string()) == ".dsv") {
        const auto at = std::search(data.begin(), data.end(), kDesmumeFooter, kDesmumeFooter + std::strlen(kDesmumeFooter));
        data.erase(at, data.end());
    }
    put(ram, 0, data);
    return {p};
}

bool moveToImported(const std::vector<fs::path>& files) {
    bool ok = true;
    for (const fs::path& p : files) {
        std::error_code ec;
        const fs::path dir = p.parent_path() / "Imported";
        fs::create_directories(dir, ec);
        const std::string ext = fs::is_directory(p, ec) ? "" : p.extension().u8string();
        const std::string stem = ext.empty() ? p.filename().u8string() : p.stem().u8string();
        fs::path to = dir / p.filename();
        for (int n = 2; fs::exists(to, ec) && n < 1000; ++n)  // keep earlier ones: "name (2).sav"
            to = dir / fs::u8path(stem + " (" + std::to_string(n) + ")" + ext);
        fs::rename(p, to, ec);
        if (ec) {
            ec.clear();
            fs::copy(p, to, fs::copy_options::recursive, ec);
            if (!ec) fs::remove_all(p, ec);
        }
        if (ec) ok = false;
    }
    return ok;
}

namespace {

bool isHex(const std::string& s, size_t len) {
    return s.size() == len && std::all_of(s.begin(), s.end(), [](char c) { return std::isxdigit(static_cast<unsigned char>(c)) != 0; });
}

bool hasSubfolder(const fs::path& dir, const char* name) {
    std::error_code ec;
    return fs::is_directory(dir / name, ec);
}

// A Wii U game folder's title ID ("00050000101c9400"), from meta/meta.xml.
std::string wiiUTitleId(const fs::path& game) {
    std::vector<uint8_t> data;
    if (!readAll(game / "meta" / "meta.xml", data)) return {};
    const std::string xml(data.begin(), data.end());
    const size_t a = xml.find("<title_id");
    if (a == std::string::npos) return {};
    const size_t b = xml.find('>', a), c = xml.find('<', b == std::string::npos ? a : b);
    if (b == std::string::npos || c == std::string::npos) return {};
    std::string id;
    for (size_t i = b + 1; i < c; ++i)
        if (std::isxdigit(static_cast<unsigned char>(xml[i]))) id += char(std::tolower(static_cast<unsigned char>(xml[i])));
    return id.size() == 16 ? id : std::string();
}

}  // namespace

FolderImport importSaveFolders(const fs::path& dir, System console, const EmulatorSaveFolders& to, const fs::path& game,
                               const std::string& gameId) {
    FolderImport result;
    std::error_code ec;
    const std::string gameName = lowerCase(fs::is_directory(game, ec) ? game.filename().u8string() : game.stem().u8string());
    std::vector<fs::path> folders;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::error_code ec2;
        if (e.is_directory(ec2) && e.path().filename() != "Imported") folders.push_back(e.path());
    }
    std::sort(folders.begin(), folders.end());
    for (const fs::path& folder : folders) {
        const std::string name = folder.filename().u8string(), low = lowerCase(name);
        fs::path target;  // the emulator's folder for this game's save
        fs::path source = folder;
        switch (console) {
        case System::Wii: {
            std::string id4;
            if (isHex(name, 8)) {
                target = to.wiiTitles / low;
                break;
            }
            if ((name.size() == 4 || name.size() == 6) && std::all_of(name.begin(), name.end(), [](char c) {
                    return std::isalnum(static_cast<unsigned char>(c)) != 0;
                }))
                id4 = name.substr(0, 4);
            else if (low == gameName && gameId.size() >= 4)
                id4 = gameId.substr(0, 4);
            if (id4.empty()) continue;
            char hex[16];
            std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x", unsigned(uint8_t(id4[0])), unsigned(uint8_t(id4[1])),
                          unsigned(uint8_t(id4[2])), unsigned(uint8_t(id4[3])));
            target = to.wiiTitles / hex;
            break;
        }
        case System::WiiU: {
            std::string id = isHex(name, 16) ? low.substr(8) : isHex(name, 8) ? low : std::string();
            if (id.empty() && low == gameName) {
                const std::string full = wiiUTitleId(game);
                if (!full.empty()) id = full.substr(8);
            }
            if (id.empty()) continue;
            target = to.cemuSaves / id;
            break;
        }
        case System::N3DS: {
            const std::string id = isHex(name, 16) ? low.substr(8) : isHex(name, 8) ? low : std::string();
            if (id.empty()) continue;
            target = to.n3dsTitles / id;
            break;
        }
        default: return result;
        }
        // Only the save is replaced: Dolphin and Citra keep it in a "data" folder (next to it can be
        // an installed game's own files). A folder with just the files is what goes in "data".
        fs::path into = target;
        if (console == System::Wii || console == System::N3DS) {
            into = target / "data";
            if (hasSubfolder(source, "data")) source /= "data";
            else if (console == System::N3DS) into /= "00000001";
        }

        // Keep the save that's there (in Imported), then put the new one in its place.
        if (fs::exists(into, ec)) {
            fs::path keep = dir / "Imported" / fs::u8path(name + " (before)");
            for (int n = 2; fs::exists(keep, ec) && n < 1000; ++n)
                keep = dir / "Imported" / fs::u8path(name + " (before " + std::to_string(n) + ")");
            fs::create_directories(keep.parent_path(), ec);
            fs::copy(into, keep, fs::copy_options::recursive, ec);
            if (ec) {
                result.problem = "Couldn't keep a copy of the save that was there, so \"" + name + "\" wasn't used";
                continue;
            }
            fs::remove_all(into, ec);
        }
        fs::create_directories(into, ec);
        fs::copy(source, into, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        if (ec) {
            result.problem = "Couldn't copy \"" + name + "\": " + ec.message();
            continue;
        }
        if (!moveToImported({folder})) {
            result.problem = "\"" + name + "\" was used but couldn't be moved to the Imported folder";
            continue;
        }
        result.used.push_back(name);
    }
    return result;
}
