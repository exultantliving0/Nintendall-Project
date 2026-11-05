#pragma once
#include <SDL.h>

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "audio.hpp"
#include "buttonconfig.hpp"
#include "config.hpp"
#include "core.hpp"
#include "detect.hpp"
#include "input.hpp"
#include "menubar.hpp"
#include "paths.hpp"
#include "settingsdialog.hpp"
#include "text.hpp"
#include "video.hpp"
#include "zip.hpp"

struct LaunchOptions {
    bool headless = false;     // --frames N: run without a window (testing/benchmarks)
    int frames = 0;
    std::string screenshot;    // --screenshot FILE (with --frames)
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
    unsigned audioOutputRate() const override { return audio_.ok() ? unsigned(audio_.outputRate()) : 48000u; }
    float displayRefreshRate() const override { return float(video_.displayHz()); }
    void hwOutputSize(unsigned& w, unsigned& h) const override;

private:
    // Menu actions
    enum Action {
        ActOpen = 1, ActCloseGame, ActExit,
        ActRecent = 10,  // + index
        ActPause = 30, ActReset, ActFastForward,
        ActSaveState = 40, ActLoadState,
        ActSlot = 50,  // + slot number
        ActButtonConfig = 70, ActFullscreen, ActConsoleSettings, ActWiiUKeys,
        ActControls = 90, ActAbout,
        ActLayout = 100,  // + layout index
        ActLayoutSwap = 120,
    };
    static constexpr int kSlots = 5;
    static constexpr int kRecent = 8;

    // Games
    bool startGame(const std::filesystem::path& path);
    void stopGame();
    void runFrames();
    void saveSram(bool force);
    void loadSram();
    std::filesystem::path statePath(int slot) const;
    void saveState();
    void loadState();
    void takeScreenshot();
    void addRecent(const std::filesystem::path& path);
    std::vector<std::string> recentGames() const;

    // Window & UI
    void handleEvent(const SDL_Event& e);
    void doAction(int action);
    void buildMenus();
    void draw();
    bool menuVisible() const;
    SDL_Rect gameArea() const;
    float uiScale() const;
    void openFileDialog();
    // Wii U disc images are encrypted with keys from the player's own console (Cemu reads keys.txt).
    void importWiiUKeys();
    std::filesystem::path wiiUKeysFile() const;
    bool needsWiiUKeys(const std::filesystem::path& game) const;
    void showMessage(const std::string& title, const std::string& text, bool error);
    void osd(const std::string& msg, unsigned ms = 2500);
    void saveConfig();
    void pace(double frameSeconds);

    // DS screen layout
    void loadLayout();
    void applyLayout();
    void fitWindowToGame();
    int16_t pointerState(unsigned id) const;
    void updateInputProfile();
    void connectPorts();

    // Subsystems
    Paths paths_;
    Config cfg_;
    Video video_;
    Audio audio_;
    Input input_;
    Text text_;
    MenuBar menu_;
    ButtonConfig buttonConfig_;
    SettingsDialog settings_;
    bool settingsWasOpen_ = false;
    LaunchOptions opt_;
    std::unique_ptr<LibretroCore> core_;

    bool quit_ = false;

    // Running game
    System system_ = System::Unknown;
    std::filesystem::path gamePath_;   // what the user picked (zip or file)
    std::filesystem::path extracted_;  // temp file pulled out of a zip
    std::string gameTitle_;
    std::vector<uint8_t> lastSram_;
    Uint32 lastSramCheck_ = 0;
    int slot_ = 1;
    bool paused_ = false;
    bool focusLost_ = false;
    bool fastForward_ = false;
    bool fastForwardToggled_ = false;
    bool wasRunning_ = false;
    std::array<int, 16> inputRemap_{};  // button the core asks for -> our button (GameCube/Wii face buttons)
    std::vector<ButtonLabel> buttonRows_;  // what Button Config lists for this game
    int layout_ = 0;           // index into the DS layout list
    bool layoutSwap_ = false;  // swap top and bottom screens
    int pendingFitFrames_ = 0;  // after a layout change: frames left to wait for the new picture size
    bool avChanged_ = false;    // the core changed its picture size/rotation during this frame

    // Mouse / messages
    int mouseX_ = 0, mouseY_ = 0;
    Uint32 lastMouseMove_ = 0;
    std::string osdText_;
    Uint32 osdUntil_ = 0;

    double nextFrame_ = 0;

    // Headless stats
    uint64_t audioFramesSeen_ = 0;
    int audioPeak_ = 0;
};
