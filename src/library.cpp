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

// ============================================================================
// Regions (to pick the cover from the same country as the game; USA when nothing says otherwise)

namespace {

// A region tag word: "USA", "U", "Europe", "PAL", "F"... -> No-Intro region name, or "".
std::string regionWord(const std::string& w) {
    static const std::map<std::string, std::string> words = {
        {"USA", "USA"}, {"US", "USA"}, {"U", "USA"}, {"NTSC-U", "USA"}, {"America", "USA"}, {"Canada", "Canada"},
        {"Europe", "Europe"}, {"EU", "Europe"}, {"EUR", "Europe"}, {"E", "Europe"}, {"PAL", "Europe"}, {"UK", "UK"},
        {"Japan", "Japan"}, {"JP", "Japan"}, {"JPN", "Japan"}, {"J", "Japan"}, {"NTSC-J", "Japan"},
        {"World", "World"}, {"W", "World"},
        {"France", "France"}, {"F", "France"}, {"Germany", "Germany"}, {"G", "Germany"}, {"Spain", "Spain"},
        {"S", "Spain"}, {"Italy", "Italy"}, {"I", "Italy"}, {"Netherlands", "Netherlands"}, {"Sweden", "Sweden"},
        {"Denmark", "Denmark"}, {"Finland", "Finland"}, {"Norway", "Norway"}, {"Portugal", "Portugal"},
        {"Russia", "Russia"}, {"Korea", "Korea"}, {"K", "Korea"}, {"China", "China"}, {"C", "China"},
        {"Taiwan", "Taiwan"}, {"Hong Kong", "Hong Kong"}, {"Asia", "Asia"}, {"Australia", "Australia"},
        {"A", "Australia"}, {"Brazil", "Brazil"}, {"B", "Brazil"},
        // Country codes some collections use: "(DE)", "(FR)" (upper case; "(De)" is a language).
        {"DE", "Germany"}, {"FR", "France"}, {"ES", "Spain"}, {"IT", "Italy"}, {"NL", "Netherlands"},
        {"SE", "Sweden"}, {"KR", "Korea"}, {"CN", "China"}, {"TW", "Taiwan"}, {"AU", "Australia"},
        {"BR", "Brazil"}, {"GB", "UK"}, {"CA", "Canada"},
    };
    const auto it = words.find(w);
    return it == words.end() ? std::string() : it->second;
}

// The region letter in Nintendo game codes (4th character of DS / GBA / GameCube / Wii IDs, N64 country byte).
std::string regionFromCode(char c) {
    switch (c) {
    case 'E': case 'N': return "USA";
    case 'J': return "Japan";
    case 'P': case 'X': case 'Y': case 'Z': case 'L': case 'M': case 'V': return "Europe";
    case 'D': return "Germany";
    case 'F': return "France";
    case 'S': return "Spain";
    case 'I': return "Italy";
    case 'H': return "Netherlands";
    case 'K': case 'Q': case 'T': return "Korea";
    case 'C': return "China";
    case 'W': return "Taiwan";
    case 'U': return "Australia";
    case 'B': return "Brazil";
    case 'R': return "Russia";
    default: return {};
    }
}

// SNES header region byte.
std::string snesRegion(uint8_t b) {
    static const char* names[] = {"Japan", "USA", "Europe", "Sweden", "Finland", "Denmark", "France", "Netherlands",
                                  "Spain", "Germany", "Italy", "China", "", "Korea", "", "Canada", "Brazil", "Australia"};
    return b < sizeof names / sizeof names[0] ? names[b] : "";
}

// One region for a list: USA if it's there (the default), else World, else the first.
std::string pickRegion(const std::vector<std::string>& regions) {
    if (regions.empty()) return {};
    for (const char* r : {"USA", "World"})
        if (std::find(regions.begin(), regions.end(), r) != regions.end()) return r;
    return regions[0];
}

}  // namespace

std::vector<std::string> regionsInName(const std::string& name) {
    std::vector<std::string> out;
    for (size_t a = name.find_first_of("(["); a != std::string::npos; a = name.find_first_of("([", a + 1)) {
        const size_t b = name.find_first_of(")]", a);
        if (b == std::string::npos) break;
        const std::string group = name.substr(a + 1, b - a - 1);
        std::vector<std::string> words;
        std::string w;
        for (const char c : group + ",") {
            if (c == ',') {
                w = trim(w);
                if (!w.empty()) words.push_back(w);
                w.clear();
            } else {
                w += c;
            }
        }
        // GoodTools letter runs: "(UE)", "(JU)", "(JUE)".
        if (words.size() == 1 && regionWord(words[0]).empty() && words[0].size() <= 3 &&
            words[0].find_first_not_of("UEJ") == std::string::npos) {
            const std::string run = words[0];
            words.clear();
            for (const char c : run) words.push_back(std::string(1, c));
        }
        std::vector<std::string> found;
        for (const std::string& x : words)
            if (const std::string r = regionWord(x); !r.empty()) found.push_back(r);
        // Only a group made entirely of regions counts ("(En,Fr,De)" is languages, "(Rev 1)" is neither).
        if (!found.empty() && found.size() == words.size()) out.insert(out.end(), found.begin(), found.end());
    }
    return out;
}

std::string parentRegion(const std::string& region) {
    static const std::map<std::string, std::string> parent = {
        {"France", "Europe"}, {"Germany", "Europe"}, {"Spain", "Europe"}, {"Italy", "Europe"},
        {"Netherlands", "Europe"}, {"Sweden", "Europe"}, {"Denmark", "Europe"}, {"Finland", "Europe"},
        {"Norway", "Europe"}, {"Portugal", "Europe"}, {"UK", "Europe"}, {"Russia", "Europe"},
        {"Australia", "Europe"}, {"Canada", "USA"}, {"Brazil", "USA"}, {"Korea", "Asia"}, {"China", "Asia"},
        {"Taiwan", "Asia"}, {"Hong Kong", "Asia"},
    };
    const auto it = parent.find(region);
    return it == parent.end() ? std::string() : it->second;
}

// ============================================================================
// Game IDs (used to find covers on GameTDB)

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
bool discHeader(const std::vector<uint8_t>& h, std::string& id, std::string& name) {
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
    return true;
}

void gameCubeWiiId(std::ifstream& f, std::string& id, std::string& name) {
    const std::vector<uint8_t> head = readAt(f, 0, 0x60);
    if (head.size() < 0x20) return;
    if (discHeader(head, id, name)) return;  // plain .iso / .gcm
    if (std::memcmp(head.data(), "WBFS", 4) == 0) {
        const unsigned shift = head[8];
        if (shift >= 9 && shift <= 16) discHeader(readAt(f, 1u << shift, 0x60), id, name);
    } else if (std::memcmp(head.data(), "WIA\x01", 4) == 0 || std::memcmp(head.data(), "RVZ\x01", 4) == 0) {
        discHeader(readAt(f, 0x58, 0x60), id, name);  // a copy of the disc header is stored at 0x58
    } else if (std::memcmp(head.data(), "CISO", 4) == 0) {
        discHeader(readAt(f, 0x8000, 0x60), id, name);
    } else if (le32(head.data()) == 0xB10BC001) {  // GCZ: zlib-compressed blocks
        const uint32_t blocks = le32(&head[0x1C]);
        if (blocks == 0 || blocks > (1u << 24)) return;
        const std::vector<uint8_t> ptrs = readAt(f, 0x20, 16);
        if (ptrs.size() < 16) return;
        constexpr uint64_t kRaw = 1ull << 63;
        const uint64_t p0 = le64(&ptrs[0]), p1 = blocks > 1 ? le64(&ptrs[8]) & ~kRaw : le64(&head[8]);
        const uint64_t start = 0x20 + uint64_t(blocks) * 12 + (p0 & ~kRaw);
        if (p0 & kRaw) {
            discHeader(readAt(f, start, 0x60), id, name);
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
        if (z.avail_out == 0) discHeader(disc, id, name);
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

LibraryGame readGame(const fs::path& path, System console, bool folder) {
    LibraryGame g;
    g.path = path;
    g.system = console;
    g.fileName = folder ? path.filename().u8string() : path.stem().u8string();
    g.title = cleanTitle(g.fileName);
    // Where the game is from: its file name says first, then the game itself (below).
    g.region = pickRegion(regionsInName(g.fileName));

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
        if (g.region.empty() && !g.gameId.empty()) g.region = regionFromCode(g.gameId[3]);
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
        gameCubeWiiId(f, g.gameId, name);
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

    if (g.region.empty()) {
        if (!g.gameId.empty()) {
            g.region = regionFromCode(g.gameId[3]);
        } else if (console == System::GBA) {
            const std::vector<uint8_t> h = readAt(f, 0xAC, 4);  // game code, e.g. "AXVE"
            if (h.size() == 4 && validId(std::string(h.begin(), h.end()), 4)) g.region = regionFromCode(char(h[3]));
        } else if (console == System::N64) {
            const std::vector<uint8_t> h = readAt(f, 0, 0x40);
            if (h.size() == 0x40) {
                // Country byte at 0x3E in big-endian (.z64) ROMs; the other byte orders move it.
                const uint32_t magic = be32(h.data());
                const size_t at = magic == 0x80371240 ? 0x3E : magic == 0x37804012 ? 0x3F : magic == 0x40123780 ? 0x3D : 0;
                if (at) g.region = regionFromCode(char(h[at]));
            }
        } else if (console == System::SNES) {
            // Internal header (LoROM / HiROM, maybe after a 512-byte copier header): the one whose
            // checksum and complement add up holds the region byte.
            for (const uint64_t base : {0x7FC0, 0xFFC0, 0x81C0, 0x101C0}) {
                const std::vector<uint8_t> h = readAt(f, base, 0x20);
                if (h.size() < 0x20 || (uint32_t(h[0x1C] | h[0x1D] << 8) ^ uint32_t(h[0x1E] | h[0x1F] << 8)) != 0xFFFF)
                    continue;
                g.region = snesRegion(h[0x19]);
                break;
            }
        } else if (console == System::GB) {
            const std::vector<uint8_t> h = readAt(f, 0x14A, 1);  // destination: 0 = Japan only
            if (h.size() == 1 && h[0] == 0) g.region = "Japan";
        }
    }
    return g;
}

}  // namespace

std::vector<LibraryGame> Library::scan(const fs::path& folder, System console) {
    std::vector<LibraryGame> games;

    auto add = [&](const fs::path& p, bool isFolder) {
        std::error_code ec;
        const auto time = fs::last_write_time(isFolder ? p / "meta" / "meta.xml" : p, ec);
        const uintmax_t size = isFolder ? 0 : fs::file_size(p, ec);
        const std::string key = p.u8string();
        auto it = known_.find(key);
        if (it == known_.end() || it->second.time != time || it->second.size != size) {
            LibraryGame game = readGame(p, console, isFolder);
            if (game.region == "USA") game.region.clear();  // the default
            it = known_.insert_or_assign(key, Known{time, size, std::move(game)}).first;
        }
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
                if (console == System::WiiU && isWiiUFolder(p)) add(p, true);
                else if (depth < 3) walk(p, depth + 1);
            } else if (e.is_regular_file(ec2)) {
                const std::string ext = lowerExt(p);
                const System s = systemFromExtension(ext);
                if (ext == ".zip" || (s != System::Unknown && family(s) == family(console))) add(p, false);
            }
        }
    };
    walk(folder, 0);

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
