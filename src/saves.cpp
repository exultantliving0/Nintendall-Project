#include "saves.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <set>

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

std::vector<fs::directory_entry> entriesIn(const fs::path& dir) {
    std::vector<fs::directory_entry> entries;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) entries.push_back(*it);
    return entries;
}

namespace {
const char* const kSavesSub = "Saves";
const char* const kStatesSub = "States";

// Moves a file or folder unless its new place is taken.
bool moveIfFree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec) || ec) return false;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (ec) {
        ec.clear();
        fs::copy(from, to, fs::copy_options::recursive, ec);
        if (ec) {
            std::error_code ec2;
            fs::remove_all(to, ec2);
            return false;
        }
        fs::remove_all(from, ec);
    }
    return true;
}
}  // namespace

void createSaveFolders(const fs::path& savesRoot) {
    std::error_code ec;
    for (const HomeConsole& c : homeConsoles())
        for (const char* sub : {kSavesSub, kStatesSub}) fs::create_directories(savesRoot / fs::u8path(c.folder) / sub, ec);
}

int organizeSaveFolders(const fs::path& savesRoot, const fs::path& oldStates) {
    int moved = 0;
    for (const HomeConsole& c : homeConsoles()) {
        const fs::path console = savesRoot / fs::u8path(c.folder);
        for (const auto& e : entriesIn(console)) {
            const std::string name = e.path().filename().u8string();
            if (name == kSavesSub || name == kStatesSub || name == "desktop.ini" || name == ".DS_Store") continue;
            if (moveIfFree(e.path(), console / kSavesSub / e.path().filename())) ++moved;
        }
    }
    // "<game>.<key>.state<slot>" -> Saves/<console>/States/<game>.state<slot>
    for (const auto& e : entriesIn(oldStates)) {
        std::error_code ec;
        if (!e.is_regular_file(ec)) continue;
        const std::string file = e.path().filename().u8string();
        const size_t at = file.rfind(".state");
        if (at == std::string::npos || at == 0) continue;
        const size_t dot = file.rfind('.', at - 1);
        if (dot == std::string::npos || dot == 0) continue;
        const std::string key = file.substr(dot + 1, at - dot - 1);
        System system = System::Unknown;
        for (System s : allSystems())
            if (key == systemInfo(s).key) system = s;
        if (system == System::Unknown) continue;
        const fs::path to = statesFolderFor(savesRoot, {}, {}, system) / fs::u8path(file.substr(0, dot) + file.substr(at));
        if (moveIfFree(e.path(), to)) ++moved;
    }
    return moved;
}

fs::path consoleFolderFor(const fs::path& savesRoot, const fs::path& gamesRoot, const fs::path& game, System system) {
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

fs::path savesFolderFor(const fs::path& savesRoot, const fs::path& gamesRoot, const fs::path& game, System system) {
    return consoleFolderFor(savesRoot, gamesRoot, game, system) / kSavesSub;
}

fs::path statesFolderFor(const fs::path& savesRoot, const fs::path& gamesRoot, const fs::path& game, System system) {
    return consoleFolderFor(savesRoot, gamesRoot, game, system) / kStatesSub;
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
    for (const auto& e : entriesIn(dir)) {
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
    for (const auto& e : entriesIn(dir)) {
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
            std::string id = isHex(name, 16) ? low.substr(8) : std::string();  // (8 digits: the sync's own)
            if (id.empty() && low == gameName) {
                const std::string full = wiiUTitleId(game);
                if (!full.empty()) id = full.substr(8);
            }
            if (id.empty()) continue;
            target = to.cemuSaves / id;
            break;
        }
        case System::N3DS: {
            const std::string id = isHex(name, 16) ? low.substr(8) : std::string();  // (8 digits: the sync's own)
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

// ============================================================================
// Wii, Wii U and 3DS saves in the Saves folder (see syncSaveFolders in saves.hpp)

namespace {

// The game ID in a Saves folder's name: "524d4345", or "Mario Kart Wii [524d4345]". "" if none.
std::string idInName(const std::string& name, size_t len) {
    if (isHex(name, len)) return lowerCase(name);
    if (name.size() >= len + 2 && name.back() == ']' && name[name.size() - len - 2] == '[') {
        const std::string id = name.substr(name.size() - len - 1, len);
        if (isHex(id, len)) return lowerCase(id);
    }
    return {};
}

// Every file under dir (relative path -> full path).
std::map<std::string, fs::path> filesIn(const fs::path& dir) {
    std::map<std::string, fs::path> files;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code ec2;
        if (it->is_regular_file(ec2)) files[fs::relative(it->path(), dir, ec2).generic_u8string()] = it->path();
    }
    return files;
}

bool sameFiles(const fs::path& a, const fs::path& b) {
    const auto fa = filesIn(a), fb = filesIn(b);
    if (fa.size() != fb.size()) return false;
    std::vector<uint8_t> x, y;
    for (const auto& [rel, path] : fa) {
        const auto it = fb.find(rel);
        std::error_code ec;
        if (it == fb.end() || fs::file_size(path, ec) != fs::file_size(it->second, ec)) return false;
        if (!readAll(path, x) || !readAll(it->second, y) || x != y) return false;
    }
    return true;
}

// (min(), not {}: the file clock's zero can be in the future, so today's times are below it.)
fs::file_time_type newestFile(const fs::path& dir) {
    fs::file_time_type newest = fs::file_time_type::min();
    for (const auto& [rel, path] : filesIn(dir)) {
        std::error_code ec;
        const auto t = fs::last_write_time(path, ec);
        if (!ec && t > newest) newest = t;
    }
    return newest;
}

// Replaces `to` with a copy of `from` (the files keep their times, so the two compare as equal).
bool copyFolder(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    const fs::path temp = to.u8string() + ".copying";
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    fs::copy(from, temp, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) {
        fs::remove_all(temp, ec);
        return false;
    }
    for (const auto& [rel, path] : filesIn(from)) {
        std::error_code ec2;
        const auto t = fs::last_write_time(path, ec2);
        if (!ec2) fs::last_write_time(temp / fs::u8path(rel), t, ec2);
    }
    fs::remove_all(to, ec);
    if (ec) {
        fs::remove_all(temp, ec);
        return false;
    }
    fs::create_directories(to.parent_path(), ec);
    fs::rename(temp, to, ec);
    return !ec;
}

std::string folderName(std::string name) {
    std::string out;
    for (const char c : name) out += std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32 ? ' ' : c;
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) out.pop_back();
    return out;
}

}  // namespace

std::string saveFolderId(System console, const fs::path& game, const std::string& gameId) {
    if (console == System::Wii && gameId.size() >= 4) {
        char hex[16];
        std::snprintf(hex, sizeof hex, "%02x%02x%02x%02x", unsigned(uint8_t(gameId[0])), unsigned(uint8_t(gameId[1])),
                      unsigned(uint8_t(gameId[2])), unsigned(uint8_t(gameId[3])));
        return hex;
    }
    if (console == System::WiiU) {
        const std::string full = wiiUTitleId(game);
        if (!full.empty()) return full.substr(8);
    }
    return {};
}

SyncCount syncSaveFolders(const fs::path& savesDir, const SaveFolderSync& emu, const std::string& currentId,
                          const std::string& currentName, const std::map<std::string, std::string>& names) {
    SyncCount count;
    std::error_code ec;
    // The games' folders on each side, by ID.
    std::map<std::string, fs::path> inEmu, inSaves;
    std::vector<fs::path> emuFolders{emu.emulator};
    emuFolders.insert(emuFolders.end(), emu.alsoIn.begin(), emu.alsoIn.end());
    for (const fs::path& folder : emuFolders)
        for (const auto& e : entriesIn(folder)) {
            std::error_code ec2;
            const std::string name = e.path().filename().u8string();
            if (e.is_directory(ec2) && isHex(name, emu.idLength) &&
                (emu.inner.empty() || fs::is_directory(e.path() / emu.inner, ec2)) && !inEmu.count(lowerCase(name)))
                inEmu[lowerCase(name)] = e.path();
        }
    for (const auto& e : entriesIn(savesDir)) {
        std::error_code ec2;
        const std::string id = e.is_directory(ec2) ? idInName(e.path().filename().u8string(), emu.idLength) : "";
        if (!id.empty() && !inSaves.count(id)) inSaves[id] = e.path();
    }
    std::set<std::string> ids;
    for (const auto& [id, p] : inEmu) ids.insert(id);
    for (const auto& [id, p] : inSaves) ids.insert(id);

    const std::string current = lowerCase(currentId);
    for (const std::string& id : ids) {
        // The Saves folder: named with the game's name too when it's the game being played, or
        // when it's known and the folder has just the ID.
        fs::path saved = inSaves.count(id) ? inSaves[id] : fs::path();
        const auto known = names.find(id);
        const std::string knownName = known == names.end() ? "" : folderName(known->second);
        std::string wanted = saved.empty() ? id : saved.filename().u8string();
        if (id == current && !folderName(currentName).empty()) wanted = folderName(currentName) + " [" + id + "]";
        else if (!knownName.empty() && lowerCase(wanted) == id) wanted = knownName + " [" + id + "]";
        if (!saved.empty() && saved.filename().u8string() != wanted && !fs::exists(savesDir / fs::u8path(wanted), ec)) {
            std::error_code ec2;
            fs::rename(saved, savesDir / fs::u8path(wanted), ec2);
            if (!ec2) saved = savesDir / fs::u8path(wanted);
        }
        if (saved.empty()) saved = savesDir / fs::u8path(wanted);
        const fs::path emuGame = inEmu.count(id)                            ? inEmu[id]
                                 : id == current && !emu.currentIn.empty() ? emu.currentIn / id
                                                                           : emu.emulator / id;
        const fs::path a = emu.inner.empty() ? emuGame : emuGame / emu.inner;
        const fs::path b = emu.inner.empty() ? saved : saved / emu.inner;
        // A side counts only with save files in it (an empty folder isn't a save).
        const bool haveA = fs::is_directory(a, ec) && !filesIn(a).empty();
        const bool haveB = fs::is_directory(b, ec) && !filesIn(b).empty();
        if (!haveA && !haveB) continue;
        if (haveA && haveB && sameFiles(a, b)) continue;
        // The newer copy wins; on a tie the emulator's (the one the game used).
        if (haveA && (!haveB || newestFile(a) >= newestFile(b))) {
            if (copyFolder(a, b)) ++count.toSaves;
        } else if (haveB) {
            if (copyFolder(b, a)) ++count.toEmulator;
        }
    }
    return count;
}
