#include "app.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

#include "filedialog.hpp"

namespace fs = std::filesystem;

namespace {

const Color kBlack{0, 0, 0};
const Color kHint{140, 140, 140};
const Color kHintDim{90, 90, 90};
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

}  // namespace

// ============================================================================
// Main loop

int App::run(const fs::path& start, const LaunchOptions& opt) {
    opt_ = opt;
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
    }
    input_.setForcedButtons(opt.holdButtons);

    // Headless: load the game, run N frames as fast as possible, report, exit.
    if (opt.headless) {
        if (!startGame(start)) return 1;
        const double t0 = nowSeconds();
        for (int i = 0; i < opt.frames && core_; ++i) core_->run();
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
        if (buttonConfig_.takeChanged()) saveConfig();

        const bool modal = menu_.isOpen() || buttonConfig_.isOpen();
        const bool running = core_ && !paused_ && !focusLost_ && !modal;
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
    fastForward_ = fastForwardToggled_ || keys[SDL_SCANCODE_TAB];
    const int runs = fastForward_ ? 4 : 1;
    for (int i = 0; i < runs && core_; ++i) core_->run();
    if (core_ && core_->shutdownRequested()) {
        stopGame();
        return;
    }
    saveSram(false);
}

void App::saveConfig() {
    input_.save(cfg_);
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
    const fs::path corePath = paths_.findCore(sys.core);
    if (corePath.empty()) {
        showMessage("Nintendall is missing a file",
                    std::string("The ") + sys.fullName + " emulator (" + coreFileName(sys.core) +
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

    // 4. Start the emulator with the game.
    core_ = std::make_unique<LibretroCore>(*this, paths_.system, paths_.saves,
                                           paths_.config / (std::string(sys.core) + ".opt"));
    std::string err;
    if (!core_->load(corePath, err) || !core_->loadGame(gameFile, err)) {
        std::fprintf(stderr, "Core failed: %s\n", err.c_str());
        core_.reset();
        if (!extracted_.empty()) fs::remove(extracted_, ec);
        extracted_.clear();
        showMessage("Couldn't start the game",
                    path.filename().u8string() + "\n\nThe file may be damaged, or be in a format that isn't supported." +
                        (sys.note ? std::string("\n\nNote: ") + sys.note : ""),
                    true);
        return false;
    }

    // 5. Running.
    system_ = det.system;
    gamePath_ = path;
    gameTitle_ = path.stem().u8string();
    const auto& av = core_->avInfo();
    onAvInfoChanged();
    audio_.clear();
    audio_.setInputRate(av.timing.sample_rate);
    input_.setStickAsDpad(usesStickAsDpad(system_));
    video_.clearFrame();
    loadSram();
    paused_ = fastForward_ = fastForwardToggled_ = false;
    nextFrame_ = nowSeconds();
    addRecent(path);
    if (video_.window()) SDL_SetWindowTitle(video_.window(), ("Nintendall - " + gameTitle_).c_str());
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
    paused_ = false;
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
    video_.uploadFrame(data, w, h, pitch, fmt);
}

void App::onAudio(const int16_t* stereo, size_t frames) {
    if (opt_.headless) {
        audioFramesSeen_ += frames;
        for (size_t i = 0; i < frames * 2; ++i) audioPeak_ = std::max(audioPeak_, std::abs(int(stereo[i])));
        return;
    }
    if (fastForward_) return;  // no chipmunk audio while fast-forwarding
    audio_.push(stereo, frames);
}

int16_t App::onInputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    return input_.state(port, device, index, id);
}

bool App::onRumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    return input_.rumble(port, effect, strength);
}

void App::onMessage(const std::string& msg, unsigned durationMs) { osd(msg, std::max(1500u, durationMs)); }

void App::onAvInfoChanged() {
    if (!core_) return;
    const auto& av = core_->avInfo();
    double aspect = av.geometry.aspect_ratio;
    if (aspect <= 0 && av.geometry.base_height > 0) aspect = double(av.geometry.base_width) / av.geometry.base_height;
    video_.setAspect(aspect);
    audio_.setInputRate(av.timing.sample_rate);
}

// ============================================================================
// Events

void App::handleEvent(const SDL_Event& e) {
    std::string msg;
    if (input_.handleDeviceEvent(e, &msg)) {
        if (!msg.empty()) osd(msg);
        return;
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

    switch (e.type) {
    case SDL_QUIT:
        quit_ = true;
        return;
    case SDL_WINDOWEVENT:
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
        startGame(p);
        return;
    }
    default:
        break;
    }

    // Button Config window is modal: it gets everything.
    if (buttonConfig_.isOpen()) {
        buttonConfig_.handleEvent(e, mouseX_, mouseY_, input_);
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
    if (a > ActWindowSize && a <= ActWindowSize + 4) {
        setWindowScale(a - ActWindowSize);
        return;
    }
    switch (a) {
    case ActOpen: openFileDialog(); break;
    case ActCloseGame: stopGame(); break;
    case ActExit: quit_ = true; break;
    case ActPause:
        if (core_) paused_ = !paused_;
        break;
    case ActReset:
        if (core_) {
            core_->reset();
            paused_ = false;
            osd("Reset");
        }
        break;
    case ActFastForward: fastForwardToggled_ = !fastForwardToggled_; break;
    case ActSaveState: saveState(); break;
    case ActLoadState: loadState(); break;
    case ActButtonConfig: buttonConfig_.open(); break;
    case ActFullscreen: video_.toggleFullscreen(); break;
    case ActControls:
        showMessage("Controls",
                    "Keyboard (change these in Options > Button Config):\n"
                    "  Arrow keys = D-pad\n  X = A,  Z = B,  S = X,  A = Y\n  Q / W = L / R,  1 / 3 = L2 / R2\n"
                    "  Enter = Start,  Right Shift = Select\n\n"
                    "Shortcuts:\n  Ctrl+O = Open game\n  Ctrl+P = Pause,  Ctrl+R = Reset\n"
                    "  F5 = Save state,  F9 = Load state,  F6 / F7 = Change slot\n"
                    "  Hold Tab = Fast forward\n  F11 = Fullscreen,  Esc = Leave fullscreen\n  F12 = Screenshot\n\n"
                    "Controllers work automatically. Home button, or Select + Start, pauses.",
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
    // Always shown in a window; in fullscreen it appears when the mouse goes to the top.
    if (!video_.isFullscreen()) return true;
    return menu_.isOpen() || (SDL_GetTicks() - lastMouseMove_ < 3000 && mouseY_ < menu_.height(text_) * 2);
}

SDL_Rect App::gameArea() const {
    const int bar = video_.isFullscreen() ? 0 : menu_.height(text_);
    return {0, bar, video_.width(), std::max(0, video_.height() - bar)};
}

void App::setWindowScale(int scale) {
    if (!video_.window()) return;
    if (video_.isFullscreen()) video_.toggleFullscreen();
    int baseH = kDefaultBaseH;
    double aspect = kDefaultAspect;
    if (core_) {
        const auto& g = core_->avInfo().geometry;
        if (g.base_height > 0) baseH = int(g.base_height);
        aspect = g.aspect_ratio > 0 ? g.aspect_ratio : double(g.base_width) / std::max(1u, g.base_height);
    }
    const float pr = video_.pixelRatio();
    const int h = baseH * scale + menu_.height(text_);
    const int w = int(std::lround(baseH * scale * aspect));
    SDL_SetWindowSize(video_.window(), int(w / pr), int(h / pr));
    video_.onResize();
}

void App::buildMenus() {
    const bool game = core_ != nullptr;
    std::vector<Menu> menus;

    Menu file{"File", {}};
    file.items.push_back({"Open...", "Ctrl+O", ActOpen});
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
    state.items.push_back({"Save State", "F5", ActSaveState, game});
    state.items.push_back({"Load State", "F9", ActLoadState, game});
    state.items.push_back({});
    for (int i = 1; i <= kSlots; ++i) {
        std::string label = "Slot " + std::to_string(i);
        std::error_code ec;
        if (game && menu_.isOpen() && !fs::exists(statePath(i), ec)) label += "  (empty)";
        state.items.push_back({label, "", ActSlot + i, true, slot_ == i});
    }
    menus.push_back(state);

    Menu options{"Options", {}};
    options.items.push_back({"Button Config...", "", ActButtonConfig});
    options.items.push_back({});
    options.items.push_back({"Fullscreen", "F11", ActFullscreen, true, video_.isFullscreen()});
    options.items.push_back({});
    for (int k = 1; k <= 4; ++k)
        options.items.push_back({"Window Size " + std::to_string(k) + "x", "", ActWindowSize + k});
    menus.push_back(options);

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
    } else if (!core_) {
        // Empty window: just a hint to drop a game in.
        const std::string title = "Drag and drop a game here";
        const std::string sub = "or choose File > Open";
        const int th = text_.lineHeight(Text::Large), sh = text_.lineHeight();
        const int y = area.y + (area.h - th - sh - int(6 * s)) / 2;
        text_.draw(area.x + (area.w - text_.width(title, Text::Large)) / 2, y, title, kHint, Text::Large);
        text_.draw(area.x + (area.w - text_.width(sub)) / 2, y + th + int(6 * s), sub, kHintDim);
    }

    if (core_ && paused_) {
        const std::string t = "Paused";
        const int tw = text_.width(t, Text::Large), th = text_.lineHeight(Text::Large);
        const int bx = area.x + (area.w - tw) / 2 - int(16 * s), by = area.y + (area.h - th) / 2 - int(8 * s);
        video_.fillRect(bx, by, tw + int(32 * s), th + int(16 * s), {0, 0, 0, 170});
        text_.draw(bx + int(16 * s), by + int(8 * s), t, kWhite, Text::Large);
    }
    if (fastForward_ && core_ && !paused_) text_.draw(area.x + int(10 * s), area.y + int(8 * s), "Fast Forward", kWhite);

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
    video_.present();
}
