#include "app.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>

namespace fs = std::filesystem;

namespace {

const Color kBg{16, 16, 26};
const Color kPanel{28, 28, 44};
const Color kAccent{232, 48, 64};
const Color kText{236, 236, 246};
const Color kDim{130, 130, 156};
const Color kGood{96, 220, 128};
const Color kSelect{58, 58, 104};
const Color kWarn{250, 190, 80};

double nowSeconds() { return double(SDL_GetPerformanceCounter()) / double(SDL_GetPerformanceFrequency()); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}

// Fit a string into maxChars, keeping the end visible if keepEnd (for paths).
std::string fit(const std::string& s, int maxChars, bool keepEnd = false) {
    if (maxChars <= 3) return "";
    if (int(s.size()) <= maxChars) return s;
    if (keepEnd) return "..." + s.substr(s.size() - size_t(maxChars - 3));
    return s.substr(0, size_t(maxChars - 3)) + "...";
}

std::vector<std::string> wrap(const std::string& text, int maxChars) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find('\n', start);
        std::string para = text.substr(start, nl == std::string::npos ? std::string::npos : nl - start);
        while (int(para.size()) > maxChars) {
            size_t cut = para.rfind(' ', size_t(maxChars));
            if (cut == std::string::npos || cut == 0) cut = size_t(maxChars);
            lines.push_back(para.substr(0, cut));
            para = para.substr(cut + (para[cut] == ' ' ? 1 : 0));
        }
        lines.push_back(para);
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return lines;
}

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

}  // namespace

// ============================================================================
// Main loop

int App::run(const fs::path& start, const LaunchOptions& opt) {
    opt_ = opt;
    paths_.init();
    cfg_.load(paths_.data / "nintendall.cfg");

    std::string err;
    if (!video_.init(opt.headless, opt.fullscreen, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (!opt.headless) {
        audio_.init();
        fs::path db = paths_.base / "gamecontrollerdb.txt";
        if (!fs::exists(db)) db = paths_.data / "gamecontrollerdb.txt";
        input_.init(fs::exists(db) ? db.u8string() : "");
    }
    input_.setForcedButtons(opt.holdButtons);
    refreshCores();

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

    // Normal start: a game path launches straight into the game; a folder opens the library there.
    std::error_code ec;
    fs::path libDir = fs::u8path(cfg_.get("last_dir"));
    if (libDir.empty() || !fs::is_directory(libDir, ec)) {
        const char* home = SDL_getenv("HOME");
        if (!home) home = SDL_getenv("USERPROFILE");
        libDir = home ? fs::u8path(home) : fs::current_path();
    }
    if (!start.empty() && fs::is_directory(start, ec) && detectGame(start).system == System::Unknown) {
        libDir = start;
    }
    openDir(libDir);
    if (!start.empty() && !(fs::is_directory(start, ec) && detectGame(start).system == System::Unknown)) {
        startGame(start);
    }

    nextFrame_ = nowSeconds();
    while (!quit_) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) handleEvent(e);
        updateHeldNav();

        if (screen_ == Screen::Game) {
            gameFrame();
            continue;
        }
        video_.clear(kBg);
        switch (screen_) {
        case Screen::Library: drawLibrary(); break;
        case Screen::Pause: drawPause(); break;
        case Screen::Error: drawError(); break;
        default: break;
        }
        drawOsd();
        video_.present();
        pace(1.0 / 60.0);
    }

    stopGame();
    cfg_.set("last_dir", cwd_.u8string());
    cfg_.save();
    input_.shutdown();
    audio_.shutdown();
    video_.shutdown();
    return 0;
}

void App::pace(double frameSeconds) {
    // Timer-based pacing. With vsync on, the present() call usually does the waiting already.
    double now = nowSeconds();
    if (now - nextFrame_ > 0.1 || nextFrame_ - now > 0.5) nextFrame_ = now;  // fell behind / clock jump
    nextFrame_ += frameSeconds;
    double wait = nextFrame_ - now;
    if (wait > 0.002) SDL_Delay(Uint32((wait - 0.0015) * 1000));
    while (nowSeconds() < nextFrame_) {
    }
}

int App::uiScale() const {
    const int h = video_.height();
    return h >= 1300 ? 4 : h >= 900 ? 3 : h >= 480 ? 2 : 1;
}

// ============================================================================
// Starting and stopping games

bool App::startGame(const fs::path& path) {
    stopGame();
    std::error_code ec;
    if (!fs::exists(path, ec)) {
        showError("File not found", path.u8string());
        return false;
    }

    // 1. Work out which system this game is for.
    const Detection det = detectGame(path);
    if (det.system == System::Unknown) {
        showError("Couldn't recognize this game",
                  path.filename().u8string() + "\n\n" + det.detail +
                      "\n\nNintendall supports NES, SNES, N64, Game Boy (Color/Advance), DS, 3DS, GameCube, Wii "
                      "and Wii U games.");
        return false;
    }
    const SystemInfo& sys = systemInfo(det.system);
    std::fprintf(stderr, "Detected %s (%s)\n", sys.fullName, det.detail.c_str());

    // 2. Every system has one bundled emulator. GPU-drawn systems aren't ready yet.
    if (sys.needsGpu) {
        showError(std::string(sys.fullName) + " support is coming soon",
                  std::string("Detected: ") + det.detail + "\n\nThe " + sys.emulator +
                      " emulator for this system is included, but it draws with the GPU, and GPU rendering "
                      "in Nintendall is still being built.");
        return false;
    }
    const fs::path corePath = paths_.findCore(sys.core);
    if (corePath.empty()) {
        showError("Nintendall is missing a file",
                  std::string("The ") + sys.emulator + " emulator (" + coreFileName(sys.core) +
                      ") should be in the cores folder next to Nintendall. Reinstalling Nintendall should fix this.");
        return false;
    }

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
            showError("Couldn't unzip game", path.u8string());
            return false;
        }
        extracted_ = paths_.cache / fs::u8path(det.zipEntry).filename();
        std::ofstream out(extracted_, std::ios::binary);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) {
            showError("Couldn't unzip game", "Writing " + extracted_.u8string() + " failed");
            return false;
        }
        gameFile = extracted_;
    }

    // 4. Start the emulator with the game.
    core_ = std::make_unique<LibretroCore>(*this, paths_.system, paths_.saves,
                                           paths_.config / (std::string(sys.core) + ".opt"));
    std::string err;
    if (!core_->load(corePath, err) || !core_->loadGame(gameFile, err)) {
        core_.reset();
        if (!extracted_.empty()) fs::remove(extracted_, ec);
        extracted_.clear();
        showError("The game couldn't be started",
                  err + "\n\nThe file may be damaged, or be a format this emulator can't read." +
                      (sys.note ? std::string("\n\nNote: ") + sys.note : ""));
        return false;
    }

    // 5. Game is running.
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
    fastForward_ = fastForwardToggled_ = false;
    slot_ = 1;
    screen_ = Screen::Game;
    nextFrame_ = nowSeconds();
    if (video_.window())
        SDL_SetWindowTitle(video_.window(), ("Nintendall - " + gameTitle_ + "  [" + sys.name + " / " + core_->name() + "]").c_str());
    osd(std::string(sys.name) + " detected - running with " + core_->name() + "   (Esc / Guide = menu)", 4000);
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
    if (video_.window()) SDL_SetWindowTitle(video_.window(), "Nintendall");
    if (screen_ == Screen::Game || screen_ == Screen::Pause) screen_ = Screen::Library;
}

void App::gameFrame() {
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    fastForward_ = fastForwardToggled_ || keys[SDL_SCANCODE_TAB];
    const int runs = fastForward_ ? 4 : 1;
    for (int i = 0; i < runs && core_; ++i) core_->run();
    if (!core_) return;
    if (core_->shutdownRequested()) {
        stopGame();
        return;
    }
    saveSram(false);

    video_.clear({0, 0, 0});
    video_.drawGame();
    if (fastForward_) video_.drawText(12, 12, ">> FAST", uiScale(), kWarn);
    drawOsd();
    video_.present();
    pace(fastForward_ ? 1.0 / 120.0 : 1.0 / core_->avInfo().timing.fps);
}

// ---- battery saves ----------------------------------------------------------

static fs::path sramPath(const Paths& p, const fs::path& game) { return p.saves / fs::u8path(game.stem().u8string() + ".srm"); }

void App::loadSram() {
    lastSram_.clear();
    uint8_t* mem = core_ ? core_->saveRamData() : nullptr;
    const size_t size = core_ ? core_->saveRamSize() : 0;
    if (!mem || size == 0) return;
    std::ifstream f(sramPath(paths_, gamePath_), std::ios::binary);
    if (f) {
        f.read(reinterpret_cast<char*>(mem), std::streamsize(size));
        std::fprintf(stderr, "Loaded battery save (%zu bytes)\n", size_t(f.gcount()));
    }
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
        osd("This core can't save states");
        return;
    }
    std::ofstream f(statePath(slot_), std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    osd(f ? "Saved state to slot " + std::to_string(slot_) : std::string("Saving state failed"));
}

void App::loadState() {
    if (!core_) return;
    std::ifstream f(statePath(slot_), std::ios::binary);
    if (!f) {
        osd("Slot " + std::to_string(slot_) + " is empty");
        return;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    osd(core_->loadState(data) ? "Loaded state from slot " + std::to_string(slot_) : std::string("Loading state failed"));
}

void App::takeScreenshot() {
    char stamp[32];
    const std::time_t t = std::time(nullptr);
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
    const fs::path p = paths_.screenshots / fs::u8path(gameTitle_ + "-" + stamp + ".png");
    osd(video_.saveScreenshot(p) ? "Screenshot saved: " + p.filename().u8string() : std::string("Screenshot failed"));
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
// Events and navigation

void App::handleEvent(const SDL_Event& e) {
    std::string msg;
    if (input_.handleDeviceEvent(e, &msg)) {
        if (!msg.empty()) osd(msg);
        return;
    }
    switch (e.type) {
    case SDL_QUIT:
        quit_ = true;
        return;
    case SDL_WINDOWEVENT:
        if (e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) video_.onResize();
        return;
    case SDL_DROPFILE: {
        // Drag a game onto the window: detect it and play it, from any screen.
        const fs::path p = fs::u8path(e.drop.file);
        SDL_free(e.drop.file);
        startGame(p);
        return;
    }
    case SDL_KEYDOWN: {
        const SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
            video_.toggleFullscreen();
            return;
        }
        if (screen_ == Screen::Library && k == SDLK_F5) {  // rescan folder + installed cores
            refreshCores();
            openDir(cwd_);
            osd("Refreshed");
            return;
        }
        if (screen_ == Screen::Game) {
            if (e.key.repeat) return;
            switch (k) {
            case SDLK_ESCAPE: screen_ = Screen::Pause; pauseSel_ = 0; audio_.clear(); return;
            case SDLK_F5: saveState(); return;
            case SDLK_F9: loadState(); return;
            case SDLK_F6: slot_ = std::max(1, slot_ - 1); osd("State slot " + std::to_string(slot_)); return;
            case SDLK_F7: slot_ = std::min(9, slot_ + 1); osd("State slot " + std::to_string(slot_)); return;
            case SDLK_F12: takeScreenshot(); return;
            case SDLK_SPACE:
                if (e.key.keysym.mod & KMOD_CTRL) {
                    fastForwardToggled_ = !fastForwardToggled_;
                }
                return;
            default: return;
            }
        }
        dispatchNav(navFromKey(e.key));
        return;
    }
    case SDL_CONTROLLERBUTTONDOWN: {
        SDL_GameController* gc = SDL_GameControllerFromInstanceID(e.cbutton.which);
        const Uint8 b = e.cbutton.button;
        if (screen_ == Screen::Game) {
            // Guide button, or Select+Start together, opens the menu.
            const bool combo = gc && ((b == SDL_CONTROLLER_BUTTON_START && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_BACK)) ||
                                      (b == SDL_CONTROLLER_BUTTON_BACK && SDL_GameControllerGetButton(gc, SDL_CONTROLLER_BUTTON_START)));
            if (b == SDL_CONTROLLER_BUTTON_GUIDE || combo) {
                screen_ = Screen::Pause;
                pauseSel_ = 0;
                audio_.clear();
            }
            return;
        }
        const Nav n = navFromButton(b);
        if (n == Nav::Up || n == Nav::Down) {
            heldNav_ = n;
            heldSince_ = lastRepeat_ = SDL_GetTicks();
        }
        dispatchNav(n);
        return;
    }
    case SDL_CONTROLLERBUTTONUP:
        if (navFromButton(e.cbutton.button) == heldNav_) heldNav_ = Nav::None;
        return;
    case SDL_CONTROLLERAXISMOTION: {
        if (screen_ == Screen::Game || e.caxis.axis != SDL_CONTROLLER_AXIS_LEFTY) return;
        const Nav n = e.caxis.value < -20000 ? Nav::Up : e.caxis.value > 20000 ? Nav::Down : Nav::None;
        if (n != heldNav_) {
            heldNav_ = n;
            heldSince_ = lastRepeat_ = SDL_GetTicks();
            if (n != Nav::None) dispatchNav(n);
        }
        return;
    }
    default:
        return;
    }
}

void App::updateHeldNav() {
    if (heldNav_ == Nav::None || screen_ == Screen::Game) return;
    const Uint32 now = SDL_GetTicks();
    if (now - heldSince_ > 400 && now - lastRepeat_ > 70) {
        lastRepeat_ = now;
        dispatchNav(heldNav_);
    }
}

App::Nav App::navFromKey(const SDL_KeyboardEvent& k) const {
    switch (k.keysym.sym) {
    case SDLK_UP: return Nav::Up;
    case SDLK_DOWN: return Nav::Down;
    case SDLK_LEFT: return Nav::Left;
    case SDLK_RIGHT: return Nav::Right;
    case SDLK_PAGEUP: return Nav::PageUp;
    case SDLK_PAGEDOWN: return Nav::PageDown;
    case SDLK_HOME: return Nav::Home;
    case SDLK_END: return Nav::End;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
    case SDLK_x: return Nav::Accept;
    case SDLK_BACKSPACE:
    case SDLK_z: return Nav::Back;
    case SDLK_ESCAPE: return Nav::Menu;
    default: return Nav::None;
    }
}

App::Nav App::navFromButton(Uint8 b) const {
    switch (b) {
    case SDL_CONTROLLER_BUTTON_DPAD_UP: return Nav::Up;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return Nav::Down;
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return Nav::Left;
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return Nav::Right;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return Nav::PageUp;
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return Nav::PageDown;
    case SDL_CONTROLLER_BUTTON_A: return Nav::Accept;   // bottom button
    case SDL_CONTROLLER_BUTTON_B: return Nav::Back;     // right button
    case SDL_CONTROLLER_BUTTON_START:
    case SDL_CONTROLLER_BUTTON_GUIDE: return Nav::Menu;
    default: return Nav::None;
    }
}

void App::dispatchNav(Nav n) {
    if (n == Nav::None) return;
    switch (screen_) {
    case Screen::Library:
        if (n == Nav::Menu) {  // Esc twice on the library quits
            if (!SDL_TICKS_PASSED(SDL_GetTicks(), quitArmedUntil_)) quit_ = true;
            quitArmedUntil_ = SDL_GetTicks() + 2000;
            osd("Press Esc / Start again to quit", 2000);
            return;
        }
        libraryNav(n);
        return;
    case Screen::Pause: pauseNav(n); return;
    case Screen::Error:
        if (n == Nav::Accept || n == Nav::Back || n == Nav::Menu) screen_ = errorReturn_;
        return;
    default: return;
    }
}

// ============================================================================
// Library (file browser)

bool App::systemReady(System s) const {
    const SystemInfo& si = systemInfo(s);
    return !si.needsGpu && !paths_.findCore(si.core).empty();
}

void App::refreshCores() {
    systemReady_.clear();
    for (System s : allSystems()) systemReady_.push_back(systemReady(s));
}

void App::openDir(const fs::path& dirIn, const fs::path& select) {
    std::error_code ec;
    fs::path dir = fs::weakly_canonical(dirIn, ec);
    if (ec || !fs::is_directory(dir, ec)) dir = dirIn;
    cwd_ = dir;
    entries_.clear();
    sel_ = scroll_ = 0;

    if (dir.has_parent_path() && dir.parent_path() != dir) {
        Entry up;
        up.path = dir.parent_path();
        up.name = "..";
        up.isDir = up.isParent = true;
        up.detected = true;
        entries_.push_back(up);
    }

    std::vector<Entry> dirs, files;
    for (fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end;
         it.increment(ec)) {
        const fs::path p = it->path();
        const std::string name = p.filename().u8string();
        if (name.empty() || name[0] == '.') continue;
        Entry e;
        e.path = p;
        e.name = name;
        std::error_code ec2;
        if (it->is_directory(ec2)) {
            e.isDir = true;
            dirs.push_back(e);
        } else if (isKnownGameExtension(lowerExt(p))) {
            files.push_back(e);
        }
    }
    auto byName = [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); };
    std::sort(dirs.begin(), dirs.end(), byName);
    std::sort(files.begin(), files.end(), byName);
    entries_.insert(entries_.end(), dirs.begin(), dirs.end());
    entries_.insert(entries_.end(), files.begin(), files.end());

    if (!select.empty())
        for (size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].path == select) sel_ = int(i);
}

void App::detectEntry(Entry& e) {
    if (e.detected) return;
    e.detected = true;
    e.det = detectGame(e.path);
}

void App::libraryNav(Nav n) {
    const int count = int(entries_.size());
    const int page = 10;
    switch (n) {
    case Nav::Up: sel_ = sel_ > 0 ? sel_ - 1 : count - 1; break;
    case Nav::Down: sel_ = sel_ < count - 1 ? sel_ + 1 : 0; break;
    case Nav::PageUp: sel_ = std::max(0, sel_ - page); break;
    case Nav::PageDown: sel_ = std::min(count - 1, sel_ + page); break;
    case Nav::Home: sel_ = 0; break;
    case Nav::End: sel_ = count - 1; break;
    case Nav::Back:
    case Nav::Left:
        if (cwd_.has_parent_path() && cwd_.parent_path() != cwd_) openDir(cwd_.parent_path(), cwd_);
        break;
    case Nav::Accept:
    case Nav::Right: {
        if (sel_ < 0 || sel_ >= count) break;
        Entry& e = entries_[size_t(sel_)];
        if (e.isParent) {
            openDir(e.path, cwd_);
        } else if (e.isDir) {
            detectEntry(e);
            if (e.det.system != System::Unknown && n == Nav::Accept) startGame(e.path);  // Wii U game folder
            else openDir(e.path);
        } else if (n == Nav::Accept) {
            startGame(e.path);
        }
        break;
    }
    default: break;
    }
    sel_ = std::clamp(sel_, 0, std::max(0, count - 1));
}

void App::drawLibrary() {
    const int s = uiScale();
    const int W = video_.width(), H = video_.height();
    const int cw = 6 * s, lh = 9 * s + 6;
    const int margin = 8 * s;

    // Header
    video_.fillRect(0, 0, W, 34 * s, kPanel);
    video_.drawText(margin, 6 * s, "NINTENDALL", s * 2, kAccent);
    video_.drawText(margin + Video::textWidth("NINTENDALL", s * 2) + cw, 13 * s, "v" NINTENDALL_VERSION, s, kDim);

    // Which systems have an emulator core installed.
    int x = margin;
    const int sy = 24 * s;
    const auto& systems = allSystems();
    for (size_t i = 0; i < systems.size(); ++i) {
        const std::string n = systemInfo(systems[i]).name;
        video_.drawText(x, sy, n, s, systemReady_[i] ? kGood : kDim);
        x += Video::textWidth(n, s) + cw * 2;
    }

    // Current folder
    const int maxChars = (W - margin * 2) / cw;
    video_.drawText(margin, 38 * s, fit(cwd_.u8string(), maxChars, true), s, kText);

    // File list
    const int top = 38 * s + lh + 4;
    const int bottom = H - lh * 2 - 4;
    const int rows = std::max(1, (bottom - top) / lh);
    if (sel_ < scroll_) scroll_ = sel_;
    if (sel_ >= scroll_ + rows) scroll_ = sel_ - rows + 1;
    scroll_ = std::max(0, std::min(scroll_, int(entries_.size()) - rows));

    if (entries_.empty() || (entries_.size() == 1 && entries_[0].isParent)) {
        video_.drawText(margin, top + lh * 2, "No games in this folder.", s, kDim);
        video_.drawText(margin, top + lh * 3, "Browse to your games, or drag a game file onto this window.", s, kDim);
    }
    const int tagChars = 14;
    for (int r = 0; r < rows && scroll_ + r < int(entries_.size()); ++r) {
        Entry& e = entries_[size_t(scroll_ + r)];
        const int y = top + r * lh;
        if (scroll_ + r == sel_) video_.fillRect(margin / 2, y - 3, W - margin, lh, kSelect);

        detectEntry(e);
        std::string tag;
        Color tagColor = kDim;
        if (e.det.system != System::Unknown) {
            const SystemInfo& si = systemInfo(e.det.system);
            tag = si.name;
            const bool ready = systemReady(e.det.system);
            tagColor = ready ? kGood : kWarn;
            if (!ready) tag += si.needsGpu ? " (coming soon)" : " (missing)";
        } else if (!e.isDir) {
            tag = "?";
        }
        const std::string label = e.isDir && e.det.system == System::Unknown ? e.name + "/" : e.name;
        video_.drawText(margin, y, fit(label, maxChars - tagChars - 1), s,
                        e.isDir && e.det.system == System::Unknown ? kDim : kText);
        if (!tag.empty()) video_.drawText(W - margin - Video::textWidth(tag, s), y, tag, s, tagColor);
    }

    // Footer
    video_.fillRect(0, H - lh * 2, W, lh * 2, kPanel);
    const std::string pads = input_.controllerCount() == 0 ? "No controller (keyboard: arrows, X/Z, Enter)"
                                                           : std::to_string(input_.controllerCount()) + " controller(s): " + input_.controllerName(0);
    video_.drawText(margin, H - lh * 2 + 4, fit("Enter/A Play  Bksp/B Back  F5 Rescan  F11 Fullscreen  Esc x2 Quit", maxChars), s, kDim);
    video_.drawText(margin, H - lh + 2, fit(pads, maxChars), s, kDim);
}

// ============================================================================
// Pause menu

static const char* kPauseItems[] = {"Resume", "Reset game", "Save state", "Load state", "State slot", "Fullscreen", "Quit to library"};
constexpr int kPauseCount = 7;

void App::pauseNav(Nav n) {
    switch (n) {
    case Nav::Up: pauseSel_ = (pauseSel_ + kPauseCount - 1) % kPauseCount; break;
    case Nav::Down: pauseSel_ = (pauseSel_ + 1) % kPauseCount; break;
    case Nav::Left:
        if (pauseSel_ == 4) slot_ = std::max(1, slot_ - 1);
        break;
    case Nav::Right:
        if (pauseSel_ == 4) slot_ = std::min(9, slot_ + 1);
        break;
    case Nav::Back:
    case Nav::Menu:
        screen_ = Screen::Game;
        nextFrame_ = nowSeconds();
        break;
    case Nav::Accept:
        switch (pauseSel_) {
        case 0: screen_ = Screen::Game; nextFrame_ = nowSeconds(); break;
        case 1: core_->reset(); screen_ = Screen::Game; osd("Game reset"); break;
        case 2: saveState(); screen_ = Screen::Game; break;
        case 3: loadState(); screen_ = Screen::Game; break;
        case 4: slot_ = slot_ % 9 + 1; break;
        case 5: video_.toggleFullscreen(); break;
        case 6: stopGame(); screen_ = Screen::Library; break;
        }
        nextFrame_ = nowSeconds();
        break;
    default: break;
    }
}

void App::drawPause() {
    video_.clear({0, 0, 0});
    video_.drawGame();
    const int W = video_.width(), H = video_.height();
    video_.fillRect(0, 0, W, H, {0, 0, 0, 170});

    const int s = uiScale();
    const int lh = 9 * s + 8;
    const int boxW = 30 * 6 * s, boxH = lh * (kPauseCount + 3);
    const int bx = (W - boxW) / 2, by = (H - boxH) / 2;
    video_.fillRect(bx, by, boxW, boxH, kPanel);
    video_.fillRect(bx, by, boxW, 3 * s, kAccent);
    video_.drawText(bx + 8 * s, by + 8 * s, fit(gameTitle_, 28), s, kText);
    video_.drawText(bx + 8 * s, by + 8 * s + lh, std::string(systemInfo(system_).name) + " / " + (core_ ? core_->name() : ""), s, kDim);
    for (int i = 0; i < kPauseCount; ++i) {
        const int y = by + 8 * s + lh * (i + 2) + 4;
        if (i == pauseSel_) video_.fillRect(bx + 4 * s, y - 4, boxW - 8 * s, lh, kSelect);
        std::string label = kPauseItems[i];
        if (i == 2 || i == 3) label += " (slot " + std::to_string(slot_) + ")";
        if (i == 4) label += ":  < " + std::to_string(slot_) + " >";
        video_.drawText(bx + 12 * s, y, label, s, i == pauseSel_ ? kText : kDim);
    }
}

// ============================================================================
// Errors and on-screen messages

void App::showError(const std::string& title, const std::string& text) {
    std::fprintf(stderr, "%s: %s\n", title.c_str(), text.c_str());
    if (opt_.headless) return;
    errorTitle_ = title;
    errorText_ = text;
    errorReturn_ = Screen::Library;
    screen_ = Screen::Error;
}

void App::drawError() {
    drawLibrary();
    const int W = video_.width(), H = video_.height();
    video_.fillRect(0, 0, W, H, {0, 0, 0, 190});
    const int s = uiScale();
    const int lh = 9 * s + 4;
    const int boxW = std::min(W - 16 * s, 70 * 6 * s);
    const int maxChars = (boxW - 16 * s) / (6 * s);
    const auto lines = wrap(errorText_, maxChars);
    const int boxH = lh * (int(lines.size()) + 5);
    const int bx = (W - boxW) / 2, by = std::max(0, (H - boxH) / 2);
    video_.fillRect(bx, by, boxW, boxH, kPanel);
    video_.fillRect(bx, by, boxW, 3 * s, kAccent);
    video_.drawText(bx + 8 * s, by + 8 * s, fit(errorTitle_, maxChars), s, kAccent);
    for (size_t i = 0; i < lines.size(); ++i) video_.drawText(bx + 8 * s, by + 8 * s + lh * int(i + 2), lines[i], s, kText);
    video_.drawText(bx + 8 * s, by + boxH - lh, "Press Enter / A to continue", s, kDim);
}

void App::osd(const std::string& msg, unsigned ms) {
    osdText_ = msg;
    osdUntil_ = SDL_GetTicks() + ms;
    if (opt_.headless) std::fprintf(stderr, "[osd] %s\n", msg.c_str());
}

void App::drawOsd() {
    if (osdText_.empty() || SDL_TICKS_PASSED(SDL_GetTicks(), osdUntil_)) return;
    const int s = uiScale();
    const int maxChars = (video_.width() - 24 * s) / (6 * s);
    const std::string t = fit(osdText_, maxChars);
    const int w = Video::textWidth(t, s) + 12 * s, h = 9 * s + 10;
    const int y = video_.height() - h - 8 * s;
    video_.fillRect(8 * s, y, w, h, {0, 0, 0, 200});
    video_.drawText(14 * s, y + 5, t, s, kText);
}
