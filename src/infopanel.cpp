#include "infopanel.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>

#include "covers.hpp"
#include "http.hpp"
#include "json.hpp"

namespace fs = std::filesystem;

namespace {

const Color kPanel{38, 38, 44};
const Color kShade{0, 0, 0, 150};
const Color kRed{230, 0, 18};
const Color kText{235, 235, 240};
const Color kBody{210, 210, 218};
const Color kDim{150, 150, 160};
const Color kButton{62, 62, 72};
const Color kButtonHover{82, 82, 94};
const Color kTabHover{52, 52, 60};
const Color kLine{56, 56, 64};

constexpr float kSlideMs = 180.0f;
constexpr auto kRetryNoManual = std::chrono::hours(24 * 7);  // look again for manuals that weren't found
constexpr int kPictureMaxW = 1600, kPictureMaxH = 1600;  // pictures are kept no bigger than this

const char* kTabNames[] = {"About", "Back of Box", "Manual"};

bool inside(const SDL_Rect& r, int x, int y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

// The width of the < and > areas beside a manual page shown big.
int bigSide(const SDL_Rect& area) { return std::max(40, area.w / 14); }

// ---------------------------------------------------------------------------
// Files

bool readFile(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}

// Writes a file whole (through a .part file), never over one that's there.
void writeNewFile(const fs::path& p, const void* data, size_t size) {
    std::error_code ec;
    if (fs::exists(p, ec)) return;
    fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.u8string() + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(static_cast<const char*>(data), std::streamsize(size));
        if (!f) return;
    }
    if (fs::exists(p, ec)) fs::remove(tmp, ec);
    else fs::rename(tmp, p, ec);
}

void writeFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream(p, std::ios::binary) << text;
}

std::string safeName(const std::string& s) {
    std::string out;
    for (const unsigned char c : s) {
        if (out.size() >= 120) break;
        out += (std::isalnum(c) || c >= 0x80 || std::strchr(" -_.()[]!,&'+", char(c))) ? char(c) : '_';
    }
    return out;
}

// ---------------------------------------------------------------------------
// Finding a game's manual on the Internet Archive (archive.org)

// How a console is written in the names of manual scans.
std::vector<std::string> consoleNames(System s) {
    switch (s) {
    case System::NES:
    case System::FDS: return {"nes", "nintendo entertainment system", "fds", "famicom disk system"};
    case System::SNES: return {"snes", "super nintendo", "super nes"};
    case System::N64: return {"n64", "nintendo 64"};
    case System::GB: return {"game boy", "gameboy", "gb", "dmg"};
    case System::GBC: return {"game boy color", "gameboy color", "gbc", "cgb"};
    case System::GBA: return {"game boy advance", "gameboy advance", "gba", "agb"};
    case System::NDS: return {"nintendo ds", "ds", "nds", "ntr"};
    case System::N3DS: return {"3ds", "nintendo 3ds", "ctr"};
    case System::GameCube: return {"gamecube", "game cube", "gc", "ngc", "gcn", "dol"};
    case System::Wii: return {"wii", "rvl"};
    case System::WiiU: return {"wii u", "wiiu", "wup"};
    default: return {};
    }
}

bool hasPhrase(const std::string& paddedWords, const std::vector<std::string>& phrases) {
    for (const std::string& p : phrases)
        if (paddedWords.find(" " + p + " ") != std::string::npos) return true;
    return false;
}

std::string joined(const std::vector<std::string>& words) {
    std::string s = " ";
    for (const std::string& w : words) s += w + " ";
    return s;
}

// How well a scan's name fits the game (higher is better; below 20 it isn't the game's manual).
int manualScore(const std::string& title, const std::string& language, const std::vector<std::string>& gameWords,
                System system) {
    const std::vector<std::string> words = nameWords(title);
    const std::string padded = joined(words);
    // Every word of the game's name must be in it.
    static const std::set<std::string> small = {"the", "a", "of", "and"};
    for (const std::string& w : gameWords)
        if (!small.count(w) && std::find(words.begin(), words.end(), w) == words.end()) return -1000;

    int score = 0;
    const std::vector<std::string> mine = consoleNames(system);
    score += hasPhrase(padded, mine) ? 50 : 20;
    for (const System other : {System::NES, System::SNES, System::N64, System::GB, System::GBC, System::GBA,
                               System::NDS, System::N3DS, System::GameCube, System::Wii, System::WiiU}) {
        if (other == system || (system == System::FDS && other == System::NES)) continue;
        if (hasPhrase(padded, consoleNames(other)) && !hasPhrase(padded, mine)) {
            score -= 40;  // for another console
            break;
        }
    }
    // Other languages, guides, magazines.
    std::string lower;
    for (const char c : title) lower += char(std::tolower(static_cast<unsigned char>(c)));
    static const char* notIt[] = {"japan", "(jp)", "jpn", "german", "deutsch", "spielanleitung", "french", "fran\xc3\xa7",
                                  "francais", "notice", "spanish", "espa\xc3\xb1", "italian", "italiano", "portug", "(pt",
                                  "dutch", "swedish", "chinese", "korean", "hong kong", "(fr)", "(de)", "(es)", "(it)",
                                  "guide book", "strategy", "player's guide", "players guide", "nintendo power",
                                  "magazine", "poster", "cartridge label"};
    for (const char* bad : notIt)
        if (lower.find(bad) != std::string::npos) score -= 60;
    if (system != System::FDS && lower.find("famicom") != std::string::npos) score -= 60;
    if (!language.empty() && language.find("eng") == std::string::npos && language.find("Eng") == std::string::npos)
        score -= 60;
    if (hasPhrase(padded, {"usa", "us", "ntsc"})) score += 5;
    // Words that aren't the game's: maybe another game of the series ("2" is a different game).
    static const std::set<std::string> fine = {
        "manual", "manuals", "booklet", "instruction", "instructions", "scans", "scan", "600dpi", "300dpi", "dpi",
        "hires", "high", "resolution", "usa", "us", "ntsc", "english", "eng", "en", "pdf", "original", "rev",
        "nintendo", "game", "boy", "pak", "for", "a", "of", "the", "and", "in", "by", "canada", "nes", "snes", "n64",
        "64", "gb", "gbc", "gba", "advance", "color", "colour", "wii", "u", "ds", "nds", "3ds", "gamecube", "cube",
        "gc", "ngc", "gcn", "dol", "rvl", "super", "entertainment", "system", "fds", "famicom", "disk", "dmg", "cgb",
        "agb", "ntr", "ctr", "wup", "version", "edition", "america", "inc", "complete", "full", "color"};
    static const std::set<std::string> numbers = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "ii",
                                                  "iii", "iv", "v", "vi", "vii", "viii", "ix", "x"};
    for (const std::string& w : std::set<std::string>(words.begin(), words.end())) {
        if (std::find(gameWords.begin(), gameWords.end(), w) != gameWords.end() || fine.count(w)) continue;
        if (w.size() == 4 && (w.rfind("19", 0) == 0 || w.rfind("20", 0) == 0) &&
            std::all_of(w.begin(), w.end(), [](char c) { return c >= '0' && c <= '9'; }))
            continue;  // a year
        score -= numbers.count(w) ? 40 : 15;
    }
    return score;
}

}  // namespace

// ============================================================================
// The download thread

struct InfoPanel::Shared {
    struct Job {
        unsigned gen = 0;
        bool info = true;  // the game's info, back of the box and manual; else one manual page
        LibraryGame game;
        fs::path backFolder;
        int page = 0;
        std::string pageUrl;
    };
    struct Result {
        enum Kind { Info, Back, Manual, Page } kind = Info;
        unsigned gen = 0;
        State state = State::None;
        GameInfo info;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string manualId, manualTitle;
        std::vector<std::string> pages;
        int page = 0;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::vector<Result> results;
    unsigned generation = 0;
    bool stop = false;
    fs::path infoDir, cacheDir;

    void run(bool pageJobs);  // two threads: one looks up games, the other loads manual pages
    void post(Result r) {
        std::lock_guard<std::mutex> lock(mutex);
        if (r.gen == generation) results.push_back(std::move(r));
    }
    bool current(unsigned gen) {
        std::lock_guard<std::mutex> lock(mutex);
        return gen == generation && !stop;
    }
    void doInfo(const Job& j);
    void doPage(const Job& j);
    // The manual's address on archive.org, its name and page pictures. State::None if there's none.
    State findManual(const LibraryGame& game, const GameInfo& info, bool known, std::string& id, std::string& title,
                     std::vector<std::string>& pages);
};

void InfoPanel::Shared::run(bool pageJobs) {
    auto mine = [&] { return std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) { return j.info != pageJobs; }); };
    for (;;) {
        Job j;
        {
            std::unique_lock<std::mutex> lock(mutex);
            wake.wait(lock, [&] { return stop || mine() != jobs.end(); });
            if (stop) return;
            const auto it = mine();
            j = std::move(*it);
            jobs.erase(it);
            if (j.gen != generation) continue;
        }
        if (j.info) doInfo(j);
        else doPage(j);
    }
}

void InfoPanel::Shared::doInfo(const Job& j) {
    // About: from the files shipped with Nintendall.
    Result info;
    info.kind = Result::Info;
    info.gen = j.gen;
    const bool known = findGameInfo(infoDir, j.game, info.info);
    info.state = known ? State::Ready : State::None;
    const GameInfo gi = info.info;
    post(std::move(info));

    // Back of the box: the picture in the Back folder (used as it is), or downloaded and kept there.
    Result back;
    back.kind = Result::Back;
    back.gen = j.gen;
    back.state = State::None;
    std::vector<uint8_t> bytes;
    const fs::path have = findCoverFile(j.backFolder, j.game.fileName);
    if (!have.empty()) {
        readFile(have, bytes);
    } else if (!gi.backUrl.empty()) {
        const int status = httpGet(gi.backUrl, bytes);
        if (status == 0) back.state = State::Offline;
        const bool png = bytes.size() > 8 && bytes[0] == 0x89 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G';
        const bool jpg = bytes.size() > 3 && bytes[0] == 0xFF && bytes[1] == 0xD8 && bytes[2] == 0xFF;
        if (status != 200 || (!png && !jpg)) {
            bytes.clear();
        } else {
            if (findCoverFile(j.backFolder, j.game.fileName).empty())
                writeNewFile(j.backFolder / fs::u8path(j.game.fileName + (png ? ".png" : ".jpg")), bytes.data(), bytes.size());
        }
    }
    if (!bytes.empty() && decodePicture(bytes, kPictureMaxW, kPictureMaxH, back.rgba, back.w, back.h))
        back.state = State::Ready;
    else
        back.rgba.clear();
    if (!current(j.gen)) return;
    post(std::move(back));

    // Manual
    Result manual;
    manual.kind = Result::Manual;
    manual.gen = j.gen;
    manual.state = findManual(j.game, gi, known, manual.manualId, manual.manualTitle, manual.pages);
    post(std::move(manual));
}

InfoPanel::State InfoPanel::Shared::findManual(const LibraryGame& game, const GameInfo& info, bool known,
                                               std::string& id, std::string& title, std::vector<std::string>& pages) {
    // Found before: "id TAB title", then a page picture's address per line. "-" = none was found.
    const fs::path note = cacheDir / "manuals" / systemInfo(game.system).key / (safeName(game.fileName) + ".txt");
    std::error_code ec;
    {
        std::ifstream f(note, std::ios::binary);
        std::string first;
        if (std::getline(f, first)) {
            if (first == "-") {
                const auto age = fs::file_time_type::clock::now() - fs::last_write_time(note, ec);
                if (!ec && age < kRetryNoManual) return State::None;
            } else if (const size_t tab = first.find('\t'); tab != std::string::npos) {
                id = first.substr(0, tab);
                title = first.substr(tab + 1);
                for (std::string line; std::getline(f, line);)
                    if (!line.empty()) pages.push_back(line);
                if (!pages.empty()) return State::Ready;
                id.clear();
                title.clear();
            }
        }
    }

    // Search the scans whose name has every word of the game's name and says it's a manual.
    const std::string name = known ? info.title : game.title;
    const std::vector<std::string> words = nameWords(name);
    std::string q;
    int used = 0;
    for (const std::string& w : words) {
        if (w == "the" || w == "a" || w == "of" || w == "and" || used == 8) continue;
        q += "title:(" + w + ") AND ";
        ++used;
    }
    if (used == 0) return State::None;
    q += "mediatype:texts AND (title:manual OR title:manuals OR title:booklet OR title:instruction OR "
         "title:instructions OR subject:manual)";
    std::vector<uint8_t> body;
    const int status = httpGet("https://archive.org/advancedsearch.php?q=" + urlEncode(q) +
                                   "&fl%5B%5D=identifier&fl%5B%5D=title&fl%5B%5D=language&rows=50&output=json",
                               body);
    if (status == 0) return State::Offline;
    Json found;
    if (status != 200 || !JsonParser(std::string(body.begin(), body.end())).parse(found)) return State::Offline;
    int best = 19;
    for (const Json& d : found["response"]["docs"].items) {
        const std::string ident = d["identifier"].s;
        const std::string t = d["title"].s;
        std::string lang = d["language"].s;
        for (const Json& l : d["language"].items) lang += l.s + " ";
        if (ident.empty() || t.empty()) continue;
        const int sc = manualScore(t, lang, words, game.system);
        if (sc > best) {
            best = sc;
            id = ident;
            title = t;
        }
    }
    if (id.empty()) {
        writeFile(note, "-\n");
        return State::None;
    }

    // Its pages (IIIF manifest). A scan with several copies of the booklet (two PDFs) shows the first.
    body.clear();
    const int st = httpGet("https://iiif.archive.org/iiif/3/" + id + "/manifest.json", body);
    Json manifest;
    if (st == 0) return State::Offline;
    if (st == 200 && JsonParser(std::string(body.begin(), body.end())).parse(manifest)) {
        std::string firstBook;
        for (const Json& canvas : manifest["items"].items) {
            for (const Json& annoPage : canvas["items"].items) {
                for (const Json& anno : annoPage["items"].items) {
                    const Json& b = anno["body"];
                    std::string url = b["service"].items.empty() ? std::string() : b["service"].items[0]["id"].s;
                    if (url.empty()) {
                        url = b["id"].s;
                        const size_t cut = url.find("/full/");
                        if (cut == std::string::npos) url.clear();
                        else url.erase(cut);
                    }
                    if (url.rfind("https://", 0) != 0) continue;
                    // ".../<id>%2F<book>_jp2.zip%2F...": which copy of the booklet the page is from.
                    std::string book;
                    if (const size_t a = url.find("%2F"); a != std::string::npos)
                        book = url.substr(a + 3, url.find("%2F", a + 3) - (a + 3));
                    if (pages.empty()) firstBook = book;
                    if (book == firstBook) pages.push_back(url);
                }
            }
        }
    }
    if (pages.empty()) {
        // Found, but its pages can't be shown here: it opens in the browser.
        pages.clear();
    }
    std::string text = id + "\t" + title + "\n";
    for (const std::string& p : pages) text += p + "\n";
    writeFile(note, text);
    return State::Ready;
}

void InfoPanel::Shared::doPage(const Job& j) {
    Result r;
    r.kind = Result::Page;
    r.gen = j.gen;
    r.page = j.page;
    r.state = State::None;
    std::vector<uint8_t> bytes;
    // The scan's own size: other sizes are made on request, and that's sometimes very slow.
    const int status = httpGet(j.pageUrl + "/full/max/0/default.jpg", bytes);
    if (status == 0) r.state = State::Offline;
    if (status != 200 || !decodePicture(bytes, kPictureMaxW, kPictureMaxH, r.rgba, r.w, r.h)) r.rgba.clear();
    if (!r.rgba.empty()) r.state = State::Ready;
    post(std::move(r));
}

// ============================================================================

InfoPanel::~InfoPanel() {
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    shared_->stop = true;
    shared_->wake.notify_all();
}

void InfoPanel::start(const fs::path& infoDir, const fs::path& cacheDir) {
    if (shared_) return;
    shared_ = std::make_shared<Shared>();
    shared_->infoDir = infoDir;
    shared_->cacheDir = cacheDir;
    // The threads keep their own reference: quitting during a download is safe.
    std::thread([s = shared_] { s->run(false); }).detach();
    std::thread([s = shared_] { s->run(true); }).detach();
}

void InfoPanel::freePictures() {
    if (back_) SDL_DestroyTexture(back_);
    back_ = nullptr;
    for (auto& [n, p] : pages_)
        if (p.tex) SDL_DestroyTexture(p.tex);
    pages_.clear();
}

void InfoPanel::shutdown() {
    freePictures();
    open_ = false;
    anim_ = 0.0f;
}

void InfoPanel::open(const LibraryGame& game, const fs::path& backFolder) {
    if (!shared_) return;
    freePictures();
    game_ = game;
    open_ = true;
    lastTick_ = SDL_GetTicks();
    tab_ = About;
    scroll_ = maxScroll_ = 0;
    infoState_ = backState_ = manualState_ = State::Loading;
    info_ = GameInfo{};
    manualId_.clear();
    manualTitle_.clear();
    manualPages_.clear();
    page_ = 0;
    big_ = false;
    wrapKey_.clear();
    std::lock_guard<std::mutex> lock(shared_->mutex);
    generation_ = ++shared_->generation;
    shared_->jobs.clear();
    shared_->results.clear();
    Shared::Job j;
    j.gen = generation_;
    j.game = game;
    j.backFolder = backFolder;
    shared_->jobs.push_back(std::move(j));
    shared_->wake.notify_all();
}

void InfoPanel::close() {
    if (!open_) return;
    open_ = false;
    big_ = false;
    lastTick_ = SDL_GetTicks();
    if (!shared_) return;
    std::lock_guard<std::mutex> lock(shared_->mutex);
    ++shared_->generation;  // stop what's still being looked for
    shared_->jobs.clear();
}

void InfoPanel::requestPage(int page) {
    if (!shared_ || page < 0 || page >= int(manualPages_.size()) || pages_.count(page)) return;
    pages_[page] = Page{};
    std::lock_guard<std::mutex> lock(shared_->mutex);
    Shared::Job j;
    j.gen = generation_;
    j.info = false;
    j.page = page;
    j.pageUrl = manualPages_[size_t(page)];
    shared_->jobs.push_back(std::move(j));
    shared_->wake.notify_all();
}

void InfoPanel::takeResults(SDL_Renderer* r) {
    if (!shared_) return;
    std::vector<Shared::Result> done;
    {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        done.swap(shared_->results);
    }
    auto texture = [&](const Shared::Result& res) -> SDL_Texture* {
        if (res.rgba.empty()) return nullptr;
        SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, res.w, res.h);
        if (!t) return nullptr;
        SDL_UpdateTexture(t, nullptr, res.rgba.data(), res.w * 4);
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_ScaleModeBest);
        return t;
    };
    for (Shared::Result& res : done) {
        if (res.gen != generation_ || !open_) continue;
        switch (res.kind) {
        case Shared::Result::Info:
            infoState_ = res.state;
            info_ = std::move(res.info);
            wrapKey_.clear();
            break;
        case Shared::Result::Back:
            backState_ = res.state;
            if (back_) SDL_DestroyTexture(back_);
            back_ = texture(res);
            backW_ = res.w;
            backH_ = res.h;
            if (!back_ && backState_ == State::Ready) backState_ = State::None;
            break;
        case Shared::Result::Manual:
            manualState_ = res.state;
            manualId_ = res.manualId;
            manualTitle_ = res.manualTitle;
            manualPages_ = std::move(res.pages);
            break;
        case Shared::Result::Page: {
            auto it = pages_.find(res.page);
            if (it == pages_.end()) break;
            it->second.tex = texture(res);
            it->second.w = res.w;
            it->second.h = res.h;
            it->second.failed = !it->second.tex;
            if (it->second.failed && res.state == State::Offline) pages_.erase(it);  // tried again when shown again
            break;
        }
        }
    }
}

// ============================================================================
// Layout and drawing

const std::vector<std::string>& InfoPanel::wrapped(Text& t, const std::string& text, int width) {
    const std::string key = std::to_string(width) + "/" + std::to_string(t.scale()) + "\n" + text;
    if (key == wrapKey_) return wrapLines_;
    wrapKey_ = key;
    wrapLines_.clear();
    size_t a = 0;
    while (a <= text.size()) {
        size_t nl = text.find('\n', a);
        if (nl == std::string::npos) nl = text.size();
        const std::string para = text.substr(a, nl - a);
        std::string line;
        size_t i = 0;
        while (i < para.size()) {
            size_t j = para.find(' ', i);
            if (j == std::string::npos) j = para.size();
            const std::string word = para.substr(i, j - i);
            i = j + 1;
            if (word.empty()) continue;
            const std::string next = line.empty() ? word : line + " " + word;
            if (line.empty() || t.width(next) <= width) line = next;
            else {
                wrapLines_.push_back(line);
                line = word;
            }
        }
        wrapLines_.push_back(line);  // an empty paragraph is a blank line
        a = nl + 1;
    }
    while (!wrapLines_.empty() && wrapLines_.back().empty()) wrapLines_.pop_back();
    return wrapLines_;
}

SDL_Rect InfoPanel::rect(Text& t, const SDL_Rect& area) const {
    const float s = t.scale();
    const int w = std::min(area.w - int(40 * s), std::clamp(int(area.w * 0.42f), int(380 * s), int(600 * s)));
    return {area.x, area.y, std::max(1, w), area.h};
}

InfoPanel::Layout InfoPanel::layout(Text& t, const SDL_Rect& area) {
    Layout L;
    const float s = L.s = t.scale();
    const SDL_Rect full = rect(t, area);
    const float e = 1.0f - std::pow(1.0f - std::clamp(anim_, 0.0f, 1.0f), 3.0f);  // ease out
    L.panel = full;
    L.panel.x = full.x - int(std::lround((1.0f - e) * full.w));
    const int pad = int(20 * s);
    const int x = L.panel.x + pad, w = L.panel.w - 2 * pad;
    const int cb = int(32 * s);
    L.close = {L.panel.x + L.panel.w - pad - cb, L.panel.y + pad - int(4 * s), cb, cb};

    // Title (up to 2 lines), then release date and players.
    const std::string title = infoState_ == State::Ready && !info_.title.empty() ? info_.title : game_.title;
    {
        std::vector<std::string> words;
        for (size_t a = 0; a < title.size();) {
            size_t b = title.find(' ', a);
            if (b == std::string::npos) b = title.size();
            if (b > a) words.push_back(title.substr(a, b - a));
            a = b + 1;
        }
        const int tw = w - cb - int(8 * s);
        std::string line;
        for (const std::string& word : words) {
            const std::string next = line.empty() ? word : line + " " + word;
            if (line.empty() || t.width(next, Text::Large) <= tw) line = next;
            else {
                L.titleLines.push_back(line);
                line = word;
            }
        }
        if (!line.empty()) L.titleLines.push_back(line);
        if (L.titleLines.size() > 2) {
            L.titleLines.resize(2);
            std::string& last = L.titleLines[1];
            while (!last.empty() && t.width(last + "...", Text::Large) > tw) {
                last.pop_back();
                while (!last.empty() && (static_cast<unsigned char>(last.back()) & 0xC0) == 0x80) last.pop_back();
            }
            last += "...";
        }
    }
    L.titleY = L.panel.y + pad;
    int y = L.titleY + int(L.titleLines.size()) * t.lineHeight(Text::Large);
    if (infoState_ == State::Ready) {
        static const char* months[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
        const std::string& d = info_.released;
        std::string when;
        if (d.size() >= 10 && d[4] == '-' && d[7] == '-') {
            const int m = std::atoi(d.substr(5, 2).c_str()), day = std::atoi(d.substr(8, 2).c_str());
            if (m >= 1 && m <= 12 && day >= 1) when = std::string(months[m - 1]) + " " + std::to_string(day) + ", " + d.substr(0, 4);
        }
        if (when.empty() && d.size() >= 4) when = d.substr(0, 4);
        if (!when.empty()) L.subtitle = "Released " + when;
        const int players = std::atoi(info_.players.c_str());
        if (players > 0) {
            if (!L.subtitle.empty()) L.subtitle += "  \xC2\xB7  ";
            L.subtitle += players == 1 ? std::string("1 player") : "Up to " + std::to_string(players) + " players";
        }
    }
    L.subtitleY = y + int(4 * s);
    if (!L.subtitle.empty()) y = L.subtitleY + t.lineHeight();
    y += int(14 * s);

    // Tabs
    const int tabH = t.lineHeight() + int(18 * s);
    int tx = x;
    for (const char* name : kTabNames) {
        const int tw = t.width(name) + int(28 * s);
        L.tabs.push_back({tx, y, tw, tabH});
        tx += tw;
    }
    y += tabH + int(14 * s);

    // Buttons under the manual: < page >, Open in Browser
    const int bh = t.lineHeight() + int(14 * s);
    const int by = L.panel.y + L.panel.h - pad - bh;
    L.prev = {x, by, bh, bh};
    L.next = {x + bh + int(110 * s), by, bh, bh};
    const int ow = t.width("Open in Browser") + int(28 * s);
    L.browser = {x + w - ow, by, ow, bh};
    const int bottom = tab_ == Manual ? by - int(12 * s) : L.panel.y + L.panel.h - pad - t.lineHeight() - int(10 * s);
    L.content = {x, y, w, std::max(0, bottom - y)};
    return L;
}

void InfoPanel::draw(Video& v, Text& t, const SDL_Rect& area, int mx, int my) {
    // Slide in / out.
    const Uint32 now = SDL_GetTicks();
    const float step = float(now - lastTick_) / kSlideMs;
    lastTick_ = now;
    anim_ = std::clamp(anim_ + (open_ ? step : -step), 0.0f, 1.0f);
    if (!shown()) {
        freePictures();
        return;
    }
    takeResults(v.renderer());
    const Layout L = layout(t, area);
    const float s = L.s;
    SDL_Renderer* r = v.renderer();

    // The rest of the home screen is shaded while the panel is out.
    Color shade = kShade;
    shade.a = Uint8(kShade.a * std::clamp(anim_, 0.0f, 1.0f));
    v.fillRect(area.x, area.y, area.w, area.h, shade);
    SDL_RenderSetClipRect(r, &area);
    v.fillRect(L.panel.x, L.panel.y, L.panel.w, L.panel.h, kPanel);
    v.fillRect(L.panel.x + L.panel.w, L.panel.y, std::max(2, int(3 * s)), L.panel.h, kRed);

    // Close button (an X)
    {
        const SDL_Rect& c = L.close;
        if (inside(c, mx, my)) v.fillRect(c.x, c.y, c.w, c.h, kButtonHover);
        const int len = c.w * 2 / 5, th = std::max(2, int(2 * s));
        const int cx = c.x + c.w / 2, cy = c.y + c.h / 2;
        for (int i = -len / 2; i <= len / 2; ++i) {
            v.fillRect(cx + i - th / 2, cy + i - th / 2, th, th, kText);
            v.fillRect(cx + i - th / 2, cy - i - th / 2, th, th, kText);
        }
    }
    int y = L.titleY;
    for (const std::string& line : L.titleLines) {
        t.draw(L.content.x, y, line, kText, Text::Large);
        y += t.lineHeight(Text::Large);
    }
    if (!L.subtitle.empty()) t.draw(L.content.x, L.subtitleY, L.subtitle, kDim);

    // Tabs
    for (size_t i = 0; i < L.tabs.size(); ++i) {
        const SDL_Rect& tr = L.tabs[i];
        const bool sel = int(i) == tab_;
        if (!sel && inside(tr, mx, my)) v.fillRect(tr.x, tr.y, tr.w, tr.h, kTabHover);
        if (sel) v.fillRect(tr.x, tr.y + tr.h - int(3 * s), tr.w, int(3 * s), kRed);
        const int tw = t.width(kTabNames[i]);
        t.draw(tr.x + (tr.w - tw) / 2, tr.y + (tr.h - t.lineHeight()) / 2, kTabNames[i], sel ? kText : kDim);
    }
    if (!L.tabs.empty())
        v.fillRect(L.content.x, L.tabs[0].y + L.tabs[0].h, L.content.w, std::max(1, int(s)), kLine);

    // A message in the middle of the page.
    auto message = [&](const std::vector<std::string>& lines) {
        int my2 = L.content.y + int(30 * s);
        for (const std::string& line : lines) {
            for (const std::string& part : wrapped(t, line, L.content.w)) {
                t.draw(L.content.x + (L.content.w - t.width(part)) / 2, my2, part, kDim);
                my2 += t.lineHeight();
            }
            my2 += int(6 * s);
            wrapKey_.clear();  // wrapped() remembers one text: don't keep these
        }
    };
    // A picture fitted to the page's width (scrolls if it's taller than the page) or height.
    auto picture = [&](SDL_Texture* tex, int w, int h, bool fitWidth) {
        if (w <= 0 || h <= 0) return;
        SDL_Rect dst;
        if (fitWidth) {
            dst.w = L.content.w;
            dst.h = int(std::lround(double(h) * dst.w / w));
        } else {
            const double f = std::min(double(L.content.w) / w, double(L.content.h) / h);
            dst.w = int(w * f);
            dst.h = int(h * f);
        }
        maxScroll_ = std::max(0, dst.h - L.content.h);
        scroll_ = std::clamp(scroll_, 0, maxScroll_);
        dst.x = L.content.x + (L.content.w - dst.w) / 2;
        dst.y = L.content.y - scroll_;
        pageRect_ = dst;
        SDL_RenderSetClipRect(r, &L.content);
        SDL_RenderCopy(r, tex, nullptr, &dst);
        SDL_RenderSetClipRect(r, &area);
    };

    maxScroll_ = 0;
    if (tab_ == About) {
        if (infoState_ == State::Loading) {
            message({"Loading..."});
        } else if (infoState_ != State::Ready || info_.overview.empty()) {
            message({"No description found for this game."});
        } else {
            const std::vector<std::string>& lines = wrapped(t, info_.overview, L.content.w);
            const int lh = t.lineHeight() + int(3 * s);
            maxScroll_ = std::max(0, int(lines.size()) * lh - L.content.h);
            scroll_ = std::clamp(scroll_, 0, maxScroll_);
            SDL_RenderSetClipRect(r, &L.content);
            int ly = L.content.y - scroll_;
            for (const std::string& line : lines) {
                if (ly + lh > L.content.y && ly < L.content.y + L.content.h && !line.empty())
                    t.draw(L.content.x, ly, line, kBody);
                ly += lh;
            }
            SDL_RenderSetClipRect(r, &area);
        }
        t.draw(L.content.x, L.panel.y + L.panel.h - int(20 * s) - t.lineHeight(), "Game info from TheGamesDB.net", kDim);
    } else if (tab_ == Back) {
        pageRect_ = {};
        if (back_) picture(back_, backW_, backH_, false);
        else if (backState_ == State::Loading) message({"Loading..."});
        else if (backState_ == State::Offline) message({"Couldn't download the back of the box.", "Check your internet connection."});
        else
            message({"No picture of the back of the box found.",
                     "You can add your own: a picture named like the game, in the Back folder inside this console's Box Covers folder."});
        t.draw(L.content.x, L.panel.y + L.panel.h - int(20 * s) - t.lineHeight(), "Pictures from TheGamesDB.net", kDim);
    } else {
        const bool have = manualState_ == State::Ready && !manualPages_.empty();
        if (manualState_ == State::Loading) message({"Looking for the manual..."});
        else if (manualState_ == State::Offline) message({"Couldn't reach the Internet Archive.", "Check your internet connection."});
        else if (manualState_ == State::None) message({"No manual found for this game."});
        else if (!have) message({"The manual can't be shown here.", "Click Open in Browser to read it."});
        else {
            requestPage(page_);
            requestPage(page_ + 1);  // ready when you turn the page
            const auto it = pages_.find(page_);
            pageRect_ = {};
            if (it != pages_.end() && it->second.tex) picture(it->second.tex, it->second.w, it->second.h, true);
            else if (it != pages_.end() && it->second.failed) message({"This page couldn't be loaded."});
            else message({"Loading page " + std::to_string(page_ + 1) + "..."});
            // Pages far from this one are let go.
            for (auto p = pages_.begin(); p != pages_.end();) {
                if (std::abs(p->first - page_) > 3 && p->second.tex) {
                    SDL_DestroyTexture(p->second.tex);
                    p = pages_.erase(p);
                } else {
                    ++p;
                }
            }
        }
        // < Page 3 of 24 >   Open in Browser
        if (have) {
            for (const bool nextBtn : {false, true}) {
                const SDL_Rect& b = nextBtn ? L.next : L.prev;
                const bool can = nextBtn ? page_ + 1 < int(manualPages_.size()) : page_ > 0;
                v.fillRect(b.x, b.y, b.w, b.h, can && inside(b, mx, my) ? kButtonHover : kButton);
                const int len = std::max(4, b.h / 5), th = std::max(2, int(2 * s));
                const int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
                for (int i = 0; i <= len; ++i) {  // a < or > made of two lines
                    const int dx = nextBtn ? len / 2 - i : i - len / 2;
                    v.fillRect(cx + dx - th / 2, cy - i - th / 2, th, th, can ? kText : kDim);
                    v.fillRect(cx + dx - th / 2, cy + i - th / 2, th, th, can ? kText : kDim);
                }
            }
            const std::string label = std::to_string(page_ + 1) + " / " + std::to_string(manualPages_.size());
            const int gap = L.next.x - (L.prev.x + L.prev.w);
            t.draw(L.prev.x + L.prev.w + (gap - t.width(label)) / 2, L.prev.y + (L.prev.h - t.lineHeight()) / 2, label, kText);
        }
        if (manualState_ == State::Ready && !manualId_.empty()) {
            const SDL_Rect& b = L.browser;
            v.fillRect(b.x, b.y, b.w, b.h, inside(b, mx, my) ? kButtonHover : kButton);
            t.draw(b.x + (b.w - t.width("Open in Browser")) / 2, b.y + (b.h - t.lineHeight()) / 2, "Open in Browser", kText);
        }
        if (!have) t.draw(L.content.x, L.panel.y + L.panel.h - int(20 * s) - t.lineHeight(), "Manuals from the Internet Archive", kDim);
    }
    // Scroll bar beside the page, when there's more than fits.
    if (maxScroll_ > 0 && L.content.h > 0) {
        const int track = L.content.h;
        const int thumb = std::max(int(30 * s), track * track / (track + maxScroll_));
        const int ty = L.content.y + (track - thumb) * scroll_ / maxScroll_;
        v.fillRect(L.content.x + L.content.w + int(8 * s), ty, std::max(2, int(4 * s)), thumb, {90, 90, 100});
    }
    SDL_RenderSetClipRect(r, nullptr);
    if (big_) drawBig(v, t, area, mx, my);
}

// The manual page over the whole home screen, as big as fits, with < > at the sides.
void InfoPanel::drawBig(Video& v, Text& t, const SDL_Rect& area, int mx, int my) {
    const bool manual = tab_ == Manual;
    const auto it = pages_.find(page_);
    Page shown;
    if (!manual) shown = {back_, backW_, backH_, false};
    else if (it != pages_.end()) shown = it->second;
    const float s = t.scale();
    v.fillRect(area.x, area.y, area.w, area.h, {12, 12, 14, 250});
    const int m = int(16 * s), side = bigSide(area);
    const SDL_Rect room{area.x + side, area.y + m, area.w - 2 * side, area.h - 2 * m - t.lineHeight() - int(8 * s)};
    if (shown.tex && shown.w > 0 && shown.h > 0 && room.w > 0 && room.h > 0) {
        const double f = std::min(double(room.w) / shown.w, double(room.h) / shown.h);
        const SDL_Rect dst{room.x + (room.w - int(shown.w * f)) / 2, room.y + (room.h - int(shown.h * f)) / 2,
                           int(shown.w * f), int(shown.h * f)};
        SDL_RenderCopy(v.renderer(), shown.tex, nullptr, &dst);
    } else if (manual) {
        const std::string msg = it != pages_.end() && it->second.failed ? "This page couldn't be loaded."
                                                                        : "Loading page " + std::to_string(page_ + 1) + "...";
        t.draw(area.x + (area.w - t.width(msg)) / 2, area.y + area.h / 2, msg, kDim);
    }
    // < and > down the sides.
    for (const bool nextBtn : {false, true}) {
        const bool can = manual && (nextBtn ? page_ + 1 < int(manualPages_.size()) : page_ > 0);
        if (!can) continue;
        const SDL_Rect b{nextBtn ? area.x + area.w - side : area.x, area.y, side, area.h};
        if (inside(b, mx, my)) v.fillRect(b.x, b.y, b.w, b.h, {255, 255, 255, 18});
        const int len = int(14 * s), th = std::max(2, int(3 * s));
        const int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
        for (int i = 0; i <= len; ++i) {
            const int dx = nextBtn ? len / 2 - i : i - len / 2;
            v.fillRect(cx + dx - th / 2, cy - i - th / 2, th, th, kText);
            v.fillRect(cx + dx - th / 2, cy + i - th / 2, th, th, kText);
        }
    }
    const std::string back = "Click or press Esc to go back";
    const std::string label = manual ? "Page " + std::to_string(page_ + 1) + " of " +
                                           std::to_string(manualPages_.size()) + "   \xC2\xB7   " + back
                                     : back;
    t.draw(area.x + (area.w - t.width(label)) / 2, area.y + area.h - m - t.lineHeight(), label, kDim);
}

bool InfoPanel::bigEvent(const SDL_Event& e, int mx, int my, const SDL_Rect& area) {
    switch (e.type) {
    case SDL_MOUSEBUTTONDOWN: {
        if (!inside(area, mx, my)) return false;
        const int edge = bigSide(area);
        const bool manual = tab_ == Manual;
        if (manual && e.button.button == SDL_BUTTON_LEFT && mx < area.x + edge && page_ > 0) setPage(page_ - 1);
        else if (manual && e.button.button == SDL_BUTTON_LEFT && mx >= area.x + area.w - edge &&
                 page_ + 1 < int(manualPages_.size()))
            setPage(page_ + 1);
        else big_ = false;
        return true;
    }
    case SDL_MOUSEBUTTONUP:
    case SDL_MOUSEWHEEL:
        if (e.type == SDL_MOUSEWHEEL) {
            const int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
            if (dy != 0) setPage(page_ + (dy < 0 ? 1 : -1));
        }
        return true;
    case SDL_KEYDOWN:
        if (e.key.keysym.mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) return false;
        switch (e.key.keysym.sym) {
        case SDLK_LEFT:
        case SDLK_PAGEUP:
        case SDLK_UP: setPage(page_ - 1); break;
        case SDLK_RIGHT:
        case SDLK_PAGEDOWN:
        case SDLK_DOWN:
        case SDLK_SPACE: setPage(page_ + 1); break;
        case SDLK_HOME: setPage(0); break;
        case SDLK_END: setPage(int(manualPages_.size()) - 1); break;
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_i:
        case SDLK_APPLICATION: big_ = false; break;
        default: return false;
        }
        return true;
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: setPage(page_ - 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: setPage(page_ + 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: break;
        default: big_ = false; break;  // B, A...
        }
        return true;
    case SDL_CONTROLLERAXISMOTION: return true;
    default: return false;
    }
}

// ============================================================================
// Mouse, keyboard and controller

void InfoPanel::setTab(int tab) {
    tab_ = (tab % 3 + 3) % 3;
    big_ = false;
    scroll_ = 0;
}

void InfoPanel::setPage(int page) {
    if (manualPages_.empty()) return;
    const int p = std::clamp(page, 0, int(manualPages_.size()) - 1);
    if (p != page_) scroll_ = 0;
    page_ = p;
}

void InfoPanel::scrollBy(int dy) { scroll_ = std::clamp(scroll_ + dy, 0, maxScroll_); }

bool InfoPanel::handleEvent(const SDL_Event& e, int mx, int my, Text& t, const SDL_Rect& area) {
    if (!open_) return false;
    if (big_) return bigEvent(e, mx, my, area);
    const Layout L = layout(t, area);
    const int lineStep = int(60 * L.s), pageStep = std::max(lineStep, L.content.h - lineStep);
    const bool manualShown = tab_ == Manual && manualState_ == State::Ready && !manualPages_.empty();

    switch (e.type) {
    case SDL_MOUSEBUTTONDOWN:
        if (!inside(area, mx, my)) return false;
        if (!inside(L.panel, mx, my)) {  // a click beside the panel closes it
            if (e.button.button == SDL_BUTTON_LEFT) close();
            return e.button.button == SDL_BUTTON_LEFT;
        }
        if (e.button.button == SDL_BUTTON_RIGHT) {
            close();
            return true;
        }
        if (e.button.button != SDL_BUTTON_LEFT) return true;
        if (inside(L.close, mx, my)) {
            close();
            return true;
        }
        for (size_t i = 0; i < L.tabs.size(); ++i)
            if (inside(L.tabs[i], mx, my)) setTab(int(i));
        if (manualShown && inside(L.prev, mx, my)) setPage(page_ - 1);
        if (manualShown && inside(L.next, mx, my)) setPage(page_ + 1);
        if (tab_ == Manual && manualState_ == State::Ready && !manualId_.empty() && inside(L.browser, mx, my))
            SDL_OpenURL(("https://archive.org/details/" + manualId_).c_str());
        if ((manualShown || (tab_ == Back && back_)) && inside(pageRect_, mx, my) && inside(L.content, mx, my)) big_ = true;
        return true;
    case SDL_MOUSEBUTTONUP:
        return inside(L.panel, mx, my);
    case SDL_MOUSEWHEEL: {
        const int dy = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        if (inside(L.panel, mx, my)) scrollBy(-dy * lineStep);
        return true;
    }
    case SDL_KEYDOWN: {
        const SDL_Keymod mod = SDL_Keymod(e.key.keysym.mod);
        if (mod & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) return false;  // the program's shortcuts still work
        switch (e.key.keysym.sym) {
        case SDLK_ESCAPE:
        case SDLK_BACKSPACE:
        case SDLK_i:
        case SDLK_APPLICATION: close(); return true;
        case SDLK_TAB: setTab(tab_ + ((mod & KMOD_SHIFT) ? -1 : 1)); return true;
        case SDLK_LEFT:
            if (manualShown) setPage(page_ - 1);
            else setTab(tab_ - 1);
            return true;
        case SDLK_RIGHT:
            if (manualShown) setPage(page_ + 1);
            else setTab(tab_ + 1);
            return true;
        case SDLK_UP: scrollBy(-lineStep); return true;
        case SDLK_DOWN: scrollBy(lineStep); return true;
        case SDLK_PAGEUP: scrollBy(-pageStep); return true;
        case SDLK_PAGEDOWN:
        case SDLK_SPACE: scrollBy(pageStep); return true;
        case SDLK_HOME: scroll_ = 0; return true;
        case SDLK_END: scroll_ = maxScroll_; return true;
        case SDLK_RETURN:
        case SDLK_KP_ENTER: big_ = manualShown || (tab_ == Back && back_); return true;
        default: return false;  // F11 (full screen)...
        }
    }
    case SDL_CONTROLLERBUTTONDOWN:
        switch (e.cbutton.button) {
        case SDL_CONTROLLER_BUTTON_B:
        case SDL_CONTROLLER_BUTTON_Y:
        case SDL_CONTROLLER_BUTTON_BACK: close(); break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: setTab(tab_ - 1); break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: setTab(tab_ + 1); break;
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            if (manualShown) setPage(page_ - 1);
            else setTab(tab_ - 1);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            if (manualShown) setPage(page_ + 1);
            else setTab(tab_ + 1);
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_UP: scrollBy(-lineStep); break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: scrollBy(lineStep); break;
        case SDL_CONTROLLER_BUTTON_A: big_ = manualShown || (tab_ == Back && back_); break;
        default: break;
        }
        return true;
    case SDL_CONTROLLERAXISMOTION:
        return true;
    default:
        return false;
    }
}
