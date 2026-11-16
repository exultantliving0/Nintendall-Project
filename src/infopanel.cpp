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
#include "filedialog.hpp"
#include "http.hpp"
#include "json.hpp"
#include "library.hpp"
#include "pdfpages.hpp"

namespace fs = std::filesystem;

namespace {

const Color kPanel{38, 38, 44};
const Color kShade{0, 0, 0, 150};
const Color kRed{230, 0, 18};
const Color kText{235, 235, 240};
const Color kDim{150, 150, 160};
const Color kGood{120, 200, 130};
const Color kBad{240, 120, 110};
const Color kButton{62, 62, 72};
const Color kButtonHover{82, 82, 94};
const Color kTabHover{52, 52, 60};
const Color kLine{56, 56, 64};

constexpr float kSlideMs = 180.0f;
constexpr auto kRetryNoManual = std::chrono::hours(24 * 7);  // look again for manuals that weren't found
constexpr int kPictureMaxW = 1600, kPictureMaxH = 1600;  // pictures are kept no bigger than this
constexpr uint64_t kMaxPdf = 20ull << 20;  // bigger PDFs on the Internet Archive are shown page by page instead
constexpr uint64_t kMaxCache = 1ull << 30, kCacheAfterTrim = 800ull << 20;  // downloaded manuals kept (bytes)
constexpr int kPageThreads = 3;  // pages downloaded at the same time

const char* kTabNames[] = {"Back of Box", "Manual"};
const char* kButtonNames[] = {"Add Custom Cover", "Add Custom Back Cover", "Import Game Manual"};

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

std::vector<std::string> splitTabs(const std::string& line) {
    std::vector<std::string> parts;
    size_t a = 0;
    for (;;) {
        const size_t b = line.find('\t', a);
        parts.push_back(line.substr(a, b == std::string::npos ? std::string::npos : b - a));
        if (b == std::string::npos) return parts;
        a = b + 1;
    }
}

bool isJpeg(const std::vector<uint8_t>& b) { return b.size() > 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF; }
bool isPng(const std::vector<uint8_t>& b) { return b.size() > 8 && b[0] == 0x89 && b[1] == 'P' && b[2] == 'N' && b[3] == 'G'; }

// Keeps the downloaded manuals under kMaxCache: the ones looked at longest ago go first.
void trimCache(const fs::path& manuals, const fs::path& keep) {
    std::error_code ec;
    struct Item {
        fs::path path;
        fs::file_time_type time;
        uint64_t size;
    };
    std::vector<Item> items;
    uint64_t total = 0;
    for (fs::directory_iterator c(manuals, ec), end; !ec && c != end; c.increment(ec)) {
        if (!c->is_directory(ec)) continue;
        for (fs::directory_iterator it(c->path(), ec), e2; !ec && it != e2; it.increment(ec)) {
            const fs::path p = it->path();
            uint64_t size = 0;
            if (it->is_directory(ec)) {  // a manual's pages
                for (fs::directory_iterator f(p, ec), e3; !ec && f != e3; f.increment(ec)) size += f->file_size(ec);
            } else if (p.extension() == ".pdf") {
                size = it->file_size(ec);
            } else {
                continue;
            }
            total += size;
            items.push_back({p, fs::last_write_time(p, ec), size});
        }
    }
    if (total <= kMaxCache) return;
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.time < b.time; });
    for (const Item& i : items) {
        if (total <= kCacheAfterTrim) break;
        if (i.path == keep || i.path == keep.parent_path() / keep.stem()) continue;
        fs::remove_all(i.path, ec);
        total -= i.size;
    }
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
                                  "magazine", "poster", "cartridge label", "quick start", "taiwan", "(kor", "_kor"};
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
// The download threads

struct InfoPanel::Shared {
    struct Job {
        unsigned gen = 0;
        enum Kind { InfoJob, ManualJob, PageJob } kind = InfoJob;  // info and back of the box; the manual; a page
        LibraryGame game;
        GameInfo info;  // ManualJob: what's known about the game
        bool known = false;
        fs::path coverFolder;
        int page = 0;
        std::string pageUrl;  // a page on the Internet Archive ("" = in the manual's PDF)
        fs::path pageFile;    // where that page is kept once downloaded
    };
    struct Result {
        enum Kind { Info, Back, Manual, Progress, Page } kind = Info;
        unsigned gen = 0;
        State state = State::None;
        GameInfo info;
        std::vector<uint8_t> rgba;
        int w = 0, h = 0;
        std::string from, link;
        int pages = 0;
        std::vector<std::string> urls;
        fs::path pageFolder, file;
        int page = 0;  // Page: which; Progress: percent
    };
    // The manual found: where it came from and its pages.
    struct Found {
        std::string from, link;
        std::vector<std::string> urls;  // the Internet Archive's page pictures, or...
        fs::path file;                  // ...the PDF in the Manuals folder (its pages are JPEG pictures, or
        std::vector<uint8_t> pdf;       //    they can't be shown here: pdfPages empty)
        std::vector<PdfPicture> pdfPages;
    };

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::vector<Result> results;
    unsigned generation = 0;
    bool stop = false;
    fs::path infoDir, cacheDir;
    // The PDF of the manual being shown (its pages are read from it).
    unsigned pdfGen = 0;
    std::vector<uint8_t> pdf;
    std::vector<PdfPicture> pdfPages;

    void run(bool pageJobs);  // one thread looks up games, the others find manuals and load their pages
    void post(Result r) {
        std::lock_guard<std::mutex> lock(mutex);
        if (r.gen == generation) results.push_back(std::move(r));
    }
    bool current(unsigned gen) {
        std::lock_guard<std::mutex> lock(mutex);
        return gen == generation && !stop;
    }
    // The game's manual in the Manuals folder: "Box Covers and Manuals/<console>/Manuals/<game file name>.pdf".
    static fs::path manualFile(const Job& j) {
        return j.coverFolder / "Manuals" / fs::u8path(j.game.fileName + ".pdf");
    }
    void doInfo(const Job& j);
    void doManual(const Job& j);
    void doPage(const Job& j);
    State findManual(const Job& j, const GameInfo& info, bool known, Found& found);
    // Downloads a PDF into `file` and finds its pages. False if it couldn't (offline: `offline` set).
    bool getPdf(const Job& j, const std::string& url, const fs::path& local, Found& found, bool& offline);
    // Downloads the pages not downloaded yet; when all are there, makes them into a PDF in the Manuals folder.
    void downloadAllPages(const Job& j, const std::vector<std::string>& urls, const fs::path& pageFolder,
                          const fs::path& local, const Found& found);
};

void InfoPanel::Shared::run(bool pageJobs) {
    auto mine = [&] { return std::find_if(jobs.begin(), jobs.end(), [&](const Job& j) { return (j.kind != Job::InfoJob) == pageJobs; }); };
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
        if (j.kind == Job::InfoJob) doInfo(j);
        else if (j.kind == Job::ManualJob) doManual(j);
        else doPage(j);
    }
}

void InfoPanel::Shared::doInfo(const Job& j) {
    // The game's name, release date and players: from the files shipped with Nintendall.
    Result info;
    info.kind = Result::Info;
    info.gen = j.gen;
    const bool known = findGameInfo(infoDir, j.game, info.info);
    info.state = known ? State::Ready : State::None;
    const GameInfo gi = info.info;
    post(std::move(info));

    // The manual is looked for at the same time (by another thread).
    {
        Job m = j;
        m.kind = Job::ManualJob;
        m.info = gi;
        m.known = known;
        std::lock_guard<std::mutex> lock(mutex);
        if (j.gen != generation) return;
        jobs.push_front(std::move(m));
        wake.notify_all();
    }

    // Back of the box: the picture in the Back folder (used as it is), or downloaded and kept there.
    const fs::path backFolder = j.coverFolder / "Back";
    Result back;
    back.kind = Result::Back;
    back.gen = j.gen;
    back.state = State::None;
    std::vector<uint8_t> bytes;
    const fs::path have = findCoverFile(backFolder, j.game.fileName);
    if (!have.empty()) {
        readFile(have, bytes);
    } else if (!gi.backUrl.empty()) {
        const int status = httpGet(gi.backUrl, bytes);
        if (status == 0) back.state = State::Offline;
        if (status != 200 || (!isPng(bytes) && !isJpeg(bytes))) {
            bytes.clear();
        } else if (findCoverFile(backFolder, j.game.fileName).empty()) {
            writeNewFile(backFolder / fs::u8path(j.game.fileName + (isPng(bytes) ? ".png" : ".jpg")), bytes.data(),
                         bytes.size());
        }
    }
    if (!bytes.empty() && decodePicture(bytes, kPictureMaxW, kPictureMaxH, back.rgba, back.w, back.h))
        back.state = State::Ready;
    else
        back.rgba.clear();
    if (!current(j.gen)) return;
    post(std::move(back));
}

void InfoPanel::Shared::doManual(const Job& j) {
    Found found;
    Result manual;
    manual.kind = Result::Manual;
    manual.gen = j.gen;
    manual.state = findManual(j, j.info, j.known, found);
    manual.from = found.from;
    manual.link = found.link;
    manual.file = found.file;
    const fs::path local = manualFile(j);
    const fs::path pageFolder = cacheDir / "manuals" / systemInfo(j.game.system).key / safeName(j.game.fileName);
    std::vector<std::string> urls;
    if (manual.state == State::Ready) {
        if (!found.pdfPages.empty()) {
            manual.pages = int(found.pdfPages.size());
            std::lock_guard<std::mutex> lock(mutex);
            pdfGen = j.gen;
            pdf = std::move(found.pdf);
            pdfPages = std::move(found.pdfPages);
        } else if (!found.urls.empty()) {
            manual.pages = int(found.urls.size());
            manual.urls = urls = found.urls;
            manual.pageFolder = pageFolder;
        }
    }
    post(std::move(manual));
    trimCache(cacheDir / "manuals", pageFolder);
    if (!urls.empty()) downloadAllPages(j, urls, pageFolder, local, found);
}

// A PDF of JPEG pictures, one per page (each page the picture's size at 150 dpi).
static std::vector<uint8_t> makePdf(const std::vector<std::vector<uint8_t>>& jpegs) {
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    std::vector<size_t> offsets(3 + jpegs.size() * 3, 0);
    auto begin = [&](size_t n) {
        offsets[n] = out.size();
        out += std::to_string(n) + " 0 obj\n";
    };
    begin(1);
    out += "<< /Type /Catalog /Pages 2 0 R >>\nendobj\n";
    begin(2);
    out += "<< /Type /Pages /Count " + std::to_string(jpegs.size()) + " /Kids [";
    for (size_t i = 0; i < jpegs.size(); ++i) out += std::to_string(3 + i * 3) + " 0 R ";
    out += "] >>\nendobj\n";
    for (size_t i = 0; i < jpegs.size(); ++i) {
        const std::vector<uint8_t>& jpg = jpegs[i];
        // The picture's size and colors, from its frame header.
        int w = 0, h = 0, comps = 3;
        for (size_t at = 2; at + 9 < jpg.size();) {
            if (jpg[at] != 0xFF) break;
            const uint8_t m = jpg[at + 1];
            const size_t len = size_t(jpg[at + 2]) << 8 | jpg[at + 3];
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
                h = jpg[at + 5] << 8 | jpg[at + 6];
                w = jpg[at + 7] << 8 | jpg[at + 8];
                comps = jpg[at + 9];
                break;
            }
            at += 2 + len;
        }
        if (w <= 0 || h <= 0) w = h = 1;
        const double pw = w * 72.0 / 150.0, ph = h * 72.0 / 150.0;
        char size[64];
        std::snprintf(size, sizeof size, "%.2f %.2f", pw, ph);
        const size_t page = 3 + i * 3, content = page + 1, image = page + 2;
        begin(page);
        out += std::string("<< /Type /Page /Parent 2 0 R /MediaBox [0 0 ") + size + "] /Resources << /XObject << /Im0 " +
               std::to_string(image) + " 0 R >> >> /Contents " + std::to_string(content) + " 0 R >>\nendobj\n";
        char draw[128];
        std::snprintf(draw, sizeof draw, "q %.2f 0 0 %.2f 0 0 cm /Im0 Do Q", pw, ph);
        begin(content);
        out += "<< /Length " + std::to_string(std::strlen(draw)) + " >>\nstream\n" + draw + "\nendstream\nendobj\n";
        begin(image);
        out += "<< /Type /XObject /Subtype /Image /Width " + std::to_string(w) + " /Height " + std::to_string(h) +
               " /ColorSpace " + (comps == 1 ? "/DeviceGray" : comps == 4 ? "/DeviceCMYK" : "/DeviceRGB") +
               " /BitsPerComponent 8 /Filter /DCTDecode /Length " + std::to_string(jpg.size()) + " >>\nstream\n";
        out.append(reinterpret_cast<const char*>(jpg.data()), jpg.size());
        out += "\nendstream\nendobj\n";
    }
    const size_t xref = out.size();
    out += "xref\n0 " + std::to_string(offsets.size()) + "\n0000000000 65535 f \n";
    for (size_t n = 1; n < offsets.size(); ++n) {
        char line[32];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", offsets[n]);
        out += line;
    }
    out += "trailer\n<< /Size " + std::to_string(offsets.size()) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) +
           "\n%%EOF\n";
    return std::vector<uint8_t>(out.begin(), out.end());
}

// Moves a file (also to another drive: copied, then removed). Never over a file that's there.
static bool moveNewFile(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    if (fs::exists(to, ec)) return false;
    fs::create_directories(to.parent_path(), ec);
    fs::rename(from, to, ec);
    if (!ec) return true;
    const fs::path tmp = to.u8string() + ".part";
    if (!fs::copy_file(from, tmp, fs::copy_options::overwrite_existing, ec) || fs::exists(to, ec)) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::rename(tmp, to, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    fs::remove(from, ec);
    return true;
}

void InfoPanel::Shared::downloadAllPages(const Job& j, const std::vector<std::string>& urls, const fs::path& pageFolder,
                                         const fs::path& local, const Found& found) {
    std::vector<std::vector<uint8_t>> jpegs(urls.size());
    for (size_t i = 0; i < urls.size(); ++i) {
        if (!current(j.gen)) return;  // the panel was closed: the rest is downloaded next time
        const fs::path file = pageFolder / (std::to_string(i + 1) + ".jpg");
        if (readFile(file, jpegs[i]) && isJpeg(jpegs[i])) continue;
        jpegs[i].clear();
        for (const std::string& url : splitTabs(urls[i])) {
            if (url.empty()) continue;
            std::vector<uint8_t> bytes;
            if (httpGet(url, bytes) == 200 && isJpeg(bytes)) {
                writeNewFile(file, bytes.data(), bytes.size());
                jpegs[i] = std::move(bytes);
                break;
            }
            if (!current(j.gen)) return;
        }
        if (jpegs[i].empty()) return;  // couldn't: tried again next time
    }
    // Every page is there: the manual goes into the Manuals folder as one PDF.
    std::error_code ec;
    if (!fs::exists(local, ec)) {
        const std::vector<uint8_t> made = makePdf(jpegs);
        writeNewFile(local, made.data(), made.size());
    }
    if (!fs::is_regular_file(local, ec)) return;
    fs::remove_all(pageFolder, ec);
    // From now on its pages are read from the PDF.
    Result r;
    r.kind = Result::Manual;
    r.gen = j.gen;
    r.state = State::Ready;
    r.from = found.from;
    r.link = found.link;
    r.file = local;
    std::vector<uint8_t> bytes;
    std::vector<PdfPicture> pages;
    if (!readFile(local, bytes) || !pdfJpegPages(bytes, pages) || pages.size() != urls.size()) return;
    r.pages = int(pages.size());
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (j.gen != generation) return;
        pdfGen = j.gen;
        pdf = std::move(bytes);
        pdfPages = std::move(pages);
    }
    post(std::move(r));
}

bool InfoPanel::Shared::getPdf(const Job& j, const std::string& url, const fs::path& local, Found& found, bool& offline) {
    // Downloaded into Nintendall's own files first; kept in the Manuals folder only if its pages can be shown.
    std::error_code ec;
    const fs::path tmp = cacheDir / "manuals" / "downloads" / (safeName(j.game.fileName) + ".pdf");
    fs::create_directories(tmp.parent_path(), ec);
    int last = -1;
    const int status = httpDownload(url, tmp, [&](uint64_t done, uint64_t total) {
        if (total > 0) {
            const int pct = int(done * 100 / total);
            if (pct != last) {
                last = pct;
                Result r;
                r.kind = Result::Progress;
                r.gen = j.gen;
                r.page = pct;
                post(std::move(r));
            }
        }
        return current(j.gen);
    });
    if (status == 0 && current(j.gen)) offline = true;
    if (status == 200 && readFile(tmp, found.pdf) && pdfJpegPages(found.pdf, found.pdfPages) &&
        (fs::exists(local, ec) || moveNewFile(tmp, local))) {
        fs::remove(tmp, ec);
        found.file = local;
        return true;
    }
    found.pdf.clear();
    found.pdfPages.clear();
    fs::remove(tmp, ec);
    return false;
}

InfoPanel::State InfoPanel::Shared::findManual(const Job& j, const GameInfo& info, bool known, Found& found) {
    const LibraryGame& game = j.game;
    const fs::path dir = cacheDir / "manuals" / systemInfo(game.system).key;
    const fs::path note = dir / (safeName(game.fileName) + ".txt");
    const fs::path local = manualFile(j);
    std::error_code ec;
    bool offline = false;

    // Where the manual came from (Nintendall's own notes):
    //   "pdf TAB from TAB web page TAB the PDF's address"
    //   "pages TAB from TAB web page", then a page picture's address per line
    //   "-" = none was found (looked for again after a week)
    std::vector<std::string> head;
    std::vector<std::string> lines;
    {
        std::ifstream f(note, std::ios::binary);
        std::string first;
        if (std::getline(f, first)) head = splitTabs(first);
        for (std::string line; std::getline(f, line);)
            if (!line.empty()) lines.push_back(line);
    }
    const bool pdfNote = head.size() == 4 && head[0] == "pdf";
    const bool pagesNote = head.size() == 3 && head[0] == "pages" && !head[2].empty() && !lines.empty();

    // Test builds of 1.3.8 kept downloaded manuals with Nintendall's own files: into the Manuals folder.
    const fs::path older = dir / (safeName(game.fileName) + ".pdf");
    if (fs::is_regular_file(older, ec) && !fs::exists(local, ec)) moveNewFile(older, local);

    // A manual in the Manuals folder (downloaded before, or put there by the player) is used as it is.
    if (fs::is_regular_file(local, ec)) {
        if (pdfNote || pagesNote) {
            found.from = head[1];
            found.link = head[2];
        } else {
            found.from = "your Manuals folder";
        }
        found.file = local;
        if (!readFile(local, found.pdf) || !pdfJpegPages(found.pdf, found.pdfPages)) {
            found.pdf.clear();  // its pages can't be shown here: Open File opens it
            found.pdfPages.clear();
        }
        return State::Ready;
    }

    if (head.size() == 1 && head[0] == "-") {
        const auto age = fs::file_time_type::clock::now() - fs::last_write_time(note, ec);
        if (!ec && age < kRetryNoManual) return State::None;
    } else if (pdfNote) {
        found.from = head[1];
        found.link = head[2];
        if (getPdf(j, head[3], local, found, offline)) return State::Ready;
        if (offline) return State::Offline;
        found = Found{};
    } else if (pagesNote) {
        found.from = head[1];
        found.link = head[2];
        found.urls = lines;
        return State::Ready;
    }
    if (!current(j.gen)) return State::None;

    // Digitpress: small PDFs, quick to download (most NES games, a few others).
    std::vector<std::string> names{game.title, cleanTitle(game.fileName)};
    if (known) names.push_back(info.title);
    const std::string digitpress = findDigitpressManual(infoDir, game.system, names);
    if (!digitpress.empty()) {
        found.from = "Digitpress";
        found.link = digitpress;
        if (getPdf(j, digitpress, local, found, offline)) {
            writeFile(note, "pdf\tDigitpress\t" + digitpress + "\t" + digitpress + "\n");
            return State::Ready;
        }
        if (offline) return State::Offline;
        found = Found{};
    }
    if (!current(j.gen)) return State::None;

    // The Internet Archive: the scans whose name has every word of the game's name and says it's a manual.
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
    Json results;
    if (status != 200 || !JsonParser(std::string(body.begin(), body.end())).parse(results)) return State::Offline;
    std::string id;
    int best = 19;
    for (const Json& d : results["response"]["docs"].items) {
        const std::string ident = d["identifier"].s;
        const std::string t = d["title"].s;
        std::string lang = d["language"].s;
        for (const Json& l : d["language"].items) lang += l.s + " ";
        if (ident.empty() || t.empty()) continue;
        const int sc = manualScore(t, lang, words, game.system);
        if (sc > best) {
            best = sc;
            id = ident;
        }
    }
    if (id.empty()) {
        writeFile(note, "-\n");
        return State::None;
    }
    found.from = "the Internet Archive";
    found.link = "https://archive.org/details/" + id;
    if (!current(j.gen)) return State::None;

    // The scan as it was put there: a PDF that isn't too big is downloaded whole (then every page is there at once).
    body.clear();
    const int metaStatus = httpGet("https://archive.org/metadata/" + id, body);
    if (metaStatus == 0) return State::Offline;
    Json meta;
    if (metaStatus != 200 || !JsonParser(std::string(body.begin(), body.end())).parse(meta)) return State::Offline;
    std::string pdfName, zipName;
    for (const Json& f : meta["files"].items) {
        const std::string n = f["name"].s;
        const uint64_t size = std::strtoull(f["size"].s.c_str(), nullptr, 10);
        std::string lower = n;
        for (char& c : lower) c = char(std::tolower(static_cast<unsigned char>(c)));
        auto endsWith = [&](const char* e) {
            const size_t len = std::strlen(e);
            return lower.size() > len && lower.compare(lower.size() - len, len, e) == 0;
        };
        // The pages' pictures (JPEG 2000, made into JPEG by the Internet Archive when asked).
        if (endsWith("_jp2.zip") && (zipName.empty() || n < zipName)) zipName = n;
        if (f["source"].s == "original" && endsWith(".pdf") && size > 0 && size <= kMaxPdf &&
            (pdfName.empty() || n < pdfName))
            pdfName = n;
    }
    if (!pdfName.empty()) {
        const std::string url = "https://archive.org/download/" + id + "/" + urlEncode(pdfName);
        if (getPdf(j, url, local, found, offline)) {
            writeFile(note, "pdf\t" + found.from + "\t" + found.link + "\t" + url + "\n");
            return State::Ready;
        }
        if (offline) return State::Offline;
    }
    if (!current(j.gen)) return State::None;

    // Else its pages one by one. A scan with several copies of the booklet shows the first.
    // Each page has two addresses: the scan's own server (quicker), then archive.org (if that server moved).
    if (!zipName.empty()) {
        body.clear();
        const std::string zipUrl = "https://archive.org/download/" + id + "/" + urlEncode(zipName) + "/";
        const int st = httpGet(zipUrl, body);
        if (st == 0) return State::Offline;
        const std::string listing(body.begin(), body.end());
        std::set<std::string> files;  // "<book>_jp2%2F<book>_0003.jp2", in page order
        for (size_t at = listing.find("href=\""); st == 200 && at != std::string::npos; at = listing.find("href=\"", at + 6)) {
            const size_t start = at + 6, end = listing.find('"', start);
            if (end == std::string::npos) break;
            const std::string href = listing.substr(start, end - start);
            if (href.size() < 5 || href.compare(href.size() - 4, 4, ".jp2") != 0) continue;
            files.insert(href.substr(href.rfind('/') + 1));
        }
        const std::string server = meta["server"].s, dir = meta["dir"].s;
        for (const std::string& f : files) {
            std::string line;
            if (!server.empty() && !dir.empty() && server.find_first_of("/?#&") == std::string::npos)
                line = "https://" + server + "/BookReader/BookReaderImages.php?zip=" + urlEncode(dir + "/" + zipName) +
                       "&file=" + f + "&id=" + id + "&scale=2&rotate=0\t";
            found.urls.push_back(line + zipUrl + f + "&ext=jpg");
        }
    }
    // Found, but with no pages that can be shown here: it opens in the browser.
    std::string text = "pages\t" + found.from + "\t" + found.link + "\n";
    for (const std::string& p : found.urls) text += p + "\n";
    if (!found.urls.empty()) writeFile(note, text);
    return State::Ready;
}

void InfoPanel::Shared::doPage(const Job& j) {
    Result r;
    r.kind = Result::Page;
    r.gen = j.gen;
    r.page = j.page;
    r.state = State::None;
    std::vector<uint8_t> bytes;
    if (j.pageUrl.empty()) {
        // A page of the PDF.
        std::lock_guard<std::mutex> lock(mutex);
        if (pdfGen != j.gen || j.page < 0 || j.page >= int(pdfPages.size())) return;
        const PdfPicture& p = pdfPages[size_t(j.page)];
        if (p.size > 0 && p.offset + p.size <= pdf.size())
            bytes.assign(pdf.begin() + std::ptrdiff_t(p.offset), pdf.begin() + std::ptrdiff_t(p.offset + p.size));
    } else if (!readFile(j.pageFile, bytes) || !isJpeg(bytes)) {
        // From the Internet Archive: "address TAB another address" (tried if the first doesn't work).
        bytes.clear();
        for (const std::string& url : splitTabs(j.pageUrl)) {
            if (url.empty()) continue;
            bytes.clear();
            const int status = httpGet(url, bytes);
            if (status == 0) r.state = State::Offline;
            if (status == 200 && isJpeg(bytes)) {
                r.state = State::None;
                writeNewFile(j.pageFile, bytes.data(), bytes.size());
                break;
            }
            bytes.clear();
            if (!current(j.gen)) return;
        }
    }
    if (!current(j.gen)) return;
    if (bytes.empty() || !decodePicture(bytes, kPictureMaxW, kPictureMaxH, r.rgba, r.w, r.h)) r.rgba.clear();
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
    for (int i = 0; i < kPageThreads; ++i) std::thread([s = shared_] { s->run(true); }).detach();
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

void InfoPanel::open(const LibraryGame& game, const fs::path& coverFolder) {
    if (!shared_) return;
    freePictures();
    game_ = game;
    coverFolder_ = coverFolder;
    open_ = true;
    lastTick_ = SDL_GetTicks();
    tab_ = Back;
    scroll_ = maxScroll_ = 0;
    infoState_ = backState_ = manualState_ = State::Loading;
    info_ = GameInfo{};
    manualFrom_.clear();
    manualLink_.clear();
    manualFile_.clear();
    manualPages_ = 0;
    manualUrls_.clear();
    manualProgress_ = -1;
    page_ = 0;
    big_ = false;
    coverNote_.clear();
    wrapKey_.clear();
    std::lock_guard<std::mutex> lock(shared_->mutex);
    generation_ = ++shared_->generation;
    shared_->jobs.clear();
    shared_->results.clear();
    shared_->pdf.clear();
    shared_->pdfPages.clear();
    Shared::Job j;
    j.gen = generation_;
    j.game = game;
    j.coverFolder = coverFolder;
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
    shared_->pdf.clear();
    shared_->pdfPages.clear();
}

void InfoPanel::requestPage(int page) {
    if (!shared_ || page < 0 || page >= manualPages_ || pages_.count(page)) return;
    pages_[page] = Page{};
    Shared::Job j;
    j.gen = generation_;
    j.kind = Shared::Job::PageJob;
    j.page = page;
    if (page < int(manualUrls_.size())) {
        j.pageUrl = manualUrls_[size_t(page)];
        j.pageFile = shared_->cacheDir / "manuals" / systemInfo(game_.system).key / safeName(game_.fileName) /
                     (std::to_string(page + 1) + ".jpg");
    }
    std::lock_guard<std::mutex> lock(shared_->mutex);
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
            manualFrom_ = res.from;
            manualLink_ = res.link;
            manualFile_ = res.file;
            manualPages_ = res.pages;
            manualUrls_ = std::move(res.urls);
            manualProgress_ = -1;
            break;
        case Shared::Result::Progress: manualProgress_ = res.page; break;
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

// The buttons under the title: the player picks a file, and it's copied into the console's Box Covers and
// Manuals folder, named like the game (replacing the one that was there: the player chose it).
//   0 Add Custom Cover       a picture: the box cover
//   1 Add Custom Back Cover  a picture: the back of the box ("Back" folder)
//   2 Import Game Manual     a PDF: the manual ("Manuals" folder)
void InfoPanel::importFile(SDL_Window* window, int which) {
    bool unavailable = false;
    const std::string picked =
        which == 2 ? openFileDialog(window, "Choose the manual of " + game_.title, "PDF files", {"pdf"}, unavailable)
                   : openFileDialog(window,
                                    (which == 0 ? "Choose a box cover for " : "Choose a back cover for ") + game_.title,
                                    "Pictures", {"png", "jpg", "jpeg", "bmp", "gif"}, unavailable);
    lastTick_ = SDL_GetTicks();  // the window was waiting: don't jump the slide animation
    auto note = [&](const std::string& text, bool bad) {
        coverNote_ = text;
        coverNoteBad_ = bad;
        coverNoteUntil_ = SDL_GetTicks() + 6000;
    };
    if (unavailable) {
        note("No file window on this computer: put the file in the Box Covers and Manuals folder yourself.", true);
        return;
    }
    if (picked.empty()) return;

    std::string error;
    if (which == 0) {
        error = addCustomCover(coverFolder_, game_.fileName, fs::u8path(picked));
        if (error.empty()) coverAdded_ = true;
    } else if (which == 1) {
        error = addCustomCover(coverFolder_ / "Back", game_.fileName, fs::u8path(picked));
    } else {
        error = importManual(fs::u8path(picked));
    }
    if (!error.empty()) {
        note(error, true);
        return;
    }
    // Shows the new back cover / manual: the panel loads the game again, on the same page.
    if (which != 0) {
        const int tab = which == 1 ? Back : Manual;
        open(game_, coverFolder_);
        tab_ = tab;
    }
    note(which == 0 ? "Cover added." : which == 1 ? "Back cover added." : "Manual imported.", false);
}

std::string InfoPanel::importManual(const fs::path& picked) {
    std::error_code ec;
    std::vector<uint8_t> bytes;
    if (!readFile(picked, bytes)) return "Couldn't open that file.";
    if (bytes.size() < 5 || std::memcmp(bytes.data(), "%PDF", 4) != 0) return "That isn't a PDF file.";
    const fs::path dest = coverFolder_ / "Manuals" / fs::u8path(game_.fileName + ".pdf");
    if (fs::equivalent(dest, picked, ec)) return "That's already this game's manual.";
    fs::create_directories(dest.parent_path(), ec);
    const fs::path tmp = dest.u8string() + ".part";
    {
        std::ofstream f(tmp, std::ios::binary);
        f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
        if (!f) {
            f.close();
            fs::remove(tmp, ec);
            return "Couldn't save the manual in the Manuals folder.";
        }
    }
    fs::remove(dest, ec);  // the player chose it: the manual that was there is replaced
    fs::rename(tmp, dest, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return "Couldn't save the manual in the Manuals folder.";
    }
    // It's the player's own manual now, not one Nintendall downloaded.
    if (shared_)
        fs::remove(shared_->cacheDir / "manuals" / systemInfo(game_.system).key / (safeName(game_.fileName) + ".txt"), ec);
    return "";
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
    const int w = std::max(1, std::min(area.w - int(40 * s), std::clamp(int(area.w * 0.42f), int(380 * s), int(600 * s))));
    return {area.x + area.w - w, area.y, w, area.h};
}

InfoPanel::Layout InfoPanel::layout(Text& t, const SDL_Rect& area) {
    Layout L;
    const float s = L.s = t.scale();
    const SDL_Rect full = rect(t, area);
    const float e = 1.0f - std::pow(1.0f - std::clamp(anim_, 0.0f, 1.0f), 3.0f);  // ease out
    L.panel = full;
    L.panel.x = full.x + int(std::lround((1.0f - e) * full.w));  // slides in from the right
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
    if (!coverNote_.empty() && !SDL_TICKS_PASSED(SDL_GetTicks(), coverNoteUntil_)) L.subtitle = coverNote_;
    L.subtitleY = y + int(4 * s);
    if (!L.subtitle.empty()) y = L.subtitleY + t.lineHeight();
    y += int(12 * s);

    // Add Custom Cover, Add Custom Back Cover, Import Game Manual (on two lines if they don't fit on one)
    const int bh = t.lineHeight() + int(14 * s), gap = int(8 * s);
    int bx = x;
    for (const char* name : kButtonNames) {
        const int bw = t.width(name) + int(24 * s);
        if (bx > x && bx + bw > x + w) {
            bx = x;
            y += bh + gap;
        }
        L.buttons.push_back({bx, y, bw, bh});
        bx += bw + gap;
    }
    y += bh + int(14 * s);

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
    const int by = L.panel.y + L.panel.h - pad - bh;
    L.prev = {x, by, bh, bh};
    L.next = {x + bh + int(110 * s), by, bh, bh};
    const int ow = t.width(manualLink_.empty() ? "Open File" : "Open in Browser") + int(28 * s);
    L.browser = {x + w - ow, by, ow, bh};
    const int bottom = tab_ == Manual ? by - int(12 * s) : L.panel.y + L.panel.h - pad;
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
    const int edge = std::max(2, int(3 * s));
    v.fillRect(L.panel.x - edge, L.panel.y, edge, L.panel.h, kRed);

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
    // Release date and players, or what happened when a button below was last used.
    const bool noted = !coverNote_.empty() && L.subtitle == coverNote_;
    if (!L.subtitle.empty()) {
        std::string line = L.subtitle;
        while (line.size() > 3 && t.width(line) > L.content.w) {  // cut to fit, with "..."
            line.resize(line.size() - 4);
            while (!line.empty() && (static_cast<unsigned char>(line.back()) & 0xC0) == 0x80) line.pop_back();
            line += "...";
        }
        t.draw(L.content.x, L.subtitleY, line, noted ? (coverNoteBad_ ? kBad : kGood) : kDim);
    }

    // The buttons
    for (size_t i = 0; i < L.buttons.size(); ++i) {
        const SDL_Rect& b = L.buttons[i];
        v.fillRect(b.x, b.y, b.w, b.h, inside(b, mx, my) ? kButtonHover : kButton);
        t.draw(b.x + (b.w - t.width(kButtonNames[i])) / 2, b.y + (b.h - t.lineHeight()) / 2, kButtonNames[i], kText);
    }
    if (!coverNote_.empty() && SDL_TICKS_PASSED(now, coverNoteUntil_)) coverNote_.clear();

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

    // The pages near the one shown are loaded ahead (also while the back of the box is shown), so
    // turning the page is quick; pages far from it are let go.
    const bool have = manualState_ == State::Ready && manualPages_ > 0;
    if (have) {
        for (const int p : {page_, page_ + 1, page_ + 2, page_ - 1, page_ + 3}) requestPage(p);
        for (auto p = pages_.begin(); p != pages_.end();) {
            if (std::abs(p->first - page_) > 3 && p->second.tex) {
                SDL_DestroyTexture(p->second.tex);
                p = pages_.erase(p);
            } else {
                ++p;
            }
        }
    }

    maxScroll_ = 0;
    if (tab_ == Back) {
        pageRect_ = {};
        if (back_) picture(back_, backW_, backH_, false);
        else if (backState_ == State::Loading) message({"Loading..."});
        else if (backState_ == State::Offline) message({"Couldn't download the back of the box.", "Check your internet connection."});
        else
            message({"No picture of the back of the box found.",
                     "Click Add Custom Back Cover to use your own picture."});
    } else {
        if (manualState_ == State::Loading) {
            message({manualProgress_ >= 0 ? "Downloading the manual... " + std::to_string(manualProgress_) + "%"
                                          : std::string("Looking for the manual...")});
        } else if (manualState_ == State::Offline) {
            message({"Couldn't download the manual.", "Check your internet connection."});
        } else if (manualState_ == State::None) {
            message({"No manual found for this game."});
        } else if (!have) {
            message({"The manual's pages can't be shown here.",
                     manualLink_.empty() ? "Click Open File to read it." : "Click Open in Browser to read it."});
        } else {
            const auto it = pages_.find(page_);
            pageRect_ = {};
            if (it != pages_.end() && it->second.tex) picture(it->second.tex, it->second.w, it->second.h, true);
            else if (it != pages_.end() && it->second.failed) message({"This page couldn't be loaded."});
            else message({"Loading page " + std::to_string(page_ + 1) + "..."});
        }
        // < Page 3 of 24 >   Open in Browser
        if (have) {
            for (const bool nextBtn : {false, true}) {
                const SDL_Rect& b = nextBtn ? L.next : L.prev;
                const bool can = nextBtn ? page_ + 1 < manualPages_ : page_ > 0;
                v.fillRect(b.x, b.y, b.w, b.h, can && inside(b, mx, my) ? kButtonHover : kButton);
                const int len = std::max(4, b.h / 5), th = std::max(2, int(2 * s));
                const int cx = b.x + b.w / 2, cy = b.y + b.h / 2;
                for (int i = 0; i <= len; ++i) {  // a < or > made of two lines
                    const int dx = nextBtn ? len / 2 - i : i - len / 2;
                    v.fillRect(cx + dx - th / 2, cy - i - th / 2, th, th, can ? kText : kDim);
                    v.fillRect(cx + dx - th / 2, cy + i - th / 2, th, th, can ? kText : kDim);
                }
            }
            const std::string label = std::to_string(page_ + 1) + " / " + std::to_string(manualPages_);
            const int gap = L.next.x - (L.prev.x + L.prev.w);
            t.draw(L.prev.x + L.prev.w + (gap - t.width(label)) / 2, L.prev.y + (L.prev.h - t.lineHeight()) / 2, label, kText);
        }
        if (manualState_ == State::Ready && (!manualLink_.empty() || !manualFile_.empty())) {
            const SDL_Rect& b = L.browser;
            const char* label = manualLink_.empty() ? "Open File" : "Open in Browser";
            v.fillRect(b.x, b.y, b.w, b.h, inside(b, mx, my) ? kButtonHover : kButton);
            t.draw(b.x + (b.w - t.width(label)) / 2, b.y + (b.h - t.lineHeight()) / 2, label, kText);
        }
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
        const bool can = manual && (nextBtn ? page_ + 1 < manualPages_ : page_ > 0);
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
    const std::string label = manual ? "Page " + std::to_string(page_ + 1) + " of " + std::to_string(manualPages_) +
                                           "   \xC2\xB7   " + back
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
        else if (manual && e.button.button == SDL_BUTTON_LEFT && mx >= area.x + area.w - edge && page_ + 1 < manualPages_)
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
        case SDLK_END: setPage(manualPages_ - 1); break;
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
    tab_ = (tab % TabCount + TabCount) % TabCount;
    big_ = false;
    scroll_ = 0;
}

void InfoPanel::setPage(int page) {
    if (manualPages_ <= 0) return;
    const int p = std::clamp(page, 0, manualPages_ - 1);
    if (p != page_) scroll_ = 0;
    page_ = p;
}

void InfoPanel::scrollBy(int dy) { scroll_ = std::clamp(scroll_ + dy, 0, maxScroll_); }

bool InfoPanel::handleEvent(const SDL_Event& e, int mx, int my, Text& t, const SDL_Rect& area) {
    if (!open_) return false;
    if (big_) return bigEvent(e, mx, my, area);
    const Layout L = layout(t, area);
    const int lineStep = int(60 * L.s), pageStep = std::max(lineStep, L.content.h - lineStep);
    const bool manualShown = tab_ == Manual && manualState_ == State::Ready && manualPages_ > 0;

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
        for (size_t i = 0; i < L.buttons.size(); ++i) {
            if (inside(L.buttons[i], mx, my)) {
                importFile(SDL_GetWindowFromID(e.button.windowID), int(i));
                return true;
            }
        }
        for (size_t i = 0; i < L.tabs.size(); ++i)
            if (inside(L.tabs[i], mx, my)) setTab(int(i));
        if (manualShown && inside(L.prev, mx, my)) setPage(page_ - 1);
        if (manualShown && inside(L.next, mx, my)) setPage(page_ + 1);
        if (tab_ == Manual && manualState_ == State::Ready && inside(L.browser, mx, my)) {
            if (!manualLink_.empty()) SDL_OpenURL(manualLink_.c_str());
            else if (!manualFile_.empty()) openFolder(manualFile_);  // opens the PDF in the computer's PDF reader
        }
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
        case SDLK_c: importFile(SDL_GetWindowFromID(e.key.windowID), 0); return true;
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
