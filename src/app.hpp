#pragma once
#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio.hpp"
#include "config.hpp"
#include "core.hpp"
#include "detect.hpp"
#include "input.hpp"
#include "paths.hpp"
#include "video.hpp"
#include "zip.hpp"

struct LaunchOptions {
    bool headless = false;   // --frames N: run without a window (testing/benchmarks)
    int frames = 0;
    std::string screenshot;  // --screenshot FILE (with --frames)
    uint16_t holdButtons = 0;  // --hold a,start (with --frames)
    bool fullscreen = false;
};

class App : public CoreHost {
public:
    int run(const std::filesystem::path& start, const LaunchOptions& opt);

    // CoreHost
    void onVideo(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt) override;
    void onAudio(const int16_t* stereo, size_t frames) override;
    int16_t onInputState(unsigned port, unsigned device, unsigned index, unsigned id) override;
    bool onRumble(unsigned port, retro_rumble_effect effect, uint16_t strength) override;
    void onMessage(const std::string& msg, unsigned durationMs) override;
    void onAvInfoChanged() override;
    bool isFastForwarding() const override { return fastForward_; }

private:
    enum class Screen { Library, Game, Pause, Error };
    enum class Nav { None, Up, Down, Left, Right, PageUp, PageDown, Home, End, Accept, Back, Menu };

    struct Entry {
        std::filesystem::path path;
        std::string name;
        bool isDir = false;
        bool isParent = false;
        bool detected = false;
        Detection det;
    };

    // Games
    bool startGame(const std::filesystem::path& path);
    void stopGame();
    void gameFrame();
    void saveSram(bool force);
    void loadSram();
    std::filesystem::path statePath(int slot) const;
    void saveState();
    void loadState();
    void takeScreenshot();

    // Library
    void openDir(const std::filesystem::path& dir, const std::filesystem::path& select = {});
    void refreshCores();
    bool systemReady(System s) const;
    void detectEntry(Entry& e);
    void libraryNav(Nav n);
    void drawLibrary();

    // Pause menu / errors / messages
    void pauseNav(Nav n);
    void drawPause();
    void drawError();
    void showError(const std::string& title, const std::string& text);
    void osd(const std::string& msg, unsigned ms = 2500);
    void drawOsd();

    // Events
    void handleEvent(const SDL_Event& e);
    Nav navFromKey(const SDL_KeyboardEvent& k) const;
    Nav navFromButton(Uint8 button) const;
    void dispatchNav(Nav n);
    void updateHeldNav();
    void pace(double frameSeconds);
    int uiScale() const;

    // Subsystems
    Paths paths_;
    Config cfg_;
    Video video_;
    Audio audio_;
    Input input_;
    LaunchOptions opt_;
    std::unique_ptr<LibretroCore> core_;

    bool quit_ = false;
    Screen screen_ = Screen::Library;

    // Library state
    std::filesystem::path cwd_;
    std::vector<Entry> entries_;
    int sel_ = 0, scroll_ = 0;
    std::vector<bool> systemReady_;

    // Running game state
    System system_ = System::Unknown;
    std::filesystem::path gamePath_;     // what the user picked (zip or file)
    std::filesystem::path extracted_;    // temp file pulled out of a zip
    std::string gameTitle_;
    std::vector<uint8_t> lastSram_;
    Uint32 lastSramCheck_ = 0;
    int slot_ = 1;
    bool fastForward_ = false;
    bool fastForwardToggled_ = false;
    int pauseSel_ = 0;

    // Error / OSD
    std::string errorTitle_, errorText_;
    Screen errorReturn_ = Screen::Library;
    std::string osdText_;
    Uint32 osdUntil_ = 0;
    Uint32 quitArmedUntil_ = 0;

    // Controller menu navigation repeat
    Nav heldNav_ = Nav::None;
    Uint32 heldSince_ = 0, lastRepeat_ = 0;

    // Frame pacing
    double nextFrame_ = 0;

    // Headless stats
    uint64_t audioFramesSeen_ = 0;
    int audioPeak_ = 0;
};
