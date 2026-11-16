#include "covers.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cstring>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

#include "http.hpp"
#include "png.hpp"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP  // custom covers (made into PNG pictures)
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "stb_image.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace fs = std::filesystem;

namespace {

constexpr auto kRetryMissing = std::chrono::hours(24 * 7);  // look again for covers that weren't found
constexpr auto kIndexAge = std::chrono::hours(24 * 30);     // refresh libretro's list of names monthly

struct Request {
    std::string key;  // the game's path
    System system;
    std::string fileName, title;
    fs::path folder;  // the console's Box Covers and Manuals folder
};

struct Image {
    std::string key;
    unsigned generation = 0;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    fs::path file;  // the picture it came from ("" = no cover)
    bool retry = false;  // couldn't be looked up (no internet, or the server had a problem): try again later
};

const char* kCoverExts[] = {".png", ".jpg", ".jpeg"};

// ---------------------------------------------------------------------------
// Files

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

void writeFile(const fs::path& p, const void* data, size_t size) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.u8string() + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(static_cast<const char*>(data), std::streamsize(size));
        if (!f) return;
    }
    fs::rename(tmp, p, ec);
}

bool olderThan(const fs::path& p, fs::file_time_type::duration age) {
    std::error_code ec;
    const auto t = fs::last_write_time(p, ec);
    return ec || fs::file_time_type::clock::now() - t > age;
}

std::string safeFileName(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (out.size() >= 120) break;
        out += (std::isalnum(c) || c >= 0x80 || std::strchr(" -_.()[]!,&'+", char(c))) ? char(c) : '_';
    }
    return out;
}

bool isImage(const std::vector<uint8_t>& b) {
    return (b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G') ||
           (b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF);
}

// ---------------------------------------------------------------------------
// Pictures

// Shrinks an RGBA picture to dw x dh, averaging every source pixel each one covers (sharp, no
// shimmer). Colors are weighted by alpha so see-through edges don't go dark.
std::vector<uint8_t> resizeArea(const uint8_t* src, int w, int h, int dw, int dh) {
    // Horizontal pass into floats (premultiplied), then vertical.
    std::vector<float> mid(size_t(dw) * h * 4, 0.0f);
    const double sx = double(w) / dw, sy = double(h) / dh;
    for (int x = 0; x < dw; ++x) {
        const double x0 = x * sx, x1 = x0 + sx;
        for (int ix = int(x0); ix < w && ix < x1; ++ix) {
            const float wt = float((std::min(x1, double(ix + 1)) - std::max(x0, double(ix))) / sx);
            for (int y = 0; y < h; ++y) {
                const uint8_t* p = src + (size_t(y) * w + size_t(ix)) * 4;
                float* m = &mid[(size_t(y) * dw + size_t(x)) * 4];
                const float a = p[3] / 255.0f;
                m[0] += wt * p[0] * a;
                m[1] += wt * p[1] * a;
                m[2] += wt * p[2] * a;
                m[3] += wt * p[3];
            }
        }
    }
    std::vector<uint8_t> out(size_t(dw) * dh * 4);
    for (int y = 0; y < dh; ++y) {
        const double y0 = y * sy, y1 = y0 + sy;
        for (int x = 0; x < dw; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int iy = int(y0); iy < h && iy < y1; ++iy) {
                const float wt = float((std::min(y1, double(iy + 1)) - std::max(y0, double(iy))) / sy);
                const float* m = &mid[(size_t(iy) * dw + size_t(x)) * 4];
                for (int c = 0; c < 4; ++c) acc[c] += wt * m[c];
            }
            uint8_t* o = &out[(size_t(y) * dw + size_t(x)) * 4];
            const float a = acc[3] / 255.0f;
            for (int c = 0; c < 3; ++c) o[c] = uint8_t(std::clamp(a > 0 ? acc[c] / a : 0.0f, 0.0f, 255.0f) + 0.5f);
            o[3] = uint8_t(std::clamp(acc[3], 0.0f, 255.0f) + 0.5f);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// libretro thumbnails: thumbnails.libretro.com/<system>/Named_Boxarts/<No-Intro name>.png

const char* libretroSystem(System s) {
    switch (s) {
    case System::NES: return "Nintendo - Nintendo Entertainment System";
    case System::FDS: return "Nintendo - Family Computer Disk System";
    case System::SNES: return "Nintendo - Super Nintendo Entertainment System";
    case System::N64: return "Nintendo - Nintendo 64";
    case System::GB: return "Nintendo - Game Boy";
    case System::GBC: return "Nintendo - Game Boy Color";
    case System::GBA: return "Nintendo - Game Boy Advance";
    case System::NDS: return "Nintendo - Nintendo DS";
    case System::GameCube: return "Nintendo - GameCube";
    case System::Wii: return "Nintendo - Wii";
    case System::WiiU: return "Nintendo - Wii U";
    case System::N3DS: return "Nintendo - Nintendo 3DS";
    default: return nullptr;
    }
}

// The plain letter for an accented one (second byte of its UTF-8 after 0xC3): é -> e. 0 = not a letter.
char plainLetter(unsigned char b) {
    static const char* const map = "aaaaaaaceeeeiiiidnooooo/ouuuuyty";  // 0xA0..0xBF (lowercase)
    if (b == 0x9F) return 's';                                               // ß
    if (b >= 0x80 && b <= 0x9E) b = static_cast<unsigned char>(b + 0x20);  // uppercase -> lowercase
    if (b < 0xA0 || b > 0xBF) return 0;
    const char c = map[b - 0xA0];
    return c == '/' ? 0 : c;
}

// The words of a game's name before its (tags), lowercase, without "the":
// "Legend of Zelda, The (USA)" and "The Legend of Zelda" both give {legend, of, zelda}.
std::vector<std::string> nameWords(const std::string& s) {
    std::vector<std::string> words;
    std::string w;
    auto flush = [&] {
        if (!w.empty() && w != "the") words.push_back(w);
        w.clear();
    };
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        const char plain = c == 0xC3 && i + 1 < s.size() ? plainLetter(static_cast<unsigned char>(s[i + 1])) : 0;
        if (c == '(' || c == '[') break;
        if (c == '&') {
            flush();
            w = "and";
            flush();
        } else if (plain) {
            w += plain;  // "Pokémon" = "Pokemon"
            ++i;
        } else if (std::isalnum(c) || c >= 0x80) {
            w += char(std::tolower(c));
        } else if (c != '\'' && c != '.') {  // "Bros." = "Bros", "Kirby's" = "Kirbys"
            flush();
        }
    }
    flush();
    return words;
}

std::string joinWords(const std::vector<std::string>& w) {
    std::string out;
    for (const auto& x : w) out += x;
    return out;
}

struct Index {
    std::vector<std::string> names;
    std::unordered_map<std::string, std::vector<size_t>> byKey;  // joined name words -> names
    // The words before " - " in names with a subtitle: "Donkey Kong Country 2 - Diddy's Kong
    // Quest" is found as "donkeykongcountry2".
    std::unordered_map<std::string, std::vector<size_t>> bySeries;
};

void buildIndex(Index& idx) {
    for (size_t i = 0; i < idx.names.size(); ++i) {
        const std::string& n = idx.names[i];
        idx.byKey[joinWords(nameWords(n))].push_back(i);
        const size_t dash = n.find(" - ");
        if (dash != std::string::npos && dash < n.find('(')) {
            const std::vector<std::string> series = nameWords(n.substr(0, dash));
            if (series.size() >= 2) idx.bySeries[joinWords(series)].push_back(i);
        }
    }
}

std::string urlDecode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) &&
            std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += char(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// Names (without .png) linked from the server's folder listing.
std::vector<std::string> parseListing(const std::vector<uint8_t>& html) {
    std::vector<std::string> names;
    const std::string text(html.begin(), html.end());
    size_t at = 0;
    while ((at = text.find("href=\"", at)) != std::string::npos) {
        at += 6;
        const size_t end = text.find('"', at);
        if (end == std::string::npos) break;
        const std::string href = text.substr(at, end - at);
        at = end;
        if (href.size() > 4 && href.compare(href.size() - 4, 4, ".png") == 0 && href.find('/') == std::string::npos) {
            std::string name = urlDecode(href.substr(0, href.size() - 4));
            for (size_t p; (p = name.find("&amp;")) != std::string::npos;) name.replace(p, 5, "&");
            names.push_back(name);
        }
    }
    return names;
}

// A "(...)" tag naming a region: "(Europe)", "(USA, Europe)", "(PAL)", "(E)"...
bool isRegionTag(const std::string& tag) {
    for (const char* r : {"USA", "Europe", "Japan", "World", "Asia", "Australia", "Brazil", "Canada", "China", "France",
                          "Germany", "Italy", "Korea", "Netherlands", "Spain", "Sweden", "UK", "PAL", "NTSC"})
        if (tag.find(r) != std::string::npos) return true;
    for (const char* r : {"(E)", "(U)", "(J)", "(UE)", "(JU)", "(EU)", "(JUE)"})
        if (tag == r) return true;
    return false;
}

// A region tag of the name says USA: "(USA)", "(USA, Europe)", "(Japan, USA)"...
bool isUsaRegion(const std::string& name) {
    for (size_t a = name.find('('); a != std::string::npos; a = name.find('(', a + 1)) {
        const size_t b = name.find(')', a);
        if (b == std::string::npos) break;
        const std::string tag = name.substr(a, b - a + 1);
        if (isRegionTag(tag) && tag.find("USA") != std::string::npos) return true;
    }
    return false;
}

// A list of languages, like "(En,Fr,De)": says nothing about which box it is.
bool isLanguageTag(const std::string& tag) {
    if (tag.size() < 4 || tag.front() != '(' || tag.back() != ')') return false;
    const std::string in = tag.substr(1, tag.size() - 2);
    size_t i = 0;
    while (i < in.size()) {
        if (i + 2 > in.size() || !std::isupper(static_cast<unsigned char>(in[i])) ||
            !std::islower(static_cast<unsigned char>(in[i + 1])))
            return false;
        i += 2;
        if (i < in.size() && in[i] != ',') return false;
        ++i;
    }
    return true;
}

// Picks the best-looking entry: the USA box first (then a World one), and the plain release: tags
// the file has ("(Rev 1)") count for it, other tags against it ("(Virtual Console)", "(Collector's
// Edition)", "(Mega Man X Legacy Collection)" are re-releases with other boxes). Hacks, betas and
// prototypes only for a file that is one too.
int score(const std::string& name, const std::string& fileName) {
    if (name == fileName) return 1 << 30;
    auto has = [](const std::string& s, const char* t) { return s.find(t) != std::string::npos; };
    int sc = 0;
    for (size_t a = fileName.find('('); a != std::string::npos; a = fileName.find('(', a + 1)) {
        const size_t b = fileName.find(')', a);
        if (b == std::string::npos) break;
        const std::string tag = fileName.substr(a, b - a + 1);
        if (!isRegionTag(tag) && !isLanguageTag(tag) && has(name, tag.c_str())) sc += 40;
    }
    for (size_t a = name.find('('); a != std::string::npos; a = name.find('(', a + 1)) {
        const size_t b = name.find(')', a);
        if (b == std::string::npos) break;
        const std::string tag = name.substr(a, b - a + 1);
        if (!isRegionTag(tag) && !isLanguageTag(tag) && !has(fileName, tag.c_str())) sc -= 15;
    }
    if (isUsaRegion(name)) sc += 100;
    else if (has(name, "(World")) sc += 60;
    for (const char* bad : {"[h", "[b", "[t", "(Beta", "(Proto", "(Demo", "(Sample", "(Pirate", "(Unl", "(Hack", "[a"})
        if (has(name, bad) && !has(fileName, bad)) sc -= 50;
    return sc * 1000 - int(std::min<size_t>(name.size(), 999));  // then the shortest name
}

// True for a USA release: "(USA)", "(USA, Europe)", "(Japan, USA)"..., and "(World)" (one release
// for every country, the USA one included: "Super Mario Bros. (World)").
bool isUsaName(const std::string& name) {
    for (size_t a = name.find('('); a != std::string::npos; a = name.find('(', a + 1)) {
        const size_t b = name.find(')', a);
        if (b == std::string::npos) break;
        const std::string tag = name.substr(a, b - a + 1);
        if (isRegionTag(tag) && (tag.find("USA") != std::string::npos || tag.find("World") != std::string::npos))
            return true;
    }
    return false;
}

// A word that makes another game of a series: "2", "3D", "II", "64"...
bool isSequelWord(const std::string& w) {
    if (!w.empty() && std::isdigit(static_cast<unsigned char>(w[0]))) return true;
    static const char* const roman[] = {"ii", "iii", "iv", "v", "vi", "vii", "viii", "ix", "x", "xi", "xii"};
    return std::any_of(std::begin(roman), std::end(roman), [&](const char* r) { return w == r; });
}

// The names matching a game's name, USA releases only.
//  - The same words ("Legend of Zelda, The (USA)" for "The Legend of Zelda.nes"). If the list has
//    the game but not its USA release, there's no match: guessing would pick another game.
//  - Otherwise close names: every word of the game's name is in the cover's name, which has at most
//    2 more, and none that make it another game of the series ("Pokemon Red" -> "Pokemon - Red
//    Version", never "Super Mario Bros." -> "Super Mario Bros. 2"). Fewest extra words wins.
std::vector<size_t> findNames(const Index& idx, const std::string& fileName, const std::string& title) {
    std::vector<size_t> found;
    bool listed = false;  // the game is in the list (in any region)
    for (const std::string& n : {fileName, title}) {
        const auto it = idx.byKey.find(joinWords(nameWords(n)));
        if (it == idx.byKey.end()) continue;
        listed = true;
        for (const size_t i : it->second)
            if (isUsaName(idx.names[i])) found.push_back(i);
        if (!found.empty()) return found;
    }
    if (listed) return {};
    // The game's name followed by a subtitle: "Donkey Kong Country 2" -> "Donkey Kong Country 2 -
    // Diddy's Kong Quest (USA)", "Super Mario Land 2" -> "Super Mario Land 2 - 6 Golden Coins".
    for (const std::string& n : {fileName, title}) {
        const auto it = idx.bySeries.find(joinWords(nameWords(n)));
        if (it == idx.bySeries.end()) continue;
        for (const size_t i : it->second)
            if (isUsaName(idx.names[i])) found.push_back(i);
        if (!found.empty()) return found;
    }
    for (const std::string& n : {fileName, title}) {
        // Close names: every word of the game's name is in the cover's name, which has at most 2
        // more ("Pokemon Red" -> "Pokemon - Red Version"). Fewest extra words wins.
        const std::vector<std::string> want = nameWords(n);
        if (want.size() < 2) continue;
        const std::set<std::string> wantSet(want.begin(), want.end());
        size_t bestExtra = 3;
        for (size_t i = 0; i < idx.names.size(); ++i) {
            if (!isUsaName(idx.names[i])) continue;
            const std::vector<std::string> w = nameWords(idx.names[i]);
            if (w.size() < want.size() || w.size() - want.size() > 2 || w.size() - want.size() > bestExtra) continue;
            const std::set<std::string> have(w.begin(), w.end());
            if (!std::all_of(wantSet.begin(), wantSet.end(), [&](const std::string& x) { return have.count(x) > 0; }))
                continue;
            if (std::any_of(w.begin(), w.end(), [&](const std::string& x) { return !wantSet.count(x) && isSequelWord(x); }))
                continue;  // another game of the series
            if (w.size() - want.size() < bestExtra) {
                bestExtra = w.size() - want.size();
                found.clear();
            }
            found.push_back(i);
        }
        if (!found.empty()) break;
    }
    return found;
}

// The cover's name for a game: its USA release ("" if the list has none).
std::string bestMatch(const Index& idx, const std::string& fileName, const std::string& title) {
    const std::vector<size_t> found = findNames(idx, fileName, title);
    std::string best;
    int bestScore = 0;
    for (const size_t i : found) {
        const int sc = score(idx.names[i], fileName);
        if (best.empty() || sc > bestScore) {
            best = idx.names[i];
            bestScore = sc;
        }
    }
    return best;
}

}  // namespace

// ============================================================================
// The download thread

// The note of where a game's downloaded cover came from (its web address).
static fs::path coverSourceFile(const fs::path& cacheDir, System system, const std::string& fileName) {
    return cacheDir / systemInfo(system).key / ("from - " + safeFileName(fileName) + ".url");
}

struct Covers::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> queue;  // newest last: what's on screen now is fetched first
    std::vector<Image> done;
    unsigned generation = 0;
    bool stop = false;
    fs::path cacheDir;

    // Used only by the thread.
    std::map<System, Index> indexes;
    std::map<System, std::chrono::steady_clock::time_point> indexRetry;  // list unavailable: not asked again before then
    std::atomic<int> boxW{0}, boxH{0};  // the size covers are shown at (set by the home screen)
    std::chrono::steady_clock::time_point offlineUntil;  // no internet: wait a minute before trying again

    bool offline() const { return std::chrono::steady_clock::now() < offlineUntil; }
    void run();
    Image load(const Request& r);
    std::vector<uint8_t> download(const Request& r, bool& reached);
    std::string downloadedFrom;  // the web address of the cover download() returned
    const Index& index(System s);
    int get(const std::string& url, std::vector<uint8_t>& body, bool& reached) {
        if (offline()) return 0;
        const int status = httpGet(url, body);
        if (status != 0) reached = true;
        else offlineUntil = std::chrono::steady_clock::now() + std::chrono::minutes(1);
        return status;
    }
};

void Covers::Shared::run() {
    for (;;) {
        Request r;
        unsigned gen;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return stop || !queue.empty(); });
            if (stop) return;
            r = std::move(queue.back());
            queue.pop_back();
            gen = generation;
        }
        Image img = load(r);
        img.key = r.key;
        img.generation = gen;
        std::lock_guard<std::mutex> lock(mutex);
        if (stop) return;
        done.push_back(std::move(img));
    }
}

const Index& Covers::Shared::index(System s) {
    static const Index empty;
    auto it = indexes.find(s);
    if (it != indexes.end()) return it->second;
    if (const auto r = indexRetry.find(s); r != indexRetry.end() && std::chrono::steady_clock::now() < r->second)
        return empty;
    const char* sys = libretroSystem(s);
    if (!sys) return empty;

    const fs::path file = cacheDir / "names" / (std::string(sys) + ".txt");
    std::vector<std::string> names;
    bool reached = true;
    if (olderThan(file, kIndexAge)) {
        std::vector<uint8_t> body;
        reached = false;
        if (get("https://thumbnails.libretro.com/" + urlEncode(sys) + "/Named_Boxarts/", body, reached) == 200) {
            names = parseListing(body);
        }
        if (!names.empty()) {
            std::string text;
            for (const auto& n : names) text += n + "\n";
            writeFile(file, text.data(), text.size());
        }
    }
    if (names.empty()) {  // fresh enough, or the download failed: use the saved list
        std::ifstream f(file);
        for (std::string line; std::getline(f, line);)
            if (!line.empty()) names.push_back(line);
    }
    if (names.empty()) {
        // Offline, or the server had a problem: ask again
        // in a couple of minutes, rather than going without covers until Nintendall restarts.
        indexRetry[s] = std::chrono::steady_clock::now() + std::chrono::minutes(2);
        return empty;
    }
    Index& idx = indexes[s];
    idx.names = std::move(names);
    buildIndex(idx);
    return idx;
}

std::vector<uint8_t> Covers::Shared::download(const Request& r, bool& reached) {
    std::vector<uint8_t> body;
    const char* sys = libretroSystem(r.system);
    if (!sys) return {};
    const Index& idx = index(r.system);
    if (idx.names.empty()) return {};  // the list isn't available yet: tried again later
    const std::string name = bestMatch(idx, r.fileName, r.title);
    if (name.empty()) {
        reached = true;  // no USA cover in the list
        return {};
    }
    const std::string url = "https://thumbnails.libretro.com/" + urlEncode(sys) + "/Named_Boxarts/" + urlEncode(name) + ".png";
    const int status = get(url, body, reached);
    if (status == 200 && isImage(body)) {
        downloadedFrom = url;
        return body;
    }
    if (status != 404) reached = false;  // offline, or the server had a problem: tried again later
    return {};
}

Image Covers::Shared::load(const Request& r) {
    Image img;
    std::vector<uint8_t> bytes;
    std::error_code ec;

    // A picture in the Box Covers and Manuals folder is always used as it is: never replaced or removed.
    const fs::path have = findCoverFile(r.folder, r.fileName);
    if (!have.empty()) {
        if (readFile(have, bytes)) img.file = have;
    } else {
        // No picture yet: download the USA cover in the best quality there is (unless it was
        // looked for in the last week and doesn't exist), and save it in the Box Covers and Manuals folder.
        // ("usa2": notes from older versions don't count, their matching missed "(World)" covers.)
        const fs::path none = cacheDir / systemInfo(r.system).key / ("usa2 - " + safeFileName(r.fileName) + ".none");
        if (fs::exists(none, ec) && !olderThan(none, kRetryMissing)) return img;
        bool reached = false;
        bytes = download(r, reached);
        if (bytes.empty()) {
            if (reached) writeFile(none, "", 0);  // the servers answered: there's no cover
            else img.retry = true;
            return img;
        }
        // Named like the game, unless the player put a picture there meanwhile.
        if (const fs::path mine = findCoverFile(r.folder, r.fileName); !mine.empty()) {
            if (!readFile(mine, bytes)) return img;
            img.file = mine;
        } else {
            const bool png = bytes.size() > 4 && bytes[0] == 0x89 && bytes[1] == 'P';
            img.file = r.folder / fs::u8path(r.fileName + (png ? ".png" : ".jpg"));
            if (fs::exists(img.file, ec)) {  // a file is there already: it's used, never written over
                if (!readFile(img.file, bytes)) return img;
            } else {
                writeFile(img.file, bytes.data(), bytes.size());
                // Where it came from (the Discord status shows the cover from there).
                writeFile(coverSourceFile(cacheDir, r.system, r.fileName), downloadedFrom.data(), downloadedFrom.size());
            }
        }
    }
    if (bytes.empty()) return img;

    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return img;
    // Made exactly the size of the box it's shown in: every cover of a console is the same size
    // (sharp, and hundreds of covers fit in memory). Covers a little narrower or wider are stretched.
    const int bw = boxW, bh = boxH;
    if (bw > 0 && bh > 0 && (w > bw || h > bh)) {
        img.w = bw;
        img.h = bh;
        img.rgba = resizeArea(px, w, h, img.w, img.h);
    } else {
        img.rgba.assign(px, px + size_t(w) * h * 4);
        img.w = w;
        img.h = h;
    }
    stbi_image_free(px);
    return img;
}

// ============================================================================

bool decodePicture(const std::vector<uint8_t>& bytes, int maxW, int maxH, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!isImage(bytes)) return false;
    int iw = 0, ih = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &iw, &ih, &n, 4);
    if (!px) return false;
    const double f = std::min({1.0, double(maxW) / iw, double(maxH) / ih});
    if (f < 1.0) {
        w = std::max(1, int(iw * f + 0.5));
        h = std::max(1, int(ih * f + 0.5));
        rgba = resizeArea(px, iw, ih, w, h);
    } else {
        w = iw;
        h = ih;
        rgba.assign(px, px + size_t(iw) * ih * 4);
    }
    stbi_image_free(px);
    return true;
}

fs::path findCoverFile(const fs::path& dir, const std::string& fileName) {
    if (dir.empty() || fileName.empty()) return {};
    std::error_code ec;
    for (const char* ext : kCoverExts) {
        const fs::path p = dir / fs::u8path(fileName + ext);
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}

std::string addCustomCover(const fs::path& dir, const std::string& fileName, const fs::path& picture) {
    std::error_code ec;
    std::vector<uint8_t> bytes;
    {
        std::ifstream f(picture, std::ios::binary);
        if (!f) return "Couldn't open that picture.";
        bytes.assign(std::istreambuf_iterator<char>(f), {});
    }
    // PNG and JPEG pictures are copied as they are; others (BMP, GIF) are made into PNG pictures.
    const bool png = bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
    const bool jpg = bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
    std::string ext = png ? ".png" : ".jpg";
    if (!png && !jpg) {
        int w = 0, h = 0, n = 0;
        stbi_uc* px = bytes.empty() ? nullptr : stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 3);
        if (!px) return "That isn't a picture Nintendall can use: pick a PNG or JPEG picture.";
        bytes = encodePng(w, h, px);
        stbi_image_free(px);
        if (bytes.empty()) return "Couldn't use that picture.";
        ext = ".png";
    }
    const fs::path old = findCoverFile(dir, fileName);
    if (!old.empty() && fs::equivalent(old, picture, ec)) return "That picture is already this game's cover.";

    fs::create_directories(dir, ec);
    const fs::path dest = dir / fs::u8path(fileName + ext);
    const fs::path tmp = dest.u8string() + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return "Couldn't save the cover in the Box Covers and Manuals folder.";
        }
    }
    // The player asked for it: the game's old cover (any kind of picture) is replaced.
    for (const char* e : kCoverExts) {
        const fs::path old = dir / fs::u8path(fileName + e);
        if (fs::is_regular_file(old, ec)) fs::remove(old, ec);
    }
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "Couldn't save the cover in the Box Covers and Manuals folder.";
    }
    return "";
}

void Covers::reload(const LibraryGame& game) {
    const auto it = textures_.find(game.path.u8string());
    if (it == textures_.end()) return;
    if (it->second.tex) SDL_DestroyTexture(it->second.tex);
    textures_.erase(it);
}

Covers::~Covers() {
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stop = true;
    shared_->wake.notify_all();
}

void Covers::start(const fs::path& cacheDir) {
    if (shared_) return;
    shared_ = std::make_shared<Shared>();
    shared_->cacheDir = cacheDir;
    // The thread keeps its own reference: quitting during a download is safe.
    std::thread([s = shared_] { s->run(); }).detach();
}

void Covers::setDisplaySize(int w, int h) {
    if (!shared_ || (std::abs(w - shared_->boxW) <= 1 && std::abs(h - shared_->boxH) <= 1)) return;
    shared_->boxW = w;
    shared_->boxH = h;
    clear();
}

SDL_Texture* Covers::get(const LibraryGame& g) {
    const std::string key = g.path.u8string();
    const auto it = textures_.find(key);
    if (it != textures_.end()) {
        const Entry& e = it->second;
        if (e.loading || e.tex || e.retryAt == 0 || SDL_TICKS_PASSED(SDL_GetTicks(), e.retryAt) == SDL_FALSE)
            return e.tex;
        textures_.erase(it);  // look for it again
    }
    textures_[key] = Entry{};
    if (shared_) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->queue.push_back({key, g.system, g.fileName, g.title, folder_});
        shared_->wake.notify_one();
    }
    return nullptr;
}

void Covers::recheck(const std::vector<LibraryGame>& games) {
    for (const LibraryGame& g : games) {
        const auto it = textures_.find(g.path.u8string());
        if (it == textures_.end() || it->second.loading) continue;
        const fs::path now = findCoverFile(folder_, g.fileName);
        std::error_code ec;
        const auto time = now.empty() ? fs::file_time_type{} : fs::last_write_time(now, ec);
        if (now.u8string() == it->second.file && time == it->second.time) continue;
        // Added, replaced or deleted: load it again.
        if (it->second.tex) SDL_DestroyTexture(it->second.tex);
        textures_.erase(it);
    }
}

void Covers::update(SDL_Renderer* renderer) {
    if (!shared_) return;
    std::vector<Image> done;
    unsigned gen;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        done.swap(shared_->done);
        gen = shared_->generation;
    }
    for (Image& img : done) {
        if (img.generation != gen) continue;
        const auto it = textures_.find(img.key);
        if (it == textures_.end() || !it->second.loading) continue;
        Entry& e = it->second;
        e.loading = false;
        if (img.retry) e.retryAt = SDL_GetTicks() + 3 * 60 * 1000;  // offline or server trouble: in 3 minutes
        e.file = img.file.u8string();
        std::error_code ec;
        if (!img.file.empty()) e.time = fs::last_write_time(img.file, ec);
        if (img.rgba.empty()) continue;
        SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.w, img.h);
        if (!t) continue;
        SDL_UpdateTexture(t, nullptr, img.rgba.data(), img.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        e.tex = t;
    }
}

void Covers::clear() {
    for (auto& [key, e] : textures_)
        if (e.tex) SDL_DestroyTexture(e.tex);
    textures_.clear();
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->queue.clear();
    shared_->done.clear();
    ++shared_->generation;
}

std::string coverUrl(const fs::path& cacheDir, System system, const std::string& fileName, const std::string& title) {
    // Discord takes addresses of up to 256 characters.
    auto fits = [](const std::string& url) { return url.size() <= 256 ? url : std::string(); };
    // The address the game's cover was downloaded from.
    {
        std::ifstream f(coverSourceFile(cacheDir, system, fileName));
        std::string url;
        if (std::getline(f, url) && url.rfind("https://", 0) == 0) return fits(url);
    }
    // Covers downloaded before Nintendall noted that: found again in the list of names.
    const char* sys = libretroSystem(system);
    if (!sys) return {};
    std::ifstream f(cacheDir / "names" / (std::string(sys) + ".txt"));
    Index idx;
    for (std::string line; std::getline(f, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) idx.names.push_back(line);
    }
    buildIndex(idx);
    const std::string best = bestMatch(idx, fileName, title);
    return best.empty() ? std::string()
                        : fits("https://thumbnails.libretro.com/" + urlEncode(sys) + "/Named_Boxarts/" + urlEncode(best) + ".png");
}
