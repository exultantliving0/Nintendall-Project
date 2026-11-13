#include "library.hpp"

#include <SDL.h>
#include <zlib.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>

#include "detect.hpp"
#include "http.hpp"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#endif

namespace fs = std::filesystem;

const std::vector<HomeConsole>& homeConsoles() {
    // Release order (first release anywhere). Famicom Disk System games go in the NES folder.
    static const std::vector<HomeConsole> list = {
        {System::NES, "NES", "Nintendo Entertainment System", "NES", "NES"},                // 1983
        {System::GB, "Game Boy", "Game Boy", "Game Boy", "GB"},                             // 1989
        {System::SNES, "SNES", "Super Nintendo", "SNES", "SNES"},                           // 1990
        {System::N64, "Nintendo 64", "Nintendo 64", "N64", "N64"},                          // 1996
        {System::GBC, "Game Boy Color", "Game Boy Color", "GB Color", "GBC"},               // 1998
        {System::GBA, "Game Boy Advance", "Game Boy Advance", "GB Advance", "GBA"},         // 2001
        {System::GameCube, "GameCube", "GameCube", "GameCube", "GC"},                       // 2001
        {System::NDS, "Nintendo DS", "Nintendo DS", "DS", "DS"},                            // 2004
        {System::Wii, "Wii", "Wii", "Wii", "Wii"},                                          // 2006
        {System::N3DS, "Nintendo 3DS", "Nintendo 3DS", "3DS", "3DS"},                       // 2011
        {System::WiiU, "Wii U", "Wii U", "Wii U", "Wii U"},                                 // 2012
    };
    return list;
}

fs::path defaultGamesFolder(const fs::path& programDir, bool portable) {
    if (portable) return programDir / "Games";
    fs::path docs;
#if defined(_WIN32)
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_PERSONAL, nullptr, 0, buf))) docs = fs::path(buf);
#else
    if (const char* home = std::getenv("HOME")) {
        std::error_code ec;
        docs = fs::u8path(home);
        if (fs::is_directory(docs / "Documents", ec)) docs /= "Documents";
    }
#endif
    if (docs.empty()) docs = programDir;
    return docs / "Nintendall" / "Games";
}

void createGameFolders(const fs::path& root) {
    std::error_code ec;
    for (const HomeConsole& c : homeConsoles()) fs::create_directories(root / fs::u8path(c.folder), ec);
}

void openFolder(const fs::path& dir) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", dir.wstring().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string url = "file://";
    for (const char c : dir.u8string()) url += c == '/' ? "/" : urlEncode(std::string(1, c));
    SDL_OpenURL(url.c_str());
#endif
}

// ============================================================================
// Titles

namespace {

std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && (std::isspace(static_cast<unsigned char>(s[a])) || s[a] == '-')) ++a;
    while (b > a && (std::isspace(static_cast<unsigned char>(s[b - 1])) || s[b - 1] == '-' || s[b - 1] == ',')) --b;
    return s.substr(a, b - a);
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

bool isIdChar(char c) { return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }

bool validId(const std::string& id, size_t len) {
    return id.size() == len && std::all_of(id.begin(), id.end(), isIdChar);
}

}  // namespace

std::string cleanTitle(const std::string& fileName) {
    std::string s;
    int depth = 0;
    for (const char c : fileName) {
        if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && depth > 0) --depth;
        else if (depth == 0) s += c;
    }
    if (s.find(' ') == std::string::npos) std::replace(s.begin(), s.end(), '_', ' ');
    // Collapse runs of spaces.
    std::string t;
    for (const char c : s)
        if (!(c == ' ' && !t.empty() && t.back() == ' ')) t += c;
    t = trim(t);
    // "Legend of Zelda, The - A Link to the Past" -> "The Legend of Zelda - A Link to the Past"
    for (const char* article : {"The", "A", "An"}) {
        const std::string tag = std::string(", ") + article;
        const size_t p = t.find(tag);
        if (p == std::string::npos || p == 0) continue;
        const size_t end = p + tag.size();
        if (end != t.size() && t.compare(end, 3, " - ") != 0 && t[end] != ':') continue;
        t = std::string(article) + " " + t.substr(0, p) + t.substr(end);
        break;
    }
    return t.empty() ? fileName : t;
}

int discInName(const std::string& name, std::string* rest) {
    const std::string low = lower(name);
    for (size_t at = 0; (at = low.find("dis", at)) != std::string::npos; ++at) {
        if (at + 4 > low.size() || (low[at + 3] != 'c' && low[at + 3] != 'k')) continue;
        if (at > 0 && std::isalnum(static_cast<unsigned char>(low[at - 1]))) continue;  // "Indisc..."
        size_t p = at + 4;
        while (p < low.size() && (low[p] == ' ' || low[p] == '_' || low[p] == '-')) ++p;
        size_t q = p;
        while (q < low.size() && q - p < 2 && std::isdigit(static_cast<unsigned char>(low[q]))) ++q;
        if (q == p || (q < low.size() && std::isalnum(static_cast<unsigned char>(low[q])))) continue;
        const int n = std::atoi(low.substr(p, q - p).c_str());
        if (n < 1 || n > 9) continue;
        if (rest) {
            // "of 2" after the number belongs to the tag too.
            size_t end = q;
            if (low.compare(end, 4, " of ") == 0 && end + 4 < low.size() && std::isdigit(static_cast<unsigned char>(low[end + 4])))
                end += 5;
            size_t start = at;
            if (start > 0 && (low[start - 1] == '(' || low[start - 1] == '[') && end < low.size() &&
                (low[end] == ')' || low[end] == ']')) {
                --start;  // "(Disc 2)": the brackets too
                ++end;
            }
            while (start > 0 && (name[start - 1] == ' ' || name[start - 1] == '-' || name[start - 1] == '_')) --start;
            *rest = name.substr(0, start) + name.substr(end);
        }
        return n;
    }
    return 0;
}

std::vector<fs::path> gameDiscs(const fs::path& file, System console) {
    Library library;
    const fs::path want = file.lexically_normal();
    for (const LibraryGame& g : library.scan(file.parent_path(), console)) {
        if (g.discs.empty()) continue;
        for (size_t i = 0; i < g.discs.size(); ++i)
            if (g.discs[i].lexically_normal() == want) {
                std::vector<fs::path> list = g.discs;  // the one picked first, then the next ones
                std::rotate(list.begin(), list.begin() + std::ptrdiff_t(i), list.end());
                return list;
            }
    }
    return {file};
}

// ============================================================================
// Game IDs and names inside the game files

namespace {

uint32_t be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint32_t le32(const uint8_t* p) { return uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]; }
uint64_t le64(const uint8_t* p) { return uint64_t(le32(p + 4)) << 32 | le32(p); }

std::vector<uint8_t> readAt(std::ifstream& f, uint64_t off, size_t len) {
    std::vector<uint8_t> out(len);
    f.clear();
    f.seekg(std::streamoff(off));
    f.read(reinterpret_cast<char*>(out.data()), std::streamsize(len));
    out.resize(size_t(std::max<std::streamsize>(0, f.gcount())));
    return out;
}

// GameCube / Wii disc header: 6-character ID at 0, game name at 0x20.
bool discHeader(const std::vector<uint8_t>& h, std::string& id, std::string& name, int& disc) {
    if (h.size() < 0x60) return false;
    const bool wii = be32(&h[0x18]) == 0x5D1C9EA3, gc = be32(&h[0x1C]) == 0xC2339F3D;
    if (!wii && !gc) return false;
    id.assign(reinterpret_cast<const char*>(h.data()), 6);
    if (!validId(id, 6)) {
        id.clear();
        return false;
    }
    name.clear();
    for (size_t i = 0x20; i < h.size() && i < 0x60 && h[i]; ++i) name += char(h[i]);
    disc = h[6] < 8 ? h[6] + 1 : 0;  // byte 6: the disc number (0 = disc 1)
    return true;
}

void gameCubeWiiId(std::ifstream& f, std::string& id, std::string& name, int& discNo) {
    const std::vector<uint8_t> head = readAt(f, 0, 0x60);
    if (head.size() < 0x20) return;
    if (discHeader(head, id, name, discNo)) return;  // plain .iso / .gcm
    if (std::memcmp(head.data(), "WBFS", 4) == 0) {
        const unsigned shift = head[8];
        if (shift >= 9 && shift <= 16) discHeader(readAt(f, 1u << shift, 0x60), id, name, discNo);
    } else if (std::memcmp(head.data(), "WIA\x01", 4) == 0 || std::memcmp(head.data(), "RVZ\x01", 4) == 0) {
        discHeader(readAt(f, 0x58, 0x60), id, name, discNo);  // a copy of the disc header is stored at 0x58
    } else if (std::memcmp(head.data(), "CISO", 4) == 0) {
        discHeader(readAt(f, 0x8000, 0x60), id, name, discNo);
    } else if (le32(head.data()) == 0xB10BC001) {  // GCZ: zlib-compressed blocks
        const uint32_t blocks = le32(&head[0x1C]);
        if (blocks == 0 || blocks > (1u << 24)) return;
        const std::vector<uint8_t> ptrs = readAt(f, 0x20, 16);
        if (ptrs.size() < 16) return;
        constexpr uint64_t kRaw = 1ull << 63;
        const uint64_t p0 = le64(&ptrs[0]), p1 = blocks > 1 ? le64(&ptrs[8]) & ~kRaw : le64(&head[8]);
        const uint64_t start = 0x20 + uint64_t(blocks) * 12 + (p0 & ~kRaw);
        if (p0 & kRaw) {
            discHeader(readAt(f, start, 0x60), id, name, discNo);
            return;
        }
        const uint64_t size = std::min<uint64_t>(p1 - (p0 & ~kRaw), 1u << 20);
        std::vector<uint8_t> packed = readAt(f, start, size_t(size)), disc(0x60);
        z_stream z{};
        if (inflateInit(&z) != Z_OK) return;
        z.next_in = packed.data();
        z.avail_in = uInt(packed.size());
        z.next_out = disc.data();
        z.avail_out = uInt(disc.size());
        inflate(&z, Z_SYNC_FLUSH);
        inflateEnd(&z);
        if (z.avail_out == 0) discHeader(disc, id, name, discNo);
    }
}

// 3DS: the product code ("CTR-P-AREE") in the first NCCH partition's header; the ID is its last part.
std::string n3dsId(std::ifstream& f) {
    const std::vector<uint8_t> head = readAt(f, 0, 0x200);
    if (head.size() < 0x200) return {};
    uint64_t ncch = 0;
    if (std::memcmp(&head[0x100], "NCSD", 4) == 0) {
        ncch = uint64_t(le32(&head[0x120])) * 0x200;
    } else if (std::memcmp(&head[0x100], "NCCH", 4) == 0) {
        ncch = 0;
    } else if (le32(head.data()) == 0x2020) {  // CIA: header, certificates, ticket, TMD, then the content
        auto align = [](uint64_t x) { return (x + 63) & ~uint64_t(63); };
        ncch = align(0x2020) + align(le32(&head[8])) + align(le32(&head[0xC])) + align(le32(&head[0x10]));
    } else {
        return {};
    }
    const std::vector<uint8_t> h = readAt(f, ncch, 0x160);
    if (h.size() < 0x160 || std::memcmp(&h[0x100], "NCCH", 4) != 0) return {};
    std::string code;
    for (size_t i = 0x150; i < 0x160 && h[i]; ++i) code += char(h[i]);
    const size_t dash = code.rfind('-');
    const std::string id = dash == std::string::npos ? code : code.substr(dash + 1);
    return validId(id, 4) ? id : std::string();
}

std::string xmlTag(const std::string& xml, const std::string& tag) {
    const size_t a = xml.find("<" + tag);
    if (a == std::string::npos) return {};
    const size_t b = xml.find('>', a), c = xml.find("</" + tag, a);
    if (b == std::string::npos || c == std::string::npos || c < b) return {};
    std::string v = xml.substr(b + 1, c - b - 1), out;
    // The few entities Wii U titles use; line breaks become spaces.
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '&') {
            const size_t semi = v.find(';', i);
            if (semi != std::string::npos) {
                const std::string ent = v.substr(i, semi - i + 1);
                const char* rep = ent == "&amp;" ? "&" : ent == "&lt;" ? "<" : ent == "&gt;" ? ">" :
                                  ent == "&quot;" ? "\"" : ent == "&apos;" ? "'" : " ";
                out += rep;
                i = semi;
                continue;
            }
        }
        out += (v[i] == '\n' || v[i] == '\r') ? ' ' : v[i];
    }
    std::string t;
    for (const char ch : out)
        if (!(ch == ' ' && !t.empty() && t.back() == ' ')) t += ch;
    return trim(t);
}

// "SUPER MARIO GALAXY 2" -> "Super Mario Galaxy 2" (names on discs are often all capitals).
std::string titleCase(std::string s) {
    if (std::any_of(s.begin(), s.end(), [](char c) { return c >= 'a' && c <= 'z'; })) return s;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i < s.size() && std::isalnum(static_cast<unsigned char>(s[i]))) continue;
        const std::string word = s.substr(start, i - start);
        const bool roman = !word.empty() && word.find_first_not_of("IVX") == std::string::npos;
        if (!roman)
            for (size_t j = start + 1; j < i; ++j) s[j] = char(std::tolower(static_cast<unsigned char>(s[j])));
        start = i + 1;
    }
    return s;
}

// The console family a file extension belongs to: only those files are listed in a console's folder.
int family(System s) {
    switch (s) {
    case System::FDS: return int(System::NES);
    case System::GBC: return int(System::GB);
    case System::Wii: return int(System::GameCube);
    default: return int(s);
    }
}

bool isWiiUFolder(const fs::path& dir) {
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(dir / "code", ec))
        if (lowerExt(f.path()) == ".rpx") return true;
    return false;
}

// An update or DLC folder (meta/meta.xml's title ID starts 0005000E / 0005000C): not a game of its
// own. Cemu finds it in the Wii U games folder and uses it with the game.
bool isWiiUUpdateOrDlc(const fs::path& dir) {
    std::ifstream f(dir / "meta" / "meta.xml", std::ios::binary);
    if (!f) return false;
    std::string xml(65536, '\0');
    f.read(xml.data(), std::streamsize(xml.size()));
    xml.resize(size_t(std::max<std::streamsize>(0, f.gcount())));
    const std::string id = lower(xmlTag(xml, "title_id"));
    return id.rfind("0005000e", 0) == 0 || id.rfind("0005000c", 0) == 0;
}

LibraryGame readGame(const fs::path& path, System console, bool folder) {
    LibraryGame g;
    g.path = path;
    g.system = console;
    g.fileName = folder ? path.filename().u8string() : path.stem().u8string();
    g.title = cleanTitle(g.fileName);

    if (folder) {  // Wii U game folder: meta/meta.xml has the title and product code
        std::ifstream f(path / "meta" / "meta.xml", std::ios::binary);
        std::string xml(65536, '\0');
        f.read(xml.data(), std::streamsize(xml.size()));
        xml.resize(size_t(std::max<std::streamsize>(0, f.gcount())));
        const std::string name = xmlTag(xml, "longname_en");
        if (!name.empty()) g.title = name;
        std::string code = xmlTag(xml, "product_code"), company = xmlTag(xml, "company_code");
        if (code.size() >= 4 && company.size() >= 2) {
            const std::string id = code.substr(code.size() - 4) + company.substr(company.size() - 2);
            if (validId(id, 6)) g.gameId = id;
        }
        return g;
    }

    const std::string ext = lowerExt(path);
    if (ext == ".fds") g.system = System::FDS;
    if (console == System::GB && (ext == ".gbc" || ext == ".cgb")) g.system = System::GBC;
    if (ext == ".zip") return g;

    std::ifstream f(path, std::ios::binary);
    if (!f) return g;
    if (console == System::GameCube || console == System::Wii) {
        std::string name;
        gameCubeWiiId(f, g.gameId, name, g.disc);
        std::string base;
        if (const int n = discInName(g.fileName, &base)) {
            g.disc = n;  // the name says which disc it is
            g.title = cleanTitle(base);
        }
        // Files named after their ID ("RMGE01.wbfs") or "game.iso" get the name from the disc.
        const bool plainName = validId(g.title, 6) || lower(g.title) == "game" || lower(g.title) == "disc";
        const bool printable = !name.empty() && std::all_of(name.begin(), name.end(), [](char c) {
            return c >= 32 && c < 127;
        });
        if (plainName && printable) g.title = titleCase(trim(name));
    } else if (console == System::NDS) {
        const std::vector<uint8_t> h = readAt(f, 0x0C, 4);
        const std::string id(h.begin(), h.end());
        if (validId(id, 4)) g.gameId = id;
    } else if (console == System::N3DS) {
        g.gameId = n3dsId(f);
    }
    return g;
}

}  // namespace

LibraryGame readGameInfo(const fs::path& path, System console) {
    std::error_code ec;
    return readGame(path, console, fs::is_directory(path, ec));
}

std::vector<LibraryGame> Library::scan(const fs::path& folder, System console) {
    std::vector<LibraryGame> games;

    auto add = [&](const fs::path& p, bool isFolder) {
        std::error_code ec;
        const auto time = fs::last_write_time(isFolder ? p / "meta" / "meta.xml" : p, ec);
        const uintmax_t size = isFolder ? 0 : fs::file_size(p, ec);
        const std::string key = p.u8string();
        auto it = known_.find(key);
        if (it == known_.end() || it->second.time != time || it->second.size != size)
            it = known_.insert_or_assign(key, Known{time, size, readGame(p, console, isFolder)}).first;
        games.push_back(it->second.game);
    };

    // Subfolders are searched too (3 levels), so games can be sorted into folders.
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec)) {
            const fs::path& p = e.path();
            if (p.filename().u8string().rfind('.', 0) == 0) continue;  // hidden
            std::error_code ec2;
            if (e.is_directory(ec2)) {
                if (console == System::WiiU && isWiiUUpdateOrDlc(p)) continue;  // used with its game
                if (console == System::WiiU && isWiiUFolder(p)) add(p, true);
                else if (depth < 3) walk(p, depth + 1);
            } else if (e.is_regular_file(ec2)) {
                const std::string ext = lowerExt(p);
                const System s = systemFromExtension(ext);
                if (ext == ".nca") continue;  // pieces of games / firmware (open one with File > Open)
                if (ext == ".zip" || (s != System::Unknown && family(s) == family(console))) add(p, false);
            }
        }
    };
    walk(folder, 0);

    // A game on several discs (GameCube) shows once, with one cover: its discs are found by the
    // game's ID (the same on every disc, with the disc number next to it) or by their names
    // ("(Disc 1)", "(Disc 2)"). Starting it gives the emulator every disc.
    if (console == System::GameCube || console == System::Wii) {
        std::map<std::string, std::vector<size_t>> groups;
        for (size_t i = 0; i < games.size(); ++i) {
            const LibraryGame& g = games[i];
            if (g.disc == 0) continue;
            std::string base;
            if (!g.gameId.empty()) groups["id:" + g.gameId].push_back(i);
            else if (discInName(g.fileName, &base)) groups["name:" + lower((g.path.parent_path() / base).u8string())].push_back(i);
        }
        std::vector<bool> drop(games.size(), false);
        for (auto& [key, list] : groups) {
            std::sort(list.begin(), list.end(), [&](size_t a, size_t b) {
                return games[a].disc != games[b].disc ? games[a].disc < games[b].disc : games[a].path < games[b].path;
            });
            std::vector<size_t> picked;  // one file per disc
            for (const size_t i : list)
                if (picked.empty() || games[picked.back()].disc != games[i].disc) picked.push_back(i);
            if (picked.size() < 2) continue;  // one disc (maybe two copies of it): nothing to join
            LibraryGame& first = games[picked.front()];
            for (const size_t i : picked) {
                first.discs.push_back(games[i].path);
                if (i != picked.front()) drop[i] = true;
            }
            std::string base;
            if (discInName(first.title, &base)) first.title = cleanTitle(base);  // "Game Disc 1" -> "Game"
        }
        std::vector<LibraryGame> kept;
        for (size_t i = 0; i < games.size(); ++i)
            if (!drop[i]) kept.push_back(std::move(games[i]));
        games = std::move(kept);
    }

    std::sort(games.begin(), games.end(), [](const LibraryGame& a, const LibraryGame& b) {
        const std::string x = lower(a.title), y = lower(b.title);
        return x != y ? x < y : a.fileName < b.fileName;
    });
    // Two games with the same title (say USA and Europe): show their full file names.
    std::map<std::string, int> count;
    for (const auto& g : games) ++count[lower(g.title)];
    for (auto& g : games)
        if (count[lower(g.title)] > 1) g.title = g.fileName;
    return games;
}
