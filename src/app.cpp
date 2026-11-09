#include "app.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <set>

#include "filedialog.hpp"
#include "http.hpp"
#include "library.hpp"
#include "selfupdate.hpp"
#include "sha256.hpp"
#include "update.hpp"
#include "wiimotepair.hpp"

#include <atomic>
#include <thread>

namespace {
// LAN games: GameCube / Wii use Dolphin's netplay; NES / SNES / N64 use Nintendall's own.
bool usesDolphinLan(System s) { return s == System::GameCube || s == System::Wii; }
bool usesNetplay(System s) { return s == System::NES || s == System::FDS || s == System::SNES || s == System::N64; }
// The console folder a game is in (FDS games are in the NES folder).
System lanFamily(System s) { return s == System::FDS ? System::NES : s; }
// Link-cable LAN games: everyone plays their own game, on an emulator with a network link cable
// (Gambatte for Game Boy / Color, gpSP for GBA), instead of the usual one.
bool usesLinkLan(System s) { return s == System::GB || s == System::GBC || s == System::GBA; }
System linkFamily(System s) { return s == System::GBC ? System::GB : s; }
const char* linkCore(System s) { return s == System::GBA ? "gpsp" : "gambatte"; }
const char* linkFamilyName(System s) { return s == System::GBA ? "Game Boy Advance" : "Game Boy or Game Boy Color"; }
constexpr const char* kGbLinkPort = "56400";  // Gambatte's own link connection (TCP)
}  // namespace

namespace fs = std::filesystem;

// Update Now: the setup being downloaded (filled in by a thread).
struct App::DownloadJob {
    std::atomic<uint64_t> got{0}, total{0};
    std::atomic<bool> done{false}, cancel{false};
    bool ok = false;
    std::string error;
    UpdateInfo info;
    fs::path file;
};

// Set by Ctrl+C / kill in headless test runs (no window, so no SDL quit event).
volatile std::sig_atomic_t g_stopRequested = 0;

namespace {

const Color kBlack{0, 0, 0};
const Color kWhite{255, 255, 255};

constexpr int kDefaultBaseW = 256, kDefaultBaseH = 240;  // NES picture, used before any game is loaded
constexpr double kDefaultAspect = 256.0 * 8.0 / 7.0 / 240.0;

double nowSeconds() { return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency()); }

// Systems that have no analog stick, so the left stick should drive the D-pad.
bool usesStickAsDpad(System s) {
    switch (s) {
    case System::N64:
    case System::GameCube:
    case System::Wii:
    case System::WiiU:
    case System::N3DS:
        return false;
    default:
        return true;
    }
}

bool ctrlHeld(const SDL_Keysym& k) { return (k.mod & (KMOD_CTRL | KMOD_GUI)) != 0; }

// Two-screen layouts. DS values are melonDS DS's "Screen Layout" option; 3DS values are
// Azahar's "Screen Layout" option; Wii U values are Cemu's (TV + GamePad screens).
// nullptr = that emulator can't do the layout.
struct DsLayout {
    const char* label;
    const char* value;    // DS, normal
    const char* swapped;  // DS, with Swap Screens on
    const char* ctr;      // 3DS (swap is a separate option there)
    const char* wiiu;     // Wii U (swap is a separate option, except for Single Screen)
};
const DsLayout kDsLayouts[] = {
    {"Vertical", "top-bottom", "bottom-top", "default", "Top Bottom"},
    {"Side by Side", "left-right", "right-left", "side_by_side", "Side by Side"},
    {"Large + Small Screen", "largescreen-top", "largescreen-bottom", "large_screen", "Picture in Picture"},
    {"Single Screen", "top", "bottom", "single_screen", "Default Screen"},
    {"Sideways (Rotate Left)", "rotate-left", "rotate-left", nullptr, nullptr},
    {"Sideways (Rotate Right)", "rotate-right", "rotate-right", nullptr, nullptr},
    // The emulator draws Vertical; Nintendall shows each half in its own window (swap = which goes where).
    {"Separate Windows", "top-bottom", "top-bottom", "default", "Top Bottom"},
};
constexpr int kSingleScreen = 3;
constexpr int kSeparate = 6;
// Order in the Layout menu (-1 = separator). Indexes are saved per game, so new layouts go at the end.
constexpr int kLayoutMenuOrder[] = {0, 1, 2, 3, kSeparate, -1, 4, 5};

// Systems drawn on the GPU: OpenGL, Vulkan (GameCube / Wii, 3DS, Wii U, if chosen in Console
// Settings), or the core's (slow) software renderer as a last resort.
enum class Renderer { OpenGL, Vulkan, Software };
bool hasSoftwareFallback(System s) { return s == System::N3DS || s == System::N64; }
void chooseRenderer(LibretroCore& core, System s, Renderer r) {
    const bool vulkan = r == Renderer::Vulkan, software = r == Renderer::Software;
    core.setPreferVulkan(vulkan);  // Dolphin follows what we prefer
    if (s == System::N3DS) core.forceOptionValue("citra_graphics_api", software ? "Software" : vulkan ? "Vulkan" : "OpenGL");
    if (s == System::WiiU) {
        core.forceOptionValue("cemu_gpu_api", vulkan ? "Vulkan" : "OpenGL");
        // Always the Wii U's own 720p: resizing breaks shadows and effects in games such as
        // Twilight Princess HD.
        core.forceOption("cemu_internal_resolution", "native");
    }
    if (s == System::N64) {
        core.forceOption("mupen64plus-rdp-plugin", software ? "angrylion" : "gliden64");
        core.forceOption("mupen64plus-rsp-plugin", software ? "cxd4" : "hle");
    }
}

// Dolphin only sends game sound to a real Wii Remote's speaker with [Core] WiimoteEnableSpeaker
// on (off by default). The libretro Dolphin has no option for it, but reads its own Dolphin.ini
// (in <saves>/User/Config), so switch it on there. Other lines are kept as they are.
void enableWiimoteSpeaker(const fs::path& ini) {
    std::vector<std::string> lines;
    {
        std::ifstream in(ini);
        for (std::string l; std::getline(in, l);) {
            if (!l.empty() && l.back() == '\r') l.pop_back();
            lines.push_back(l);
        }
    }
    auto trim = [](std::string t) {
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
        return t;
    };
    const std::string setting = "WiimoteEnableSpeaker = True";
    bool inCore = false, done = false;
    size_t coreEnd = std::string::npos;  // where to add the line if [Core] doesn't have it
    for (size_t i = 0; i < lines.size() && !done; ++i) {
        const std::string t = trim(lines[i]);
        if (!t.empty() && t.front() == '[') {
            if (inCore) coreEnd = i;
            inCore = t == "[Core]";
            if (inCore) coreEnd = i + 1;
            continue;
        }
        if (inCore) {
            coreEnd = i + 1;
            if (trim(t.substr(0, t.find('='))) == "WiimoteEnableSpeaker") {
                if (t == setting) return;  // already on
                lines[i] = setting;
                done = true;
            }
        }
    }
    if (!done) {
        if (coreEnd == std::string::npos) {
            lines.push_back("[Core]");
            lines.push_back(setting);
        } else {
            lines.insert(lines.begin() + std::ptrdiff_t(coreEnd), setting);
        }
    }
    std::error_code ec;
    fs::create_directories(ini.parent_path(), ec);
    std::ofstream out(ini, std::ios::trunc);
    for (const auto& l : lines) out << l << '\n';
}

bool hasTwoScreens(System s) { return s == System::NDS || s == System::N3DS || s == System::WiiU; }
bool layoutAvailable(System s, int i) {
    return s == System::NDS || (s == System::N3DS && kDsLayouts[i].ctr) || (s == System::WiiU && kDsLayouts[i].wiiu);
}
constexpr int kDsLayoutCount = int(sizeof kDsLayouts / sizeof kDsLayouts[0]);
constexpr int kFirstSideways = 4;

// Config keys can't contain '=' or '#'.
std::string configSafe(std::string s) {
    for (char& c : s)
        if (c == '=' || c == '#') c = '_';
    return s;
}

}  // namespace

// ============================================================================
// Main loop

int App::run(const fs::path& start, const LaunchOptions& opt) {
    opt_ = opt;
    updateInputProfile();  // identity button mapping until a game says otherwise
    paths_.init();
    cfg_.load(paths_.data / "nintendall.cfg");

    // Default window: the largest of 3x / 2x NES size that fits on the screen.
    float dpi = 1.0f;
#if defined(_WIN32)
    float hdpi = 96.0f;
    if (!opt.headless && SDL_GetDisplayDPI(0, nullptr, &hdpi, nullptr) == 0 && hdpi > 0) dpi = hdpi / 96.0f;
#endif
    int winScale = 3;
    SDL_Rect usable{0, 0, 1280, 720};
    if (!opt.headless) SDL_GetDisplayUsableBounds(0, &usable);
    while (winScale > 1 && (kDefaultBaseH * winScale + 60) * dpi > usable.h) --winScale;
    const int winH = int((kDefaultBaseH * winScale + 24) * dpi);
    const int winW = int(kDefaultBaseH * winScale * kDefaultAspect * dpi);

    std::string err;
    if (!video_.init(opt.headless, opt.fullscreen, winW, winH, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!opt.headless) {
        const std::vector<fs::path> fonts = {
#if defined(_WIN32)
            fs::u8path(SDL_getenv("WINDIR") ? SDL_getenv("WINDIR") : "C:\\Windows") / "Fonts" / "segoeui.ttf",
#endif
            paths_.base / "fonts" / "DejaVuSans.ttf",
            paths_.base / ".." / "assets" / "fonts" / "DejaVuSans.ttf",
        };
        if (!text_.init(video_.renderer(), fonts, err)) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Nintendall", err.c_str(), video_.window());
            return 1;
        }
        text_.setScale(uiScale());
        audio_.init();
        fs::path db = paths_.base / "gamecontrollerdb.txt";
        if (!fs::exists(db)) db = paths_.data / "gamecontrollerdb.txt";
        input_.init(fs::exists(db) ? db.u8string() : "");
        input_.load(cfg_);
        // Home screen: Documents/Nintendall/Games and, next to it, Box Covers (or the folders set as
        // games.folder / covers.folder in nintendall.cfg).
        const std::string games = cfg_.get("games.folder"), covers = cfg_.get("covers.folder");
        const fs::path gamesRoot = games.empty() ? defaultGamesFolder(paths_.base, paths_.portable) : fs::u8path(games);
        const fs::path coversRoot = covers.empty() ? gamesRoot.parent_path() / "Box Covers" : fs::u8path(covers);
        home_.start(gamesRoot, coversRoot, paths_.data / "covers", cfg_.get("home.console"));
        home_.setCoverSize(std::atoi(cfg_.get("home.cover_size").c_str()));
        settings_.setWiiUPackFolders(paths_.systemDirFor("cemu") / "Cemu" / "graphicPacks",
                                     paths_.systemDirFor("cemu") / "Cemu" / "customGraphicPacks");
        // After Update Now: say whether the new version is running, and tidy the temp folder.
        removeOldUpdateFiles();
        const std::string updated = cfg_.get("update.installing");
        if (!updated.empty()) {
            cfg_.set("update.installing", "");
            if (updated == NINTENDALL_VERSION) osd("Nintendall was updated to " + updated, 5000);
            else if (isNewerVersion(updated, NINTENDALL_VERSION))
                osd("The update to " + updated + " wasn't installed - you can try again next time", 6000);
        }
        startUpdateCheck();
    }
    input_.setForcedButtons(opt.holdButtons);

    // Headless: load the game, run N frames as fast as possible, report, exit.
    if (opt.headless) {
        std::signal(SIGINT, [](int) { g_stopRequested = 1; });
        std::signal(SIGTERM, [](int) { g_stopRequested = 1; });
        if (!startGame(start)) return 1;
        const double t0 = nowSeconds();
        for (int i = 0; i < opt.frames && core_ && !g_stopRequested; ++i) core_->run();
        const double dt = nowSeconds() - t0;
        std::printf("Ran %d frames in %.2fs (%.0f fps, %.1fx real time)\n", opt.frames, dt, opt.frames / dt,
                    opt.frames / dt / core_->avInfo().timing.fps);
        std::printf("Audio: %llu frames received, peak level %d\n", (unsigned long long)audioFramesSeen_, audioPeak_);
        if (!opt.screenshot.empty())
            std::printf("Screenshot %s: %s\n", video_.saveScreenshot(opt.screenshot) ? "saved" : "FAILED",
                        opt.screenshot.c_str());
        stopGame();
        return 0;
    }

    std::error_code ec;
    draw();  // show the window before any message box can pop up
    if (!start.empty() && !fs::is_directory(start, ec)) startGame(start);
    else if (!start.empty() && detectGame(start).system != System::Unknown) startGame(start);  // Wii U folder

    nextFrame_ = nowSeconds();
    while (!quit_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        if (buttonConfig_.takeChanged()) {
            saveConfig();
            connectPorts();
        }
        checkWiimotePairing();
        updateLan();
        checkUpdateResult();
        checkUpdateDownload();
        if (settings_.takeRestartRequest() && core_) {
            const fs::path game = gamePath_;  // reload the game so the new settings apply
            startGame(game);
            osd(realWiimotes() ? "Game restarted - press a button on each Wii Remote to connect it"
                               : "Game restarted with the new settings",
                realWiimotes() ? 6000 : 2500);
        } else if (settings_.takeClearCacheRequest() && core_) {
            // The running game has the cache open: close it, clear, start it again.
            const fs::path game = gamePath_;
            stopGame();
            clearWiiUShaderCache();
            startGame(game);
            osd("Shader cache cleared - the game rebuilds its shaders as you play", 6000);
        } else if (settingsWasOpen_ && !settings_.isOpen() && settings_.restartPending()) {
            osd("Some settings apply the next time the game starts", 4000);
        }
        settingsWasOpen_ = settings_.isOpen();

        const bool modal = menu_.isOpen() || buttonConfig_.isOpen() || settings_.isOpen() || lanDialog_.isOpen();
        // A LAN game never stops: the other players' games wait for this one.
        const bool running = core_ && (lan_.active || (!paused_ && !focusLost_ && !modal));
        input_.setKeyboardEnabled(!modal);
        if (wasRunning_ && !running) audio_.clear();  // silence while paused / in menus
        if (running && !wasRunning_) nextFrame_ = nowSeconds();
        wasRunning_ = running;

        if (running) runFrames();
        text_.setScale(uiScale());
        draw();

        // Hide the mouse pointer while playing fullscreen.
        const bool hideCursor = running && video_.isFullscreen() && SDL_GetTicks() - lastMouseMove_ > 2000;
        SDL_ShowCursor(hideCursor ? SDL_DISABLE : SDL_ENABLE);

        if (running) pace(fastForward_ ? 1.0 / 120.0 : 1.0 / core_->avInfo().timing.fps);
        else pace(1.0 / 60.0);
    }

    stopGame();
    saveConfig();
    home_.shutdown();
    text_.shutdown();
    input_.shutdown();
    audio_.shutdown();
    video_.shutdown();
    return 0;
}

void App::pace(double frameSeconds) {
    // Timer-based pacing. With vsync on, present() usually does most of the waiting.
    const double now = nowSeconds();
    if (now - nextFrame_ > 0.1 || nextFrame_ - now > 0.5) nextFrame_ = now;  // fell behind / clock jump
    nextFrame_ += frameSeconds;
    const double wait = nextFrame_ - now;
    if (wait > 0.002) SDL_Delay(Uint32((wait - 0.0015) * 1000));
    while (nowSeconds() < nextFrame_) {
    }
}

void App::runFrames() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    fastForward_ = !lan_.active && (fastForwardToggled_ || keys[SDL_SCANCODE_TAB]);
    const int runs = fastForward_ ? 4 : 1;
    if (lan_.net && core_ && lan_.net->game().link && lan_.net->phase() == Netplay::Phase::Playing) {
        linkFrame();  // link-cable game: our own game, with packets from the others' emulators
    } else if (lan_.net && core_) {
        if (lan_.net->phase() == Netplay::Phase::Playing) {
            Netplay::Callbacks cb;
            cb.sampleLocal = [this] { return localNetInput(); };
            cb.save = [this] { return core_->saveState(); };
            cb.load = [this](const std::vector<uint8_t>& st) { return core_->loadState(st); };
            cb.run = [this](const NetInputs& in, bool replay) {
                netInputs_ = in;
                netReplay_ = replay;
                core_->run();
                netReplay_ = false;
            };
            lan_.net->advance(cb);
        } else if (lan_.warmup < 2) {
            // A couple of frames after loading (some emulators can only save their state after
            // running), then the game waits behind the lobby.
            ++lan_.warmup;
            netReplay_ = true;
            core_->run();
            netReplay_ = false;
        }
    } else {
        for (int i = 0; i < runs && core_; ++i) core_->run();
    }
    if (core_ && core_->shutdownRequested()) {
        stopGame();
        return;
    }
    // After a layout change, resize the window once the core reports the new picture size.
    if (pendingFitFrames_ > 0) {
        --pendingFitFrames_;
        if (avChanged_) {
            fitWindowToGame();
            pendingFitFrames_ = 0;
        }
    }
    avChanged_ = false;
    saveSram(false);
}

void App::saveConfig() {
    input_.save(cfg_);
    if (!opt_.headless) {
        cfg_.set("home.console", home_.consoleKey());
        cfg_.set("home.cover_size", std::to_string(home_.coverSize()));
    }
    cfg_.save();
}

// ============================================================================
// Starting and stopping games

bool App::startGame(const fs::path& path) {
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        showMessage("File not found", path.u8string(), true);
        return false;
    }

    // 1. Work out which system this game is for.
    const Detection det = detectGame(path);
    if (det.system == System::Unknown) {
        showMessage("Couldn't open this file",
                    path.filename().u8string() + "\n\nThis doesn't look like a game Nintendall can play (" +
                        det.detail + ").",
                    true);
        return false;
    }
    const SystemInfo& sys = systemInfo(det.system);
    std::fprintf(stderr, "Detected %s (%s)\n", sys.fullName, det.detail.c_str());

    if (sys.needsGpu) {
        showMessage("Coming soon", std::string(sys.fullName) +
                                       " games aren't supported yet. Support is coming in a future update.",
                    false);
        return false;
    }
    if (det.system == System::WiiU && needsWiiUKeys(det.launchPath.empty() ? path : det.launchPath)) {
        showMessage("Wii U keys needed",
                    path.filename().u8string() +
                        "\n\nWii U disc images are encrypted with keys from your own console. Add your keys.txt "
                        "with Options > Wii U Keys..., then open the game again.\n\n(A game.key file next to the "
                        "game works too.)",
                    true);
        return false;
    }
    const char* coreName = lanLinkLoad_ && usesLinkLan(det.system) ? linkCore(det.system) : sys.core;
    const fs::path corePath = paths_.findCore(coreName);
    if (corePath.empty()) {
        showMessage("Nintendall is missing a file",
                    std::string("The ") + sys.fullName + " emulator (" + coreFileName(coreName) +
                        ") is missing from the cores folder. Reinstalling Nintendall should fix this.",
                    true);
        return false;
    }

    // 2. Checks passed: close whatever is running.
    stopGame();

    // 3. If the game is inside a zip, pull it out to a temporary file.
    fs::path gameFile = det.launchPath;
    if (!det.zipEntry.empty()) {
        ZipFile z;
        std::vector<uint8_t> data;
        const ZipEntry* entry = nullptr;
        if (z.open(path))
            for (const auto& e : z.entries())
                if (e.name == det.zipEntry) entry = &e;
        if (!entry || !z.read(*entry, data)) {
            showMessage("Couldn't unzip game", path.u8string(), true);
            return false;
        }
        extracted_ = paths_.cache / fs::u8path(det.zipEntry).filename();
        std::ofstream out(extracted_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) {
            showMessage("Couldn't unzip game", "Writing " + extracted_.u8string() + " failed", true);
            return false;
        }
        gameFile = extracted_;
    }

    if (det.system == System::Wii) enableWiimoteSpeaker(paths_.saves / "User" / "Config" / "Dolphin.ini");

    // 4. Start the emulator with the game. GPU systems try the chosen renderer first, then fall
    //    back: Vulkan -> OpenGL -> software (3DS / N64), if this PC can't do the first.
    std::string err;
    std::vector<Renderer> renderers;
    std::string startNote;
    for (size_t attempt = 0; attempt == 0 || attempt < renderers.size(); ++attempt) {
        core_ = std::make_unique<LibretroCore>(*this, paths_.systemDirFor(coreName), paths_.saves,
                                               paths_.config / (std::string(coreName) + ".opt"));
        const bool coreLoaded = core_->load(corePath, err);
        if (coreLoaded && lanPending_ && usesDolphinLan(det.system) && !lanApi(*core_).ok()) {
            core_.reset();
            system_ = System::Unknown;
            showMessage("LAN game", std::string("This copy of Nintendall's ") + sys.fullName +
                                        " emulator can't play LAN games. Reinstalling Nintendall should fix this.",
                        true);
            return false;
        }
        if (coreLoaded && hasTwoScreens(det.system)) {
            system_ = det.system;
            gameTitle_ = path.stem().u8string();
            loadLayout();
            if (det.system == System::NDS)
                core_->forceOption("melonds_show_current_layout", "disabled");  // we have our own menu
            applyLayout();
        }
        if (coreLoaded && attempt == 0) {
            if (canUseVulkan(det.system))
                core_->addAppOption(kGraphicsApiOption, "Graphics API", "vulkan", {"vulkan", "opengl"}, {"Vulkan", "OpenGL"});
            if (canUseVulkan(det.system) && core_->optionValue(kGraphicsApiOption) == "vulkan")
                renderers.push_back(Renderer::Vulkan);
            renderers.push_back(Renderer::OpenGL);
            if (hasSoftwareFallback(det.system)) renderers.push_back(Renderer::Software);
        }
        if (coreLoaded && canUseVulkan(det.system) && attempt > 0)  // keep the user's choice in the file
            core_->addAppOption(kGraphicsApiOption, "Graphics API", "vulkan", {"vulkan", "opengl"}, {"Vulkan", "OpenGL"});
        if (coreLoaded) chooseRenderer(*core_, det.system, renderers[attempt]);
        if (coreLoaded && attempt == 0 && det.system == System::WiiU) {
            // A different Cemu than the one that built the shader cache (an update): start it afresh.
            std::error_code sec;
            const std::string build = core_->version() + "/" + std::to_string(fs::file_size(corePath, sec));
            if (cfg_.get("wiiu.shader_cache_build") != build) {
                clearWiiUShaderCache();
                cfg_.set("wiiu.shader_cache_build", build);
                saveConfig();
            }
        }
        if (coreLoaded && (det.system == System::GameCube || det.system == System::Wii)) {
            core_->forceOption("dolphin_osd_enabled", "disabled");  // no emulator info text over the game
            // 0.13.4's Ultrawide choice set Aspect Ratio to Custom (21:9): put that back to Stretch.
            const std::string aspect = core_->optionValue("dolphin_aspect_ratio");
            if (aspect == "4" || aspect == "5") core_->forceOption("dolphin_aspect_ratio", "3");
            // Dolphin's dual-core mode keeps the game running between our frames, so games ran too fast
            // (or at half speed). Single-core runs exactly one video field per frame, at the right speed.
            core_->forceOption("dolphin_main_cpu_thread", "disabled");
            core_->setRefreshTimingOnGeometry(true);
        }
        if (coreLoaded && det.system == System::Wii) {
            core_->preferDefault("dolphin_wiimote_default_device", "nunchuk");  // what most Wii games expect
            core_->preferDefault("dolphin_ir_offset", "10");  // pointer lines up with the mouse / stick
        }
        if (coreLoaded && !lanCoreOptions_.empty()) core_->setSessionOptions(lanCoreOptions_);  // joining
        if (coreLoaded && lanPending_ && usesDolphinLan(det.system)) {
            // A LAN game: the emulator hosts / joins, and boots the game when the host starts it.
            const LanApi api = lanApi(*core_);
            api.setup(lanPending_->kind == LanDialog::Kind::Host ? 1 : 2, lanPending_->target.c_str(), kLanPort,
                      lanPending_->name.c_str(), unsigned(lanPending_->delay));
        }
        if (coreLoaded && core_->loadGame(gameFile, err)) {
            if (attempt > 0 && renderers[0] == Renderer::Vulkan)
                startNote = renderers[attempt] == Renderer::OpenGL ? "Vulkan couldn't start on this PC - using OpenGL"
                                                                   : "Vulkan couldn't start on this PC";
            break;
        }

        // If this PC couldn't give the emulator its GPU renderer, try the next one.
        const bool retry = coreLoaded && attempt + 1 < renderers.size() && core_->wantsHardwareRendering() &&
                           !core_->usingHardwareRendering();
        std::fprintf(stderr, "Core failed: %s\n", err.c_str());
        core_.reset();
        if (!retry) break;
        std::fprintf(stderr, "Retrying with %s\n", renderers[attempt + 1] == Renderer::OpenGL ? "OpenGL" : "software rendering");
    }
    if (!core_) {
        system_ = System::Unknown;
        if (!extracted_.empty()) fs::remove(extracted_, ec);
        extracted_.clear();
        showMessage("Couldn't start the game",
                    path.filename().u8string() + "\n\nThe file may be damaged, or be in a format that isn't supported." +
                        (sys.note ? std::string("\n\nNote: ") + sys.note : ""),
                    true);
        return false;
    }

    // 5. Running.
    if (lanPending_ && usesDolphinLan(det.system)) {
        lan_ = Lan{};
        lan_.active = true;
        lan_.req = *lanPending_;
        lan_.api = lanApi(*core_);
        if (lan_.req.kind == LanDialog::Kind::Host) lan_.addresses = localAddresses();
    } else if (lanPending_) {
        // Hosting an NES / SNES / N64 game: wait for players (Nintendall's netplay).
        Netplay::GameInfo info;
        info.system = int(det.system);
        info.crc = netplayGameCrc(gameFile.u8string());
        info.title = lanPending_->game.title.empty() ? path.stem().u8string() : lanPending_->game.title;
        for (const auto& [key, o] : core_->options())
            if (!core_->forcedOptions().count(key)) info.options.push_back({key, o.value});
        info.window = det.system == System::N64 ? 0 : 8;  // N64 save states are too big to take every frame
        info.delay = lanPending_->delay;
        if (usesLinkLan(det.system)) {  // everyone plays their own game, with their own settings
            info.link = true;
            info.options.clear();
            info.maxPlayers = det.system == System::GBA ? 4 : 2;  // a Game Boy link cable joins two
        }
        auto net = std::make_unique<Netplay>();
        std::string netError;
        if (!net->host(lanPending_->name, info, netError)) {
            stopGame();
            showMessage("LAN Game", netError, true);
            return false;
        }
        lan_ = Lan{};
        lan_.active = true;
        lan_.req = *lanPending_;
        lan_.net = std::move(net);
        lan_.addresses = localAddresses();
    }
    system_ = det.system;
    gamePath_ = path;
    gameTitle_ = path.stem().u8string();
    const auto& av = core_->avInfo();
    onAvInfoChanged();
    audio_.clear();
    audio_.setInputRate(av.timing.sample_rate);
    input_.setStickAsDpad(usesStickAsDpad(system_));
    updateInputProfile();
    connectPorts();
    if (realWiimotes()) {
        // SDL's own Wii Remote driver would talk to the remotes too and confuse them: let Dolphin have them.
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_WII", "0");
        sdlWiiDriverOff_ = true;
        osd("Real Wii Remotes: press a button on each remote to connect it", 6000);
    }
    video_.clearFrame();
    loadSram();
    paused_ = fastForward_ = fastForwardToggled_ = false;
    playedSinceLaunch_ = true;  // no "new version" popup from now on
    nextFrame_ = nowSeconds();
    if (!startNote.empty()) osd(startNote, 6000);
    addRecent(path);
    if (video_.window()) SDL_SetWindowTitle(video_.window(), ("Nintendall - " + gameTitle_).c_str());
    // If the game's shape is very different from the window (e.g. NES -> tall DS), refit the window.
    updateScreenWindow();
    if (separateWindows()) {
        fitWindowToGame();
    } else {
        const SDL_Rect a = gameArea();
        const auto& g = av.geometry;
        const double gameAspect = g.aspect_ratio > 0 ? g.aspect_ratio : double(g.base_width) / std::max(1u, g.base_height);
        const double winAspect = a.h > 0 ? double(a.w) / a.h : gameAspect;
        if (std::max(gameAspect, winAspect) / std::min(gameAspect, winAspect) > 1.25) fitWindowToGame();
    }
    std::fprintf(stderr, "Running %s with core %s %s (%ux%u @ %.3f fps, %.0f Hz audio)\n", gameTitle_.c_str(),
                 core_->name().c_str(), core_->version().c_str(), av.geometry.base_width, av.geometry.base_height,
                 av.timing.fps, av.timing.sample_rate);
    return true;
}

void App::stopGame() {
    if (!core_) return;
    saveSram(true);
    core_.reset();
    input_.stopAllRumble();
    audio_.clear();
    std::error_code ec;
    if (!extracted_.empty()) fs::remove(extracted_, ec);
    extracted_.clear();
    video_.clearFrame();
    system_ = System::Unknown;
    updateScreenWindow();  // closes the second window
    updateInputProfile();
    if (sdlWiiDriverOff_) {  // Wii Remotes can be regular controllers again
        SDL_SetHint("SDL_JOYSTICK_HIDAPI_WII", "1");
        sdlWiiDriverOff_ = false;
    }
    paused_ = false;
    lan_ = Lan{};  // unloading the emulator ended the LAN game (it told the other players)
    if (video_.window()) SDL_SetWindowTitle(video_.window(), "Nintendall");
}

void App::addRecent(const fs::path& path) {
    std::vector<std::string> list = recentGames();
    const std::string p = fs::absolute(path).u8string();
    list.erase(std::remove(list.begin(), list.end(), p), list.end());
    list.insert(list.begin(), p);
    if (list.size() > size_t(kRecent)) list.resize(size_t(kRecent));
    for (int i = 0; i < kRecent; ++i) cfg_.set("recent." + std::to_string(i + 1), i < int(list.size()) ? list[size_t(i)] : "");
    cfg_.save();
}

std::vector<std::string> App::recentGames() const {
    std::vector<std::string> list;
    for (int i = 0; i < kRecent; ++i) {
        const std::string p = cfg_.get("recent." + std::to_string(i + 1));
        if (!p.empty()) list.push_back(p);
    }
    return list;
}

// ---- battery saves ----------------------------------------------------------

static fs::path sramPath(const Paths& p, const fs::path& game) {
    return p.saves / fs::u8path(game.stem().u8string() + ".srm");
}

void App::loadSram() {
    lastSram_.clear();
    uint8_t* mem = core_ ? core_->saveRamData() : nullptr;
    const size_t size = core_ ? core_->saveRamSize() : 0;
    if (!mem || size == 0) return;
    std::ifstream f(sramPath(paths_, gamePath_), std::ios::binary);
    if (f) f.read(reinterpret_cast<char*>(mem), std::streamsize(size));
    lastSram_.assign(mem, mem + size);
    lastSramCheck_ = SDL_GetTicks();
}

void App::saveSram(bool force) {
    if (!core_) return;
    // Joined an NES / SNES / N64 LAN game: the game runs on the host's save, which isn't ours to keep.
    if (lan_.net && !lan_.net->isHost() && !lan_.net->game().link) return;
    const Uint32 now = SDL_GetTicks();
    if (!force && now - lastSramCheck_ < 3000) return;  // check a few times a minute
    lastSramCheck_ = now;
    uint8_t* mem = core_->saveRamData();
    const size_t size = core_->saveRamSize();
    if (!mem || size == 0) return;
    if (lastSram_.size() == size && std::equal(mem, mem + size, lastSram_.begin())) return;
    lastSram_.assign(mem, mem + size);
    std::ofstream f(sramPath(paths_, gamePath_), std::ios::binary);
    f.write(reinterpret_cast<const char*>(mem), std::streamsize(size));
}

// ---- save states ------------------------------------------------------------

fs::path App::statePath(int slot) const {
    return paths_.states / fs::u8path(gameTitle_ + "." + systemInfo(system_).key + ".state" + std::to_string(slot));
}

void App::saveState() {
    if (!core_) return;
    if (system_ == System::WiiU) {
        osd("Wii U games can't use save states - use the game's own saves");
        return;
    }
    const auto data = core_->saveState();
    if (data.empty()) {
        osd("Save states aren't available for this game");
        return;
    }
    std::ofstream f(statePath(slot_), std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    osd(f ? "State saved (slot " + std::to_string(slot_) + ")" : std::string("Saving the state failed"));
}

void App::loadState() {
    if (!core_) return;
    if (system_ == System::WiiU) {
        osd("Wii U games can't use save states - use the game's own saves");
        return;
    }
    std::ifstream f(statePath(slot_), std::ios::binary);
    if (!f) {
        osd("Slot " + std::to_string(slot_) + " is empty");
        return;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    osd(core_->loadState(data) ? "State loaded (slot " + std::to_string(slot_) + ")" : std::string("Loading the state failed"));
}

void App::takeScreenshot() {
    if (!core_) return;
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    const fs::path p = paths_.screenshots / fs::u8path(gameTitle_ + "-" + stamp + ".png");
    osd(video_.saveScreenshot(p) ? "Screenshot saved" : "Screenshot failed");
}

// ============================================================================
// CoreHost

void App::onVideo(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt) {
    if (netReplay_) return;  // LAN game: a frame played again (only the last one is shown)
    video_.uploadFrame(data, w, h, pitch, fmt);
    if (separateWindows()) {  // main window: top screen (bottom when swapped); second window: the other
        // Wii U: Cemu squeezes both 16:9 screens into one 16:9 picture; each window shows its
        // screen at 16:9 again.
        const double shape = system_ == System::WiiU ? 16.0 / 9.0 : 0.0;
        video_.setCrop(screenCrop(layoutSwap_, int(w), int(h)), shape);
        if (screen2_.window()) {
            screen2_.uploadFrame(data, w, h, pitch, fmt);
            screen2_.setAspect(video_.aspect());
            screen2_.setCrop(screenCrop(!layoutSwap_, int(w), int(h)), shape);
        }
    } else {
        video_.setCrop({});
    }
}

void App::onAudio(const int16_t* stereo, size_t frames) {
    if (opt_.headless) {
        audioFramesSeen_ += frames;
        for (size_t i = 0; i < frames * 2; ++i) audioPeak_ = std::max(audioPeak_, std::abs(int(stereo[i])));
        return;
    }
    if (fastForward_ || netReplay_) return;  // no chipmunk audio while fast-forwarding / replaying
    audio_.push(stereo, frames);
}

int16_t App::onInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if (netInputActive()) {
        // LAN game (NES / SNES / N64): each port is a player, with their presses for this frame.
        if (port >= unsigned(kNetplayMaxPlayers)) return 0;
        const NetInput& in = netInputs_[port];
        switch (device & RETRO_DEVICE_MASK) {
        case RETRO_DEVICE_JOYPAD:
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(in.buttons);
            return id < 16 ? int16_t((in.buttons >> id) & 1) : 0;
        case RETRO_DEVICE_ANALOG:
            if (index <= RETRO_DEVICE_INDEX_ANALOG_RIGHT && id <= RETRO_DEVICE_ID_ANALOG_Y)
                return in.axes[index * 2 + id];
            if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON && id < 16) return (in.buttons >> id) & 1 ? 32767 : 0;
            return 0;
        default:
            return 0;
        }
    }
    return localInputState(port, device, index, id);
}

// This computer's controllers (port = player on this computer).
int16_t App::localInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_POINTER) return port == 0 && index == 0 ? pointerState(id) : 0;
    if ((device & RETRO_DEVICE_MASK) == RETRO_DEVICE_JOYPAD) {
        // Some systems get their face buttons rearranged (e.g. GameCube A on the bottom button).
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
            const int ours = input_.state(port, device, index, id);
            int mask = 0;
            for (int b = 0; b < 16; ++b)
                if ((ours >> inputRemap_[size_t(b)]) & 1) mask |= 1 << b;
            return int16_t(mask);
        }
        if (id < 16) return input_.state(port, device, index, unsigned(inputRemap_[id]));
    }
    return input_.state(port, device, index, id);
}

// Plug in a controller for player 1 and for every other player who has a controller (or keys).
// Leaving the rest unplugged matters for e.g. Wii games, which show a pointer per Wiimote.
void App::connectPorts() {
    if (!core_) return;
    const bool real = realWiimotes();
    for (int p = 0; p < Input::kMaxPlayers; ++p) {
        // LAN game: each computer has one player (its port 1); netplay plugs in a controller for
        // every player on every computer. Unplugging ports here would unplug the other players.
        if (lan_.active && !lan_.net && p > 0) break;
        const bool link = lanLinkLoad_ || (lan_.net && lan_.net->game().link);  // everyone's own game
        if (link && p > 0) {
            core_->setPortDevice(unsigned(p), RETRO_DEVICE_NONE);
            continue;
        }
        if (link) {
            core_->setPortDevice(0, RETRO_DEVICE_JOYPAD);
            continue;
        }
        if ((lanPending_ || lan_.net) && !lan_.portsSet && p > 0) {
            // NES / SNES / N64 LAN game, before it starts: just port 1 on every computer
            // (the same everywhere), then every player's port from frame 0.
            core_->setPortDevice(unsigned(p), RETRO_DEVICE_NONE);
            continue;
        }
        if (lan_.net && lan_.portsSet) {
            const auto& names = lan_.net->players();
            const bool player = size_t(p) < names.size() && !names[size_t(p)].empty();
            core_->setPortDevice(unsigned(p), player ? RETRO_DEVICE_JOYPAD : RETRO_DEVICE_NONE);
            continue;
        }
        // Real Wii Remotes: every slot stays plugged in (an unplugged slot can't take a real remote).
        const bool present = p == 0 || real || input_.controllerForPort(p) || input_.hasKeyboardKeys(p);
        // Wii U: player 1 is the GamePad; players 2-4 are Pro Controllers (Cemu's device id for one).
        const unsigned pad = system_ == System::WiiU && p > 0 ? RETRO_DEVICE_SUBCLASS(RETRO_DEVICE_JOYPAD, 2)
                                                              : RETRO_DEVICE_JOYPAD;
        core_->setPortDevice(unsigned(p), present ? pad : RETRO_DEVICE_NONE);
    }
}

bool App::realWiimotes() const {
    if (!core_ || system_ != System::Wii) return false;
    auto& opts = core_->options();
    auto it = opts.find("dolphin_wiimote_default_device");
    return it != opts.end() && it->second.value == "real";
}

// Which buttons Button Config shows, and how our buttons feed the core's, for the running game.
void App::updateInputProfile() {
    for (int i = 0; i < 16; ++i) inputRemap_[size_t(i)] = i;
    buttonRows_ = systemButtons(system_);
    if (system_ == System::GameCube) {
        // GameCube: A = bottom button, B = left, X = right, Y = top (Dolphin's default puts A on the right).
        inputRemap_[RETRO_DEVICE_ID_JOYPAD_A] = RETRO_DEVICE_ID_JOYPAD_B;
        inputRemap_[RETRO_DEVICE_ID_JOYPAD_B] = RETRO_DEVICE_ID_JOYPAD_Y;
        inputRemap_[RETRO_DEVICE_ID_JOYPAD_X] = RETRO_DEVICE_ID_JOYPAD_A;
        inputRemap_[RETRO_DEVICE_ID_JOYPAD_Y] = RETRO_DEVICE_ID_JOYPAD_X;
    } else if (system_ == System::Wii && core_) {
        auto& opts = core_->options();
        auto it = opts.find("dolphin_wiimote_default_device");
        const std::string device = it != opts.end() ? it->second.value : "nunchuk";
        buttonRows_ = wiiButtons(device);
        if (device == "nunchuk" || device == "upright" || device == "motionplus" || device == "motionplus_nunchuk") {
            // Wiimote A (the big button) on the bottom button, B (the trigger) on the right one.
            inputRemap_[RETRO_DEVICE_ID_JOYPAD_A] = RETRO_DEVICE_ID_JOYPAD_B;
            inputRemap_[RETRO_DEVICE_ID_JOYPAD_B] = RETRO_DEVICE_ID_JOYPAD_A;
        }
    }
}

// The mouse acts as the stylus / finger: libretro pointer coordinates run from
// -0x7fff to 0x7fff across the picture the core sent, wherever we drew it.
int16_t App::pointerState(unsigned id) const {
    int wx = 0, wy = 0;
    const Uint32 buttons = SDL_GetMouseState(&wx, &wy);  // relative to the window the mouse is over
    SDL_Window* over = SDL_GetMouseFocus();
    double fx = 0, fy = 0;
    bool inside = false;
    if (over && over == screen2_.window()) {
        const float pr = screen2_.pixelRatio();
        inside = screen2_.toFrame(int(wx * pr), int(wy * pr), fx, fy);
    } else if (!over || over == video_.window()) {
        const float pr = video_.pixelRatio();
        const int mx = int(wx * pr), my = int(wy * pr);
        inside = video_.toFrame(mx, my, fx, fy) && !(menuVisible() && my < menu_.height(text_));
    }
    switch (id) {
    case RETRO_DEVICE_ID_POINTER_X:
        return inside ? int16_t(std::lround((fx * 2.0 - 1.0) * 0x7fff)) : int16_t(-0x8000);
    case RETRO_DEVICE_ID_POINTER_Y:
        return inside ? int16_t(std::lround((fy * 2.0 - 1.0) * 0x7fff)) : int16_t(-0x8000);
    case RETRO_DEVICE_ID_POINTER_PRESSED:
        return inside && (buttons & SDL_BUTTON_LMASK) ? 1 : 0;
    case RETRO_DEVICE_ID_POINTER_COUNT:
        return inside && (buttons & SDL_BUTTON_LMASK) ? 1 : 0;
    case RETRO_DEVICE_ID_POINTER_IS_OFFSCREEN:
        return inside ? 0 : 1;
    default:
        return 0;
    }
}

// ============================================================================
// DS screen layout

void App::loadLayout() {
    layout_ = system_ == System::WiiU ? kSingleScreen : 0;  // Wii U: just the TV picture, like the real thing
    layoutSwap_ = false;
    const std::string v = cfg_.get("layout." + configSafe(gameTitle_));  // remembered per game, e.g. "4" or "0,swap"
    if (!v.empty()) {
        layout_ = std::clamp(std::atoi(v.c_str()), 0, kDsLayoutCount - 1);
        layoutSwap_ = v.find("swap") != std::string::npos;
    }
}

void App::applyLayout() {
    if (!core_ || !hasTwoScreens(system_)) return;
    if (!layoutAvailable(system_, layout_)) layout_ = 0;  // e.g. a sideways layout saved for a 3DS game
    const DsLayout& l = kDsLayouts[layout_];
    if (system_ == System::NDS) {
        core_->forceOption("melonds_number_of_screen_layouts", "1");
        core_->forceOption("melonds_screen_layout1", layoutSwap_ ? l.swapped : l.value);
    } else if (system_ == System::WiiU) {
        // One layout only (our menu switches them), and no button combo that changes it behind our back.
        core_->forceOption("cemu_number_of_screen_layouts", "1");
        core_->forceOption("cemu_next_screen_layout_button", "Disabled");
        const bool single = layout_ == kSingleScreen;
        core_->forceOption("cemu_screen_layout1", single && layoutSwap_ ? "GamePad Screen" : l.wiiu);
        // Separate Windows: the TV stays on top; which window gets which screen is ours to swap.
        core_->forceOption("cemu_drc_position", !single && layoutSwap_ && layout_ != kSeparate ? "swapped" : "normal");
    } else {
        core_->forceOption("citra_layout_option", l.ctr);
        core_->forceOption("citra_swap_screen", layoutSwap_ && layout_ != kSeparate ? "Bottom" : "Top");
    }
}

void App::fitWindowToGame() {
    SDL_Window* win = video_.window();
    if (!win || !core_ || video_.isFullscreen() || (SDL_GetWindowFlags(win) & SDL_WINDOW_MAXIMIZED)) return;
    const auto& g = core_->avInfo().geometry;
    const bool sideways = core_->rotation() % 2 == 1;
    double baseH = std::max(1u, sideways ? g.base_width : g.base_height);  // height as shown
    double aspect = g.aspect_ratio > 0 ? g.aspect_ratio : (sideways ? double(g.base_height) : double(g.base_width)) / baseH;
    const float pr = video_.pixelRatio();
    const int bar = menu_.height(text_);
    // Separate Windows: this window shows one screen, and the other window goes next to it.
    const bool separate = separateWindows() && !sideways;
    double aspect2 = 0;
    if (separate) {
        const double pixelShape = aspect / (double(g.base_width) / baseH);
        const SDL_Rect c1 = screenCrop(layoutSwap_, int(g.base_width), int(g.base_height));
        const SDL_Rect c2 = screenCrop(!layoutSwap_, int(g.base_width), int(g.base_height));
        aspect = double(c1.w) / std::max(1, c1.h) * pixelShape;
        aspect2 = double(c2.w) / std::max(1, c2.h) * pixelShape;
        baseH = std::max(1, c1.h);
        if (system_ == System::WiiU) {  // both screens are shown at 16:9, at the TV's size
            aspect = aspect2 = 16.0 / 9.0;
            baseH = std::max(1u, g.base_height);
        }
    }

    // Largest size (up to 3x) that fits on the screen.
    SDL_Rect usable{0, 0, 1280, 720};
    const int display = SDL_GetWindowDisplayIndex(win);
    SDL_GetDisplayUsableBounds(display < 0 ? 0 : display, &usable);
    const double maxH = usable.h * pr - 80 * text_.scale() - bar;
    const double maxW = usable.w * pr - 40 * text_.scale();
    double scale = std::min({3.0, maxH / baseH, maxW / (baseH * (aspect + aspect2))});
    // Whole steps from 2x, half steps from 1x, and below 1x (very large pictures) just fit the screen.
    scale = scale >= 2.0 ? std::floor(scale) : scale >= 1.0 ? std::floor(scale * 2) / 2 : scale;
    const int h = int(std::lround(baseH * scale)) + bar;
    const int w = int(std::lround(baseH * scale * aspect));
    SDL_SetWindowSize(win, int(w / pr), int(h / pr));
    if (separate && screen2_.window()) {
        // Same picture size in both windows, side by side and centred together.
        const int h2 = int((h - bar) / pr), w2 = int(std::lround(h2 * aspect2));
        const int gap = int(12 * text_.scale() / pr);
        const int x = usable.x + std::max(0, (usable.w - int(w / pr) - gap - w2) / 2);
        const int y = usable.y + std::max(0, (usable.h - int(h / pr)) / 2);
        SDL_SetWindowPosition(win, x, y);
        SDL_SetWindowSize(screen2_.window(), w2, h2);
        SDL_SetWindowPosition(screen2_.window(), x + int(w / pr) + gap, y + int(bar / pr));
        screen2_.onResize();
    } else {
        SDL_SetWindowPosition(win, SDL_WINDOWPOS_CENTERED_DISPLAY(display < 0 ? 0 : display),
                              SDL_WINDOWPOS_CENTERED_DISPLAY(display < 0 ? 0 : display));
    }
    video_.onResize();
}

bool App::separateWindows() const { return core_ && hasTwoScreens(system_) && layout_ == kSeparate; }

// Where one screen is in a Vertical-layout frame. Worked out from the width, so it holds at any
// resolution and with a gap between the screens: DS screens are 4:3; the 3DS top screen is
// 400x240 and its 320x240 bottom screen is centred under it.
SDL_Rect App::screenCrop(bool bottom, int fw, int fh) const {
    if (system_ == System::N3DS) {
        const int sh = std::min(fh / 2, fw * 3 / 5);
        return bottom ? SDL_Rect{fw / 10, fh - sh, fw * 4 / 5, sh} : SDL_Rect{0, 0, fw, sh};
    }
    if (system_ == System::WiiU) {  // Cemu's Top Bottom: TV in the top 70%, GamePad in the bottom 30%
        const int split = fh * 70 / 100;
        return bottom ? SDL_Rect{0, split, fw, fh - split} : SDL_Rect{0, 0, fw, split};
    }
    const int sh = std::min(fh / 2, fw * 3 / 4);
    return bottom ? SDL_Rect{0, fh - sh, fw, sh} : SDL_Rect{0, 0, fw, sh};
}

void App::updateScreenWindow() {
    const bool want = separateWindows() && video_.window();
    if (want && !screen2_.window()) {
        std::string err;
        // No vsync here: the main window already waits for it.
        if (!screen2_.init(false, false, 512, 384, err, "Nintendall", false)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            osd("Couldn't open the second window");
            setLayout(0, layoutSwap_);
            return;
        }
        SDL_SetWindowMinimumSize(screen2_.window(), 128, 96);
        SDL_RaiseWindow(video_.window());  // keep the main window (and its menus) in front
    } else if (!want && screen2_.window()) {
        screen2_.shutdown();
    }
    if (!want) video_.setCrop({});
    if (screen2_.window())
        SDL_SetWindowTitle(screen2_.window(), (std::string(system_ == System::WiiU ? (layoutSwap_ ? "TV" : "GamePad")
                                                                           : (layoutSwap_ ? "Top Screen" : "Bottom Screen")) +
                                                   " - " +
                                               gameTitle_).c_str());
}

// Switch the DS / 3DS / Wii U screen layout (remembered per game).
void App::setLayout(int layout, bool swap) {
    if (!core_ || !hasTwoScreens(system_) || !layoutAvailable(system_, layout)) return;
    const bool wasSeparate = separateWindows();
    layout_ = layout;
    layoutSwap_ = swap;
    cfg_.set("layout." + configSafe(gameTitle_), std::to_string(layout_) + (layoutSwap_ ? ",swap" : ""));
    cfg_.save();
    applyLayout();
    updateScreenWindow();
    pendingFitFrames_ = 30;
    avChanged_ = false;
    if (paused_) {  // show the new layout right away
        core_->run();
        audio_.clear();
        if (avChanged_) {
            fitWindowToGame();
            pendingFitFrames_ = 0;
        }
    }
    // Separate Windows uses the same emulator picture as Vertical, so its size may not change: fit now.
    if (wasSeparate || separateWindows()) fitWindowToGame();
}

bool App::onRumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    return input_.rumble(port, effect, strength);
}

void App::onMessage(const std::string& msg, unsigned durationMs) {
    if (msg.empty()) return;  // melonDS sends empty messages every frame; they'd wipe ours
    osd(msg, std::max(1500u, durationMs));
}

void App::hwOutputSize(unsigned& w, unsigned& h) const {
    w = h = 0;
    const SDL_Rect& r = video_.gameRect();  // where the last frame was drawn
    if (opt_.headless || r.w <= 0 || r.h <= 0 || !core_) return;
    const bool sideways = core_->rotation() % 2 == 1;  // the picture is turned after copying
    w = unsigned(sideways ? r.h : r.w);
    h = unsigned(sideways ? r.w : r.h);
    // Separate Windows: each window shows only part of the frame, so copy it whole (shrinking it
    // to one window's size would leave each screen with a fraction of its pixels).
    if (separateWindows()) w = h = 0;
}

void App::onAvInfoChanged() {
    if (!core_) return;
    const auto& av = core_->avInfo();
    double aspect = av.geometry.aspect_ratio;
    if (aspect <= 0 && av.geometry.base_height > 0) {
        aspect = double(av.geometry.base_width) / av.geometry.base_height;
        if (core_->rotation() % 2) aspect = 1.0 / aspect;
    }
    video_.setAspect(aspect);
    video_.setRotation(core_->rotation());
    audio_.setInputRate(av.timing.sample_rate);
    avChanged_ = true;  // the window is refit after the frame, once the core has finished changing things
}

// ============================================================================
// Events

void App::handleEvent(const SDL_Event& e) {
    std::string msg;
    if (input_.handleDeviceEvent(e, &msg)) {
        if (!msg.empty()) osd(msg);
        connectPorts();  // a controller came or went: plug / unplug that player
        return;
    }

    // The second screen's window: its mouse is the touch screen (read directly in pointerState),
    // and closing it goes back to one window.
    if (const Uint32 id2 = screen2_.windowId()) {
        Uint32 id = 0;
        if (e.type == SDL_WINDOWEVENT) id = e.window.windowID;
        else if (e.type == SDL_MOUSEMOTION) id = e.motion.windowID;
        else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) id = e.button.windowID;
        else if (e.type == SDL_MOUSEWHEEL) id = e.wheel.windowID;
        if (id == id2) {
            if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) screen2_.onResize();
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focusLost_ = true;
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focusLost_ = false;
                if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    setLayout(0, layoutSwap_);
                    osd("Back to one window (Layout menu)");
                }
            }
            return;
        }
    }

    // Mouse position in renderer pixels.
    const float pr = video_.pixelRatio();
    if (e.type == SDL_MOUSEMOTION) {
        mouseX_ = int(e.motion.x * pr);
        mouseY_ = int(e.motion.y * pr);
        lastMouseMove_ = SDL_GetTicks();
    } else if (e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP) {
        mouseX_ = int(e.button.x * pr);
        mouseY_ = int(e.button.y * pr);
        lastMouseMove_ = SDL_GetTicks();
    }

    // Downloading an update: only Esc (cancel) and closing the window do anything.
    if (download_) {
        if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) download_->cancel = true;
        if (e.type == SDL_DROPFILE) SDL_free(e.drop.file);
        if (e.type == SDL_QUIT || (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE))
            download_->cancel = true;
        if (e.type != SDL_QUIT && e.type != SDL_WINDOWEVENT) return;
    }

    switch (e.type) {
    case SDL_QUIT:
        quit_ = true;
        return;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_CLOSE && e.window.windowID == video_.windowId()) quit_ = true;
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) video_.onResize();
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) focusLost_ = true;
        if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) focusLost_ = false;
        return;
    case SDL_DROPFILE: {
        // Drag a game onto the window: detect it and play it.
        const fs::path p = fs::u8path(e.drop.file);
        SDL_free(e.drop.file);
        menu_.close();
        buttonConfig_.close();
        settings_.close();
        lanDialog_.close();
        startGame(p);
        return;
    }
    default:
        break;
    }

    // Dialog windows are modal: they get everything.
    if (settings_.isOpen()) {
        if (core_) settings_.handleEvent(e, mouseX_, mouseY_, *core_);
        else settings_.close();
        return;
    }
    if (buttonConfig_.isOpen()) {
        buttonConfig_.handleEvent(e, mouseX_, mouseY_, input_);
        return;
    }
    if (lanDialog_.isOpen()) {
        lanDialog_.handleEvent(e, mouseX_, mouseY_);
        LanDialog::Request req;
        if (lanDialog_.takeRequest(req)) startLanGame(req);
        return;
    }

    // Fullscreen toggle works everywhere.
    if (e.type == SDL_KEYDOWN && !e.key.repeat &&
        (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT)))) {
        menu_.close();
        video_.toggleFullscreen();
        return;
    }

    // Menu bar
    if (menuVisible() || menu_.isOpen()) {
        buildMenus();
        int action = 0;
        if (menu_.handleEvent(e, mouseX_, mouseY_, action)) {
            if (action) doAction(action);
            return;
        }
    }

    // LAN game lobby (waiting for players, or why it couldn't start): its buttons, Enter and Esc.
    if (lanLobbyShown()) {
        switch (lanLobby_.handleEvent(e, mouseX_, mouseY_, lan_.status, lanLobbyInfo())) {
        case LanLobby::Click::Start: lanStart(); break;
        case LanLobby::Click::Leave: leaveLan(""); break;
        case LanLobby::Click::Copy:
            SDL_SetClipboardText(lan_.status.code.c_str());
            osd("Host code copied - paste it to your friends");
            break;
        default: break;
        }
        if (e.type == SDL_KEYDOWN || e.type == SDL_MOUSEBUTTONDOWN) return;
    }

    // Home screen (no game running): pick a console and a game.
    if (!core_ && !opt_.headless) {
        fs::path launch;
        if (home_.handleEvent(e, mouseX_, mouseY_, text_, homeArea(), launch)) {
            if (!launch.empty()) startGame(launch);
            return;
        }
    }

    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        const SDL_Keysym& k = e.key.keysym;
        if (ctrlHeld(k)) {
            switch (k.sym) {
            case SDLK_o: doAction(ActOpen); return;
            case SDLK_r: doAction(ActReset); return;
            case SDLK_p: doAction(ActPause); return;
            case SDLK_w: doAction(ActCloseGame); return;
            default: return;
            }
        }
        switch (k.sym) {
        case SDLK_F10: menu_.openFirst(); return;
        case SDLK_ESCAPE:
            if (video_.isFullscreen()) video_.toggleFullscreen();
            return;
        case SDLK_PAUSE: doAction(ActPause); return;
        case SDLK_F5: doAction(ActSaveState); return;
        case SDLK_F9: doAction(ActLoadState); return;
        case SDLK_F6: doAction(ActSlot + std::max(1, slot_ - 1)); return;
        case SDLK_F7: doAction(ActSlot + std::min(kSlots, slot_ + 1)); return;
        case SDLK_F12: takeScreenshot(); return;
        default: return;
        }
    }

    if (e.type == SDL_CONTROLLERBUTTONDOWN && core_) {
        // Home/Guide button, or Select+Start together, pauses.
        SDL_GameController* gc = SDL_GameControllerFromInstanceID(e.cbutton.which);
        const Uint8 b = e.cbutton.button;
        const bool combo = gc && ((b == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) ||
                                  (b == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)));
        if (b == SDL_CONTROLLER_BUTTON_GUIDE || combo) doAction(ActPause);
    }
}

void App::doAction(int a) {
    if (a >= ActRecent && a < ActRecent + kRecent) {
        const auto list = recentGames();
        const size_t i = size_t(a - ActRecent);
        if (i < list.size()) startGame(fs::u8path(list[i]));
        return;
    }
    if (a > ActSlot && a <= ActSlot + kSlots) {
        slot_ = a - ActSlot;
        osd("State slot " + std::to_string(slot_));
        return;
    }
    if ((a >= ActLayout && a < ActLayout + kDsLayoutCount) || a == ActLayoutSwap) {
        if (!core_ || !hasTwoScreens(system_)) return;
        if (a != ActLayoutSwap && !layoutAvailable(system_, a - ActLayout)) return;
        if (a == ActLayoutSwap) setLayout(layout_, !layoutSwap_);
        else setLayout(a - ActLayout, layoutSwap_);
        return;
    }
    switch (a) {
    case ActOpen: openFileDialog(); break;
    case ActGamesFolder:
        createGameFolders(home_.gamesRoot());
        openFolder(home_.gamesRoot());
        break;
    case ActCoversFolder:
        createGameFolders(home_.coversRoot());
        openFolder(home_.coversRoot());
        break;
    case ActCloseGame: stopGame(); break;
    case ActExit: quit_ = true; break;
    case ActPause:
        if (core_ && !lanBlocks("Pausing")) paused_ = !paused_;
        break;
    case ActReset:
        if (core_ && !lanBlocks("Reset")) {
            core_->reset();
            paused_ = false;
            osd("Reset");
        }
        break;
    case ActFastForward:
        if (!lanBlocks("Fast forward")) fastForwardToggled_ = !fastForwardToggled_;
        break;
    case ActSaveState:
        if (!lanBlocks("Save states")) saveState();
        break;
    case ActLoadState:
        if (!lanBlocks("Save states")) loadState();
        break;
    case ActButtonConfig: buttonConfig_.open(system_, core_ ? buttonRows_ : systemButtons(System::Unknown)); break;
    case ActConsoleSettings:
        if (core_ && !lanBlocks("Console Settings")) settings_.open(system_, *core_);
        break;
    case ActLanHost: openLanDialog(LanDialog::Kind::Host); break;
    case ActLanJoin: openLanDialog(LanDialog::Kind::Join); break;
    case ActLanStart:
        if (lan_.active && lan_.req.kind == LanDialog::Kind::Host) lanStart();
        break;
    case ActLanDelayUp:
    case ActLanDelayDown:
        if (lan_.active && !lan_.net && lan_.req.kind == LanDialog::Kind::Host) {
            const int d = std::clamp(int(lan_.status.delay) + (a == ActLanDelayUp ? 1 : -1), 1, 20);
            lan_.api.setDelay(unsigned(d));
            lan_.status.delay = unsigned(d);
            osd("Input delay: " + std::to_string(d) + (d == 1 ? " frame" : " frames"));
        }
        break;
    case ActLanLeave: leaveLan(""); break;
    case ActLanHelp:
        showMessage("LAN Games",
                    "Play NES, SNES, N64, GameCube and Wii games together on several computers, and link "
                    "Game Boy, Game Boy Color and GBA games (Pokemon trades and battles, ...).\n\n"
                    "Host: LAN > Host Game, pick the game and click Host. Give your friends your address (it "
                    "appears in the window; GameCube and Wii games also show a host code, which works over the "
                    "internet). When everyone has joined, click Start Game.\n\n"
                    "Join: LAN > Join Game, type the host's address (or host code), pick the game and click Join. "
                    "Nintendall finds the host's game in your Games folder.\n\n"
                    "Everyone needs the same version of Nintendall. Except for Game Boy links, everyone needs "
                    "the same game (same region and version) and plays with the host's save data. Pausing, "
                    "fast forward and save states are off during LAN games. Input delay: lower feels quicker; raise it if the game stutters.\n\n"
                    "Game Boy, Game Boy Color and GBA: everyone picks their own game and plays with their own "
                    "save, like real link cables. Friends can join after the game started.\n\n"
                    "NES, SNES, N64 and Game Boy games: friends join with the host's address, on the same "
                    "network or on one virtual LAN. GameCube and Wii: if a host code doesn't connect, computers on one virtual "
                    "LAN can join with the host's address in it.",
                    false);
        break;
    case ActWiiUKeys: importWiiUKeys(); break;
    case ActPairWiimotes: startWiimotePairing(); break;
    case ActFullscreen: video_.toggleFullscreen(); break;
    case ActControls:
        showMessage("Controls",
                    "Keyboard (change these in Options > Button Config):\n"
                    "  Arrow keys = D-pad\n  X = A,  Z = B,  S = X,  A = Y\n  Q / W = L / R,  1 / 3 = L2 / R2\n"
                    "  Enter = Start,  Right Shift = Select\n\n"
                    "Shortcuts:\n  Ctrl+O = Open game\n  Ctrl+P = Pause,  Ctrl+R = Reset\n"
                    "  F5 = Save state,  F9 = Load state,  F6 / F7 = Change slot\n"
                    "  Hold Tab = Fast forward\n  F11 = Fullscreen,  Esc = Leave fullscreen\n  F12 = Screenshot\n\n"
                    "Controllers work automatically. Home button, or Select + Start, pauses.\n\n"
                    "Home screen (no game running):\n  Click a console, then click a game to play it\n"
                    "  Arrow keys / D-pad = Choose a game,  Enter / A = Play\n"
                    "  Tab / Shift+Tab or L / R = Next / previous console\n"
                    "  Ctrl + / Ctrl - = Bigger / smaller covers,  Ctrl 0 = Normal size",
                    false);
        break;
    case ActAbout:
        showMessage("About Nintendall", std::string("Nintendall ") + NINTENDALL_VERSION +
                                            "\n\nOpen or drag in a game and Nintendall plays it.\n\n"
                                            "Built with open-source software. See THIRD_PARTY_LICENSES.md "
                                            "in the Nintendall folder.",
                    false);
        break;
    default: break;
    }
}

void App::openFileDialog() {
    menu_.close();
    audio_.clear();
    bool unavailable = false;
    const std::string path = openGameDialog(video_.window(), unavailable);
    nextFrame_ = nowSeconds();
    if (unavailable) {
        showMessage("Open Game", "Drag and drop a game file onto the Nintendall window to play it.", false);
        return;
    }
    if (!path.empty()) startGame(fs::u8path(path));
}

// ---- Wii U keys -------------------------------------------------------------

namespace {
// A keys.txt line: 32 hex digits (one AES key), optionally followed by "# a note".
std::string keyFromLine(const std::string& line) {
    std::string hex;
    for (char c : line) {
        if (c == '#') break;
        if (std::isxdigit(static_cast<unsigned char>(c))) hex += char(std::tolower(static_cast<unsigned char>(c)));
        else if (!std::isspace(static_cast<unsigned char>(c))) return "";  // not a key line
    }
    // Cemu writes a template keys.txt containing this example key; it isn't a real one.
    if (hex == "541b9889519b27d363cd21604b97c67a") return "";
    return hex.size() == 32 ? hex : "";
}

std::vector<std::string> readLines(const fs::path& p) {
    std::vector<std::string> lines;
    std::ifstream f(p);
    for (std::string l; std::getline(f, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(l);
    }
    return lines;
}
}  // namespace

fs::path App::wiiUKeysFile() const { return paths_.systemDirFor("cemu") / "Cemu" / "keys.txt"; }

bool App::needsWiiUKeys(const fs::path& game) const {
    std::string ext = game.extension().u8string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
    if (ext != ".wud" && ext != ".wux") return false;  // only disc images are encrypted with the console's keys
    std::error_code ec;
    if (fs::exists(fs::path(game).replace_extension(".key"), ec)) return false;  // game.key next to game.wux
    for (const auto& l : readLines(wiiUKeysFile()))
        if (!keyFromLine(l).empty()) return false;
    return true;
}

void App::importWiiUKeys() {
    menu_.close();
    audio_.clear();
    bool unavailable = false;
    const std::string chosen =
        ::openFileDialog(video_.window(), "Choose your Wii U keys file", "Key files", {"txt", "key"}, unavailable);
    nextFrame_ = nowSeconds();
    if (unavailable) {
        showMessage("Wii U Keys", "Put your keys.txt in this folder:\n" + wiiUKeysFile().parent_path().u8string(),
                    false);
        return;
    }
    if (chosen.empty()) return;

    // Add every key that isn't there yet; keep the notes ("# Game name") that come with them.
    const fs::path dest = wiiUKeysFile();
    std::set<std::string> have;
    for (const auto& l : readLines(dest))
        if (const std::string k = keyFromLine(l); !k.empty()) have.insert(k);
    std::vector<std::string> add;
    int found = 0;
    for (const auto& l : readLines(fs::u8path(chosen))) {
        const std::string k = keyFromLine(l);
        if (k.empty()) continue;
        ++found;
        if (have.insert(k).second) add.push_back(l);
    }
    if (found == 0) {
        showMessage("Wii U Keys",
                    "That file doesn't contain any Wii U keys.\n\nA keys file has one key per line: 32 letters and "
                    "numbers (0-9, a-f), optionally followed by # and a note.",
                    true);
        return;
    }
    std::error_code ec;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::app);
    for (const auto& l : add) out << l << "\n";
    if (!out) {
        showMessage("Wii U Keys", "Couldn't write " + dest.u8string(), true);
        return;
    }
    showMessage("Wii U Keys",
                add.empty() ? "Those keys were already added."
                            : "Added " + std::to_string(add.size()) + (add.size() == 1 ? " key" : " keys") +
                                  ". Encrypted Wii U disc images (.wud / .wux) can now be played.",
                false);
}

void App::showMessage(const std::string& title, const std::string& text, bool error) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
    if (opt_.headless || !video_.window()) return;
    audio_.clear();
    SDL_ShowSimpleMessageBox(error ? SDL_MESSAGEBOX_ERROR : SDL_MESSAGEBOX_INFORMATION, title.c_str(), text.c_str(),
                             video_.window());
    nextFrame_ = nowSeconds();
}

void App::osd(const std::string& msg, unsigned ms) {
    osdText_ = msg;
    osdUntil_ = SDL_GetTicks() + ms;
    if (opt_.headless) std::fprintf(stderr, "[osd] %s\n", msg.c_str());
}

// ============================================================================
// Window layout and drawing

float App::uiScale() const {
    float s = video_.pixelRatio();
#if defined(_WIN32)
    float hdpi = 96.0f;
    const int d = video_.window() ? SDL_GetWindowDisplayIndex(video_.window()) : 0;
    if (SDL_GetDisplayDPI(d < 0 ? 0 : d, nullptr, &hdpi, nullptr) == 0 && hdpi > 0) s *= hdpi / 96.0f;
#endif
    return std::clamp(s, 1.0f, 4.0f);
}

bool App::menuVisible() const {
    // Always shown in a window and on the home screen; in fullscreen games it appears when the mouse goes to the top.
    if (!video_.isFullscreen() || !core_) return true;
    return menu_.isOpen() || (SDL_GetTicks() - lastMouseMove_ < 3000 && mouseY_ < menu_.height(text_) * 2);
}

SDL_Rect App::gameArea() const {
    const int bar = video_.isFullscreen() ? 0 : menu_.height(text_);
    return {0, bar, video_.width(), std::max(0, video_.height() - bar)};
}

SDL_Rect App::homeArea() const {
    const int bar = menu_.height(text_);
    return {0, bar, video_.width(), std::max(0, video_.height() - bar)};
}

void App::buildMenus() {
    const bool game = core_ != nullptr;
    std::vector<Menu> menus;

    Menu file{"File", {}};
    file.items.push_back({"Open...", "Ctrl+O", ActOpen});
    file.items.push_back({"Open Games Folder", "", ActGamesFolder});
    file.items.push_back({"Open Box Covers Folder", "", ActCoversFolder});
    file.items.push_back({"Close Game", "Ctrl+W", ActCloseGame, game});
    const auto recent = recentGames();
    if (!recent.empty()) {
        file.items.push_back({});
        for (size_t i = 0; i < recent.size(); ++i)
            file.items.push_back({std::to_string(i + 1) + "  " + fs::u8path(recent[i]).filename().u8string(), "",
                                  int(ActRecent + i)});
    }
    file.items.push_back({});
    file.items.push_back({"Exit", "Alt+F4", ActExit});
    menus.push_back(file);

    Menu emu{"Emulation", {}};
    emu.items.push_back({"Pause", "Ctrl+P", ActPause, game, paused_});
    emu.items.push_back({"Reset", "Ctrl+R", ActReset, game});
    emu.items.push_back({});
    emu.items.push_back({"Fast Forward", "Hold Tab", ActFastForward, true, fastForwardToggled_});
    menus.push_back(emu);

    Menu state{"State", {}};
    const bool states = game && system_ != System::WiiU;  // Cemu can't save states
    state.items.push_back({"Save State", "F5", ActSaveState, states});
    state.items.push_back({"Load State", "F9", ActLoadState, states});
    state.items.push_back({});
    for (int i = 1; i <= kSlots; ++i) {
        std::string label = "Slot " + std::to_string(i);
        std::error_code ec;
        if (game && menu_.isOpen() && !fs::exists(statePath(i), ec)) label += "  (empty)";
        state.items.push_back({label, "", ActSlot + i, true, slot_ == i});
    }
    menus.push_back(state);

    // Layout: how the two DS / 3DS / Wii U screens are arranged (other systems have one screen).
    const bool twoScreens = game && hasTwoScreens(system_);
    Menu layout{"Layout", {}};
    for (const int i : kLayoutMenuOrder) {
        if (i < 0) {
            layout.items.push_back({});
            continue;
        }
        const bool ok = twoScreens && layoutAvailable(system_, i);
        layout.items.push_back({kDsLayouts[i].label, "", ActLayout + i, ok, ok && layout_ == i});
    }
    layout.items.push_back({});
    layout.items.push_back({"Swap Screens", "", ActLayoutSwap, twoScreens && (layout_ < kFirstSideways || layout_ == kSeparate),
                            twoScreens && layoutSwap_});
    menus.push_back(layout);

    Menu options{"Options", {}};
    options.items.push_back({"Button Config...", "", ActButtonConfig});
    options.items.push_back({"Console Settings...", "", ActConsoleSettings, game && !lan_.active});
    options.items.push_back({"Wii U Keys...", "", ActWiiUKeys});
    options.items.push_back({pairJob_ ? "Pairing Wii Remotes..." : "Pair Wii Remotes", "", ActPairWiimotes, !pairJob_});
    options.items.push_back({});
    options.items.push_back({"Fullscreen", "F11", ActFullscreen, true, video_.isFullscreen()});
    menus.push_back(options);

    // LAN: GameCube and Wii games with friends (Dolphin's netplay).
    const bool lanHost = lan_.active && lan_.req.kind == LanDialog::Kind::Host;
    Menu lan{"LAN", {}};
    lan.items.push_back({"Host Game...", "", ActLanHost, !lan_.active});
    lan.items.push_back({"Join Game...", "", ActLanJoin, !lan_.active});
    lan.items.push_back({});
    lan.items.push_back({"Start Game", "", ActLanStart, lanHost && lan_.status.state == LanState::Lobby});
    lan.items.push_back({"More Input Delay", "", ActLanDelayUp, lanHost && !lan_.net});
    lan.items.push_back({"Less Input Delay", "", ActLanDelayDown, lanHost && !lan_.net});
    lan.items.push_back({"Leave LAN Game", "", ActLanLeave, lan_.active});
    lan.items.push_back({});
    lan.items.push_back({"How LAN Games Work", "", ActLanHelp});
    menus.push_back(lan);

    Menu help{"Help", {}};
    help.items.push_back({"Controls", "", ActControls});
    help.items.push_back({"About Nintendall", "", ActAbout});
    menus.push_back(help);

    menu_.setMenus(std::move(menus));
}

void App::draw() {
    const int W = video_.width(), H = video_.height();
    const float s = text_.scale();
    video_.clear(kBlack);
    const SDL_Rect area = gameArea();

    if (core_ && video_.hasFrame()) {
        video_.drawGame(area);
    }
    if (!core_) home_.draw(video_, text_, homeArea(), mouseX_, mouseY_);

    if (core_ && paused_) {
        const std::string t = "Paused";
        const int tw = text_.width(t, Text::Large), th = text_.lineHeight(Text::Large);
        const int bx = area.x + (area.w - tw) / 2 - int(16 * s), by = area.y + (area.h - th) / 2 - int(8 * s);
        video_.fillRect(bx, by, tw + int(32 * s), th + int(16 * s), {0, 0, 0, 170});
        text_.draw(bx + int(16 * s), by + int(8 * s), t, kWhite, Text::Large);
    }
    if (fastForward_ && core_ && !paused_) text_.draw(area.x + int(10 * s), area.y + int(8 * s), "Fast Forward", kWhite);
    if (lanLobbyShown()) lanLobby_.draw(video_, text_, area, mouseX_, mouseY_, lan_.status, lanLobbyInfo());

    // On-screen message (bottom left).
    if (!osdText_.empty() && !SDL_TICKS_PASSED(SDL_GetTicks(), osdUntil_)) {
        const int tw = text_.width(osdText_), th = text_.lineHeight();
        const int x = area.x + int(10 * s), y = area.y + area.h - th - int(18 * s);
        video_.fillRect(x, y, tw + int(16 * s), th + int(8 * s), {0, 0, 0, 180});
        text_.draw(x + int(8 * s), y + int(4 * s), osdText_, kWhite);
    }

    if (menuVisible()) {
        buildMenus();
        menu_.draw(video_, text_, W);
    }
    buttonConfig_.draw(video_, text_, input_, W, H);
    drawUpdateDownload();
    if (core_) settings_.draw(video_, text_, *core_, W, H);
    lanDialog_.draw(video_, text_, W, H);
    video_.present();

    if (screen2_.window()) {
        screen2_.clear(kBlack);
        if (core_ && screen2_.hasFrame()) screen2_.drawGame({0, 0, screen2_.width(), screen2_.height()});
        screen2_.present();
    }
}

// ============================================================================
// Wii U shader cache

fs::path App::wiiUShaderCache() const { return paths_.systemDirFor("cemu") / "Cemu" / "shaderCache"; }

void App::clearWiiUShaderCache() {
    std::error_code ec;
    fs::remove_all(wiiUShaderCache(), ec);
    if (ec) std::fprintf(stderr, "Couldn't clear the Wii U shader cache: %s\n", ec.message().c_str());
}

// ============================================================================
// New version popup

struct App::UpdateJob {
    std::atomic<bool> done{false};
    UpdateInfo info;
};

void App::startUpdateCheck() {
    if (cfg_.get("update.check") == "false") return;  // turned off in nintendall.cfg
    auto job = std::make_shared<UpdateJob>();
    updateJob_ = job;
    std::thread([job] {
        job->info = checkForUpdate(NINTENDALL_VERSION);
        job->done = true;
    }).detach();
}

void App::checkUpdateResult() {
    if (!updateJob_ || !updateJob_->done) return;
    // Never during a game: once one has been started, the popup is dropped for this time.
    if (core_ || playedSinceLaunch_) {
        updateJob_.reset();
        return;
    }
    // Wait for menus and dialogs to close first.
    if (menu_.isOpen() || buttonConfig_.isOpen() || settings_.isOpen()) return;
    const UpdateInfo info = updateJob_->info;
    updateJob_.reset();
    if (!info.available || cfg_.get("update.skip") == info.version) return;

    // Installed on Windows: Update Now does everything. Otherwise: Download opens the release page.
    const bool self = canUpdateItself(paths_.base, paths_.portable) && !info.setupUrl.empty();
    std::string text = "Nintendall " + info.version + " is available. You have " NINTENDALL_VERSION ".";
    if (!info.notes.empty()) text += "\n\nWhat's new:\n\n" + info.notes;
    text += self ? "\n\nUpdate Now downloads it, closes Nintendall, installs it and starts the new version.\n"
                   "Windows will ask for permission to install it."
                 : "\n\nDownload opens the release page, where you can get the new setup file.";
    enum { Download = 1, Skip, Later };
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, Download, self ? "Update Now" : "Download"},
        {0, Skip, "Skip"},
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, Later, "Later"},
    };
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_INFORMATION | SDL_MESSAGEBOX_BUTTONS_LEFT_TO_RIGHT, video_.window(),
                                 "New version of Nintendall", text.c_str(), 3, buttons, nullptr};
    int choice = Later;
    SDL_ShowMessageBox(&box, &choice);
    if (choice == Download) {
        if (self) startUpdateDownload(info);
        else SDL_OpenURL(info.pageUrl.c_str());
    } else if (choice == Skip) {
        cfg_.set("update.skip", info.version);  // asked again only for a newer version than this
        saveConfig();
    }
    nextFrame_ = nowSeconds();
}

void App::startUpdateDownload(const UpdateInfo& info) {
    auto job = std::make_shared<DownloadJob>();
    job->info = info;
    std::error_code ec;
    fs::remove_all(updateFolder(), ec);
    fs::create_directories(updateFolder(), ec);
    job->file = updateFolder() / ("Nintendall-" + info.version + "-setup.exe");
    download_ = job;
    std::thread([job] {
        const int status = httpDownload(job->info.setupUrl, job->file, [&](uint64_t got, uint64_t total) {
            job->got = got;
            job->total = total;
            return !job->cancel.load();
        });
        if (job->cancel) {
            job->error = "cancelled";
        } else if (status != 200) {
            job->error = "The download didn't finish. Check the internet connection and try again.";
        } else if (sha256File(job->file) != job->info.setupSha256) {
            // Not exactly the file GitHub published: never run it.
            job->error = "The downloaded file was damaged, so it wasn't installed. Please try again.";
        } else {
            job->ok = true;
        }
        if (!job->ok) {
            std::error_code ec2;
            fs::remove(job->file, ec2);
        }
        job->done = true;
    }).detach();
}

void App::checkUpdateDownload() {
    if (!download_ || !download_->done) return;
    const auto job = download_;
    download_.reset();
    if (job->error == "cancelled") {
        osd("Update cancelled");
        return;
    }
    std::string error = job->error;
    if (job->ok) {
        cfg_.set("update.installing", job->info.version);  // checked when Nintendall starts again
        saveConfig();
        const InstallStart started = startInstallingUpdate(job->file, paths_.base, error);
        if (started == InstallStart::Started) {
            quit_ = true;  // the setup waits for Nintendall to close, installs and starts it again
            return;
        }
        cfg_.set("update.installing", "");
        saveConfig();
        if (started == InstallStart::Refused) {
            osd("Update not installed - Windows needs your permission to install it", 6000);
            return;
        }
    }
    showMessage("Update failed", error + "\n\nYou can also download it from the release page:\n" + job->info.pageUrl, true);
}

void App::drawUpdateDownload() {
    if (!download_) return;
    const int W = video_.width(), H = video_.height();
    const float s = text_.scale();
    video_.fillRect(0, 0, W, H, {0, 0, 0, 170});
    const std::string hint = "Nintendall will close, update and start again. Esc cancels.";
    const int bw = std::min(W - int(40 * s), std::max(int(460 * s), text_.width(hint) + int(40 * s)));
    const int lh = text_.lineHeight();
    const int bh = int(40 * s) + 4 * lh + int(36 * s);
    const int bx = (W - bw) / 2, by = (H - bh) / 2;
    video_.fillRect(bx, by, bw, bh, {44, 44, 50});
    int y = by + int(20 * s);
    const std::string title = "Downloading Nintendall " + download_->info.version;
    text_.draw(bx + int(20 * s), y, title, kWhite, Text::Large);
    y += text_.lineHeight(Text::Large) + int(12 * s);
    // Progress bar
    const uint64_t got = download_->got, total = download_->total;
    const int barW = bw - int(40 * s), barH = int(12 * s);
    video_.fillRect(bx + int(20 * s), y, barW, barH, {70, 70, 80});
    if (total > 0) video_.fillRect(bx + int(20 * s), y, int(barW * std::min(1.0, double(got) / double(total))), barH, {230, 0, 18});
    y += barH + int(10 * s);
    char line[96];
    if (total > 0) std::snprintf(line, sizeof line, "%.1f of %.1f MB", got / 1048576.0, total / 1048576.0);
    else std::snprintf(line, sizeof line, "%.1f MB", got / 1048576.0);
    text_.draw(bx + int(20 * s), y, line, {200, 200, 208});
    y += lh + int(4 * s);
    text_.draw(bx + int(20 * s), y, hint, {150, 150, 160});
}

// ============================================================================
// Pairing real Wii Remotes over Bluetooth

struct App::PairJob {
    std::atomic<bool> done{false};
    WiimotePairResult result;
};

void App::startWiimotePairing() {
    if (pairJob_) return;
    auto job = std::make_shared<PairJob>();
    pairJob_ = job;
    osd("Searching for Wii Remotes - press the red sync button on each remote now", 30000);
    // The thread keeps its own reference, so quitting mid-search is safe.
    std::thread([job] {
        job->result = pairWiiRemotes();
        job->done = true;
    }).detach();
}

void App::checkWiimotePairing() {
    if (!pairJob_ || !pairJob_->done) return;
    const WiimotePairResult r = pairJob_->result;
    pairJob_.reset();
    if (!r.error.empty()) {
        osd(r.error, 6000);
    } else if (r.paired == 0) {
        osd("No Wii Remote found - press its red sync button, then choose Pair Wii Remotes again", 6000);
    } else {
        const std::string n = r.paired == 1 ? "1 Wii Remote" : std::to_string(r.paired) + " Wii Remotes";
        osd("Paired " + n + (realWiimotes() ? "" : " - for Wii games, set Console Settings > Wii Controller to Real Wii Remote"),
            8000);
    }
}

// ============================================================================
// LAN games (GameCube / Wii through Dolphin's netplay)

std::vector<LanDialog::Game> App::lanGames() {
    std::vector<LanDialog::Game> games;
    for (const HomeConsole& c : homeConsoles()) {
        if (!usesDolphinLan(c.system) && !usesNetplay(c.system) && !usesLinkLan(c.system)) continue;
        for (const LibraryGame& g : lanLibrary_.scan(home_.gamesRoot() / fs::u8path(c.folder), c.system))
            if (usesDolphinLan(g.system) || usesNetplay(g.system) || usesLinkLan(g.system))
                games.push_back({g.path, g.title, g.gameId, g.system});
    }
    std::stable_sort(games.begin(), games.end(), [](const LanDialog::Game& a, const LanDialog::Game& b) {
        std::string x = a.title, y = b.title;
        for (auto& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (auto& ch : y) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        return x < y;
    });
    return games;
}

void App::openLanDialog(LanDialog::Kind kind) {
    menu_.close();
    std::vector<LanDialog::Game> games = lanGames();
    // The GameCube / Wii game that's running can be picked even if it isn't in the Games folder.
    const bool gcWii = core_ && (usesDolphinLan(system_) || usesNetplay(system_) || usesLinkLan(system_));
    if (gcWii && std::none_of(games.begin(), games.end(), [&](const LanDialog::Game& g) { return g.path == gamePath_; }))
        games.insert(games.begin(), {gamePath_, gameTitle_, "", system_});
    const std::string want = gcWii ? gamePath_.u8string() : cfg_.get("lan.game");
    int selected = 0;
    for (size_t i = 0; i < games.size(); ++i)
        if (games[i].path.u8string() == want) selected = int(i);
    std::string name = cfg_.get("lan.name");
    if (name.empty()) name = defaultPlayerName();
    const int delay = std::atoi(cfg_.get("lan.delay").c_str());
    lanDialog_.open(kind, std::move(games), selected, name, cfg_.get("lan.target"), delay > 0 ? delay : 4);
}

bool App::startLanGame(const LanDialog::Request& request) {
    const LanDialog::Request req = request;  // startGame resets lan_ (which may hold `request`)
    if (req.kind == LanDialog::Kind::Join && (usesNetplay(req.game.system) || usesLinkLan(req.game.system))) {
        // NES / SNES / N64: connect first; the host says which game (and its settings), then it loads.
        // Game Boy / GBA: connect first too, then our own game loads.
        stopGame();
        auto net = std::make_unique<Netplay>();
        std::string error;
        if (!net->join(req.target, req.name, error)) {
            showMessage("LAN Game", error, true);
            return false;
        }
        lan_ = Lan{};
        lan_.active = true;
        lan_.req = req;
        lan_.net = std::move(net);
        cfg_.set("lan.name", req.name);
        cfg_.set("lan.game", req.game.path.u8string());
        cfg_.set("lan.target", req.target);
        saveConfig();
        return true;
    }
    lanPending_ = &req;
    lanLinkLoad_ = usesLinkLan(req.game.system);
    if (lanLinkLoad_ && linkFamily(req.game.system) == System::GB)  // Gambatte waits for the other Game Boy
        lanCoreOptions_ = {{"gambatte_gb_link_mode", "Network Server"}, {"gambatte_gb_link_network_port", kGbLinkPort}};
    const bool ok = startGame(req.game.path);
    lanPending_ = nullptr;
    lanLinkLoad_ = false;
    lanCoreOptions_.clear();
    cfg_.set("lan.name", req.name);
    cfg_.set("lan.game", req.game.path.u8string());
    if (req.kind == LanDialog::Kind::Host) cfg_.set("lan.delay", std::to_string(req.delay));
    else cfg_.set("lan.target", req.target);
    saveConfig();
    return ok;
}

bool App::lanLobbyShown() const {
    return lan_.active && (core_ || lan_.net) &&
           (lan_.status.state != LanState::Playing || !lan_.errorText.empty());
}

LanLobby::Info App::lanLobbyInfo() const {
    LanLobby::Info info;
    info.host = lan_.req.kind == LanDialog::Kind::Host;
    info.gameTitle = lan_.req.game.title.empty() ? gameTitle_ : lan_.req.game.title;
    if (lan_.net && !lan_.net->isHost() && !lan_.net->game().title.empty()) info.gameTitle = lan_.net->game().title;
    info.target = lan_.req.target;
    info.addresses = lan_.addresses;
    info.errorText = lan_.errorText;
    return info;
}

void App::leaveLan(const std::string& message) {
    stopGame();  // unloading the emulator tells the other players
    lan_ = Lan{};  // (joining NES / SNES / N64: before the game was loaded)
    if (!message.empty()) showMessage("LAN Game", message, false);
}

bool App::lanBlocks(const char* what) {
    if (!lan_.active) return false;
    osd(std::string(what) + " is off during LAN games");
    return true;
}

void App::updateLan() {
    if (!lan_.active) return;
    if (lan_.net) {
        updateNetplay();
        return;
    }
    if (!core_) {
        lan_ = Lan{};
        return;
    }
    lan_.status = readLanStatus(lan_.api);
    for (const std::string& m : lan_.status.messages) osd(m, 4000);
    const LanStatus& st = lan_.status;

    if (st.state == LanState::Ended) {
        leaveLan(st.error.empty() ? "The LAN game ended." : st.error);
        return;
    }
    // Joining: the host left the lobby (Dolphin itself would only notice after 30 seconds).
    const bool waiting = st.state == LanState::Lobby || st.state == LanState::Starting;
    if (lan_.req.kind == LanDialog::Kind::Join && waiting) {
        if (!st.hostName.empty()) lan_.sawHost = true;
        else if (lan_.sawHost) {
            leaveLan("The host closed the LAN game.");
            return;
        }
    }
    if (st.state != LanState::Error || !lan_.errorText.empty()) return;

    // Couldn't join: say why. If the host picked another game that's in our Games folder, join with it.
    const bool joining = lan_.req.kind == LanDialog::Kind::Join;
    if (joining && st.errorKind == "wronggame" && !lan_.switchedGame && !st.hostGameId.empty()) {
        for (const LanDialog::Game& g : lanGames()) {
            if (g.gameId != st.hostGameId || g.path == lan_.req.game.path) continue;
            LanDialog::Request req = lan_.req;
            req.game = g;
            osd("The host is playing " + g.title + " - joining with it", 5000);
            if (startLanGame(req)) lan_.switchedGame = true;
            return;
        }
    }
    if (joining && st.errorKind == "wronggame") {
        const std::string game = st.hostGame.empty() ? "another game" : st.hostGame;
        lan_.errorText = "The host is playing " + game + (st.hostGameId.empty() ? "" : " (" + st.hostGameId + ")") +
                         ". Put that game in your GameCube or Wii games folder, then join again.";
    } else if (st.errorKind == "gamecopy") {
        lan_.errorText = st.error + " Everyone needs the same copy of the game (same region and version).";
    } else {
        lan_.errorText = st.error.empty() ? "The LAN game couldn't start." : st.error;
    }
}

// ============================================================================
// LAN games for NES / SNES / N64 (Nintendall's netplay, see netplay.hpp)

void App::lanStart() {
    if (!lan_.active || lan_.req.kind != LanDialog::Kind::Host) return;
    if (!lan_.net) {
        lan_.api.start();  // GameCube / Wii
        return;
    }
    if (!core_ || lan_.net->phase() != Netplay::Phase::Lobby) return;
    if (lan_.net->game().link) {  // everyone plays their own game: nothing to send
        lan_.net->startLink();
        return;
    }
    const std::vector<uint8_t> state = core_->saveState();
    if (state.empty()) {
        osd("Couldn't start - try again in a moment");
        return;
    }
    lan_.net->start(state);
}

NetInput App::localNetInput() {
    NetInput in;
    in.buttons = uint16_t(localInputState(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK));
    for (unsigned i = 0; i < 4; ++i)
        in.axes[i] = localInputState(0, RETRO_DEVICE_ANALOG, i / 2, i % 2);
    return in;
}

void App::updateNetplay() {
    Netplay& net = *lan_.net;
    if (net.phase() != Netplay::Phase::Playing) net.poll();  // (while playing, the frames poll)
    for (const std::string& m : net.takeMessages()) osd(m, 4000);

    // What the lobby shows.
    LanStatus st;
    switch (net.phase()) {
    case Netplay::Phase::Connecting: st.state = LanState::Connecting; break;
    case Netplay::Phase::Lobby: st.state = LanState::Lobby; break;
    case Netplay::Phase::Syncing: st.state = LanState::Starting; break;
    case Netplay::Phase::Playing: st.state = LanState::Playing; break;
    case Netplay::Phase::Ended: st.state = LanState::Ended; break;
    case Netplay::Phase::Error: st.state = LanState::Error; break;
    }
    st.codeState = "none";  // no host codes: friends type the host's address
    st.delay = unsigned(net.game().delay);
    const auto& names = net.players();
    for (size_t p = 0; p < names.size(); ++p) {
        if (names[p].empty()) continue;
        if (p == 0) st.hostName = names[p];
        else st.players.push_back(names[p]);
    }
    if (net.phase() == Netplay::Phase::Syncing)
        st.progress = net.isHost() ? "Sending the game to the players..." : "Getting the game from the host...";
    st.error = net.error();
    lan_.status = st;

    switch (net.phase()) {
    case Netplay::Phase::Ended:
        if (net.game().link && core_ && (net.isHost() || lan_.netLoaded)) {
            // Link-cable game: the others are gone, but this game is our own: keep playing.
            const std::string why = net.error().empty() ? "The LAN game ended." : net.error();
            if (lan_.linkStarted && core_->netpacket() && core_->netpacket()->stop) core_->netpacket()->stop();
            lan_ = Lan{};
            osd(why + " Your game keeps going.", 6000);
            return;
        }
        leaveLan(net.error().empty() ? "The LAN game ended." : net.error());
        return;
    case Netplay::Phase::Error:
        if (lan_.errorText.empty()) lan_.errorText = net.error().empty() ? "The LAN game couldn't start." : net.error();
        return;
    case Netplay::Phase::Lobby: {
        if (net.isHost() || lan_.netLoaded || !lan_.errorText.empty()) return;
        const Netplay::GameInfo& g = net.game();
        if (g.link) {
            joinLinkGame();
            return;
        }
        // Joined: load the host's game (the same file, found by its checksum) with the host's settings.
        const System family = lanFamily(System(g.system));
        auto crcOf = [&](const fs::path& path) {
            std::error_code ec;
            const uintmax_t size = fs::file_size(path, ec);
            auto& cached = lanCrcs_[path.u8string()];
            if (cached.first != size || cached.second == 0) cached = {size, netplayGameCrc(path.u8string())};
            return cached.second;
        };
        fs::path found;
        if (lanFamily(lan_.req.game.system) == family && crcOf(lan_.req.game.path) == g.crc) found = lan_.req.game.path;
        if (found.empty())
            for (const LanDialog::Game& game : lanGames())
                if (lanFamily(game.system) == family && crcOf(game.path) == g.crc) {
                    found = game.path;
                    break;
                }
        if (found.empty()) {
            lan_.errorText = "The host is playing " + g.title + ". Put the same file of that game in your " +
                             systemInfo(family).fullName + " games folder, then join again.";
            return;
        }
        Lan keep = std::move(lan_);
        lan_ = Lan{};
        lanCoreOptions_ = g.options;
        const bool ok = startGame(found);
        lanCoreOptions_.clear();
        if (!ok) {
            keep.net->leave();
            return;  // (startGame said why)
        }
        lan_ = std::move(keep);
        lan_.netLoaded = true;
        lan_.req.game.path = found;
        lan_.req.game.title = g.title;
        connectPorts();  // just port 1 until the game starts, like the host
        if (video_.window()) SDL_SetWindowTitle(video_.window(), ("Nintendall - " + g.title).c_str());
        return;
    }
    case Netplay::Phase::Syncing: {
        std::vector<uint8_t> state;
        if (!net.isHost() && core_ && net.takeState(state)) {
            if (core_->loadState(state)) net.stateLoaded();
            else lan_.errorText = "Couldn't load the game from the host.";
        }
        return;
    }
    case Netplay::Phase::Playing:
        if (!lan_.portsSet && core_ && !net.game().link) {
            lan_.portsSet = true;
            connectPorts();  // every player's port, the same on every computer, from frame 0
        }
        return;
    default:
        return;
    }
}

// ============================================================================
// Link-cable LAN games (Game Boy / Color / Advance): everyone plays their own game

void App::joinLinkGame() {
    Netplay& net = *lan_.net;
    const Netplay::GameInfo& g = net.game();
    const System family = linkFamily(System(g.system));
    if (linkFamily(lan_.req.game.system) != family) {
        lan_.errorText = "The host is playing " + g.title + " with the link cable. Pick one of your " +
                         linkFamilyName(family) + " games, then join again.";
        return;
    }
    std::vector<std::pair<std::string, std::string>> options;
    if (family == System::GB) {
        // Gambatte connects to the host's Game Boy itself; it takes the address digit by digit.
        unsigned a = 0, b = 0, c = 0, d = 0;
        if (std::sscanf(net.hostIp().c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
            lan_.errorText = "Couldn't use the host's address for the link cable.";
            return;
        }
        char digits[16];
        std::snprintf(digits, sizeof digits, "%03u%03u%03u%03u", a, b, c, d);
        options = {{"gambatte_gb_link_mode", "Network Client"}, {"gambatte_gb_link_network_port", kGbLinkPort}};
        for (int i = 0; i < 12; ++i)
            options.push_back({"gambatte_gb_link_network_server_ip_" + std::to_string(i + 1), std::string(1, digits[i])});
    }
    const fs::path game = lan_.req.game.path;
    Lan keep = std::move(lan_);
    lan_ = Lan{};
    lanCoreOptions_ = options;
    lanLinkLoad_ = true;
    const bool ok = startGame(game);
    lanLinkLoad_ = false;
    lanCoreOptions_.clear();
    if (!ok) {
        keep.net->leave();
        return;  // (startGame said why)
    }
    lan_ = std::move(keep);
    lan_.netLoaded = true;
    lan_.net->linkLoaded();
    connectPorts();
}

// The emulator's packet functions (one game runs at a time).
static App* g_linkApp = nullptr;

static void RETRO_CALLCONV linkSendCb(int /*flags*/, const void* buf, size_t len, uint16_t to) {
    if (g_linkApp && buf && len) g_linkApp->linkSend(to, buf, len);  // always reliable, sent at once
}

static void RETRO_CALLCONV linkPollCb() {
    if (g_linkApp) g_linkApp->linkPump();
}

void App::linkSend(unsigned to, const void* data, size_t size) {
    if (lan_.net) lan_.net->linkSend(to == RETRO_NETPACKET_BROADCAST ? Netplay::kLinkEveryone : int(to), data, size);
}

void App::linkPump() {
    if (!lan_.net || !core_) return;
    Netplay& net = *lan_.net;
    net.poll();
    const retro_netpacket_callback* np = lan_.linkStarted ? core_->netpacket() : nullptr;
    if (!np) return;
    for (int p : net.takeLinkJoined()) {
        if (np->connected && !np->connected(uint16_t(p))) {
            const auto& names = net.players();
            osd((size_t(p) < names.size() ? names[size_t(p)] : "A player") + " can't link with this game (too many players)",
                5000);
        }
    }
    for (int p : net.takeLinkLeft())
        if (np->disconnected) np->disconnected(uint16_t(p));
    for (const Netplay::LinkPacket& pk : net.takeLinkPackets())
        np->receive(pk.data.data(), pk.data.size(), uint16_t(pk.from));
}

void App::linkFrame() {
    const retro_netpacket_callback* np = core_->netpacket();
    if (np && !lan_.linkStarted) {
        lan_.linkStarted = true;
        g_linkApp = this;
        np->start(uint16_t(lan_.net->localPlayer()), linkSendCb, linkPollCb);
    }
    linkPump();
    if (np && np->poll) np->poll();
    core_->run();
}
