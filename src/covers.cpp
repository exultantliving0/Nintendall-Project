#include "covers.hpp"

#include <algorithm>
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

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
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

constexpr int kMaxHeight = 400;  // covers are shrunk to this (they're shown about 200 pixels tall)
constexpr auto kRetryMissing = std::chrono::hours(24 * 7);  // look again for covers that weren't found
constexpr auto kIndexAge = std::chrono::hours(24 * 30);     // refresh libretro's list of names monthly

struct Request {
    std::string key;  // the game's path
    System system;
    std::string gameId, fileName, title;
    std::string region;  // where the game is from ("" = USA)
};

struct Image {
    std::string key;
    unsigned generation = 0;
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};

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
// GameTDB: art.gametdb.com/<platform>/cover/<region>/<ID>.<ext>

// GameTDB's cover sizes, best first. 3DS, DS and Wii U covers come in HQ (768 pixels wide) and
// M (about 400); GameCube and Wii fronts only in the small size (160 x 224).
std::vector<const char*> gameTdbSizes(System s) {
    if (s == System::N3DS || s == System::NDS || s == System::WiiU) return {"coverHQ", "coverM", "cover"};
    return {"cover"};
}

const char* gameTdbPlatform(System s, const char*& ext) {
    ext = "jpg";
    switch (s) {
    case System::GameCube:
    case System::Wii: ext = "png"; return "wii";
    case System::WiiU: return "wiiu";
    case System::NDS: return "ds";
    case System::N3DS: return "3ds";
    default: return nullptr;
    }
}

// The 4th character of an ID is the region; try that region's covers first.
std::vector<std::string> gameTdbRegions(const std::string& id) {
    std::vector<std::string> r;
    switch (id.size() >= 4 ? id[3] : 'E') {
    case 'J': r = {"JA"}; break;
    case 'P': case 'X': case 'Y': case 'Z': case 'L': case 'M': r = {"EN", "FR", "DE"}; break;
    case 'D': r = {"DE"}; break;
    case 'F': r = {"FR"}; break;
    case 'S': r = {"ES"}; break;
    case 'I': r = {"IT"}; break;
    case 'H': r = {"NL"}; break;
    case 'U': r = {"AU", "EN"}; break;
    case 'K': case 'Q': case 'T': r = {"KO"}; break;
    case 'W': r = {"ZH", "ZHTW"}; break;
    case 'R': r = {"RU"}; break;
    default: break;
    }
    for (const char* x : {"US", "EN", "JA"})
        if (std::find(r.begin(), r.end(), x) == r.end()) r.push_back(x);
    return r;
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

// The words of a game's name before its (tags), lowercase, without "the":
// "Legend of Zelda, The (USA)" and "The Legend of Zelda" both give {legend, of, zelda}.
std::vector<std::string> nameWords(const std::string& s) {
    std::vector<std::string> words;
    std::string w;
    auto flush = [&] {
        if (!w.empty() && w != "the") words.push_back(w);
        w.clear();
    };
    for (const char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c == '(' || c == '[') break;
        if (c == '&') {
            flush();
            w = "and";
            flush();
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
};

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

// Picks the best-looking entry: same name as the file, then the game's region (USA unless the
// game is from somewhere else), skipping hacks, betas and prototypes unless the file is one too.
int score(const std::string& name, const std::string& fileName, const std::string& region) {
    if (name == fileName) return 1 << 30;  // exactly the file's name: always the one
    auto has = [](const std::string& s, const char* t) { return s.find(t) != std::string::npos; };
    int sc = 0;
    // Other tags the file has, e.g. "(Rev 1)": the same tags in the name count.
    for (size_t a = fileName.find('('); a != std::string::npos; a = fileName.find('(', a + 1)) {
        const size_t b = fileName.find(')', a);
        if (b == std::string::npos) break;
        if (has(name, fileName.substr(a, b - a + 1).c_str())) sc += 20;
    }
    // The region: the game's own first, then the wider one (France -> Europe), then World, USA,
    // Europe, Japan.
    const std::vector<std::string> have = regionsInName(name);
    auto in = [&](const std::string& r) { return !r.empty() && std::find(have.begin(), have.end(), r) != have.end(); };
    const std::string want = region.empty() ? "USA" : region;
    if (in(want)) sc += 60;
    else if (in(parentRegion(want))) sc += 45;
    else if (in("World")) sc += 35;
    else if (in("USA")) sc += 30;
    else if (in("Europe")) sc += 20;
    else if (in("Japan")) sc += 10;
    for (const char* bad : {"[h", "[b", "[t", "(Beta", "(Proto", "(Demo", "(Sample", "(Pirate", "(Unl", "(Hack", "[a"})
        if (has(name, bad) && !has(fileName, bad)) sc -= 50;
    if (has(name, "(Rev") || has(name, "(Virtual Console")) sc -= 2;
    return sc * 1000 - int(std::min<size_t>(name.size(), 999));  // then the shortest name
}

std::string bestMatch(const Index& idx, const std::string& fileName, const std::string& title, const std::string& region) {
    std::vector<size_t> found;
    for (const std::string& n : {fileName, title}) {
        const auto it = idx.byKey.find(joinWords(nameWords(n)));
        if (it != idx.byKey.end()) {
            found = it->second;
            break;
        }
    }
    if (found.empty()) {
        // Close names: every word of the file's name is in the game's name, which has at most 2
        // more ("Pokemon Red" -> "Pokemon - Red Version"). Fewest extra words wins.
        const std::vector<std::string> want = nameWords(fileName);
        if (want.size() < 2) return {};
        const std::set<std::string> wantSet(want.begin(), want.end());
        size_t bestExtra = 3;
        for (size_t i = 0; i < idx.names.size(); ++i) {
            const std::vector<std::string> w = nameWords(idx.names[i]);
            if (w.size() < want.size() || w.size() - want.size() > 2 || w.size() - want.size() > bestExtra) continue;
            const std::set<std::string> have(w.begin(), w.end());
            if (!std::all_of(wantSet.begin(), wantSet.end(), [&](const std::string& x) { return have.count(x) > 0; }))
                continue;
            if (w.size() - want.size() < bestExtra) {
                bestExtra = w.size() - want.size();
                found.clear();
            }
            found.push_back(i);
        }
    }
    std::string best;
    int bestScore = 0;
    for (const size_t i : found) {
        const int sc = score(idx.names[i], fileName, region);
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
    std::chrono::steady_clock::time_point offlineUntil;  // no internet: wait a minute before trying again

    bool offline() const { return std::chrono::steady_clock::now() < offlineUntil; }
    void run();
    Image load(const Request& r);
    std::vector<uint8_t> download(const Request& r, bool& reached);
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
    const char* sys = libretroSystem(s);
    if (!sys) return empty;

    const fs::path file = cacheDir / "names" / (std::string(sys) + ".txt");
    std::vector<std::string> names;
    bool reached = true;
    if (olderThan(file, kIndexAge)) {
        std::vector<uint8_t> html;
        reached = false;
        if (get("https://thumbnails.libretro.com/" + urlEncode(sys) + "/Named_Boxarts/", html, reached) == 200)
            names = parseListing(html);
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
    if (names.empty() && !reached) return empty;  // offline: try again later
    Index& idx = indexes[s];
    idx.names = std::move(names);
    for (size_t i = 0; i < idx.names.size(); ++i) idx.byKey[joinWords(nameWords(idx.names[i]))].push_back(i);
    return idx;
}

std::vector<uint8_t> Covers::Shared::download(const Request& r, bool& reached) {
    std::vector<uint8_t> body;
    const char* ext = nullptr;
    if (const char* platform = gameTdbPlatform(r.system, ext); platform && !r.gameId.empty()) {
        for (const std::string& region : gameTdbRegions(r.gameId))
            for (const char* size : gameTdbSizes(r.system)) {
                const std::string url = std::string("https://art.gametdb.com/") + platform + "/" + size + "/" + region +
                                        "/" + r.gameId + "." + ext;
                if (get(url, body, reached) == 200 && isImage(body)) return body;
                if (!reached) return {};
            }
    }
    const char* sys = libretroSystem(r.system);
    if (!sys) return {};
    const std::string name = bestMatch(index(r.system), r.fileName, r.title, r.region);
    if (name.empty()) return {};
    const std::string url = "https://thumbnails.libretro.com/" + urlEncode(sys) + "/Named_Boxarts/" + urlEncode(name) + ".png";
    if (get(url, body, reached) == 200 && isImage(body)) return body;
    return {};
}

Image Covers::Shared::load(const Request& r) {
    Image img;
    // Covers found by ID on systems with big GameTDB covers are saved as "hq - <ID>"; small ones
    // saved by older versions ("id - <ID>") are replaced the first time.
    const bool hq = gameTdbSizes(r.system).size() > 1;
    // Games from outside the USA have the region in the name (their covers were picked by region).
    const std::string base = r.gameId.empty()
                                 ? "name - " + safeFileName(r.fileName) + (r.region.empty() ? "" : " - " + r.region)
                                 : (hq ? "hq - " : "id - ") + r.gameId;
    const fs::path dir = cacheDir / systemInfo(r.system).key;
    const fs::path have = dir / (base + ".img"), none = dir / (base + ".none");
    if (hq && !r.gameId.empty()) {
        std::error_code ec;
        fs::remove(dir / ("id - " + r.gameId + ".img"), ec);
        fs::remove(dir / ("id - " + r.gameId + ".none"), ec);
    }

    std::vector<uint8_t> bytes;
    std::error_code ec;
    if (!readFile(have, bytes)) {
        if (fs::exists(none, ec) && !olderThan(none, kRetryMissing)) return img;
        bool reached = false;
        bytes = download(r, reached);
        if (!bytes.empty()) writeFile(have, bytes.data(), bytes.size());
        else if (reached) writeFile(none, "", 0);  // the server answered: there's no cover (look again in a week)
    }
    if (bytes.empty()) return img;

    int w = 0, h = 0, n = 0;
    stbi_uc* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return img;
    // Shrink big covers (box filter) so hundreds of them fit in memory.
    const int scale = (h + kMaxHeight - 1) / kMaxHeight;
    if (scale > 1) {
        const int nw = w / scale, nh = h / scale;
        img.rgba.resize(size_t(nw) * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    unsigned sum = 0;
                    for (int dy = 0; dy < scale; ++dy)
                        for (int dx = 0; dx < scale; ++dx)
                            sum += px[(size_t(y * scale + dy) * w + size_t(x * scale + dx)) * 4 + size_t(c)];
                    img.rgba[(size_t(y) * nw + size_t(x)) * 4 + size_t(c)] = uint8_t(sum / unsigned(scale * scale));
                }
        img.w = nw;
        img.h = nh;
    } else {
        img.rgba.assign(px, px + size_t(w) * h * 4);
        img.w = w;
        img.h = h;
    }
    stbi_image_free(px);
    return img;
}

// ============================================================================

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

SDL_Texture* Covers::get(const LibraryGame& g) {
    const std::string key = g.path.u8string();
    const auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    textures_[key] = nullptr;
    if (shared_) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->queue.push_back({key, g.system, g.gameId, g.fileName, g.title, g.region});
        shared_->wake.notify_one();
    }
    return nullptr;
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
        if (img.generation != gen || img.rgba.empty()) continue;
        const auto it = textures_.find(img.key);
        if (it == textures_.end() || it->second) continue;
        SDL_Texture* t = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, img.w, img.h);
        if (!t) continue;
        SDL_UpdateTexture(t, nullptr, img.rgba.data(), img.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeLinear);
        it->second = t;
    }
}

void Covers::clear() {
    for (auto& [key, t] : textures_)
        if (t) SDL_DestroyTexture(t);
    textures_.clear();
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->queue.clear();
    shared_->done.clear();
    ++shared_->generation;
}
