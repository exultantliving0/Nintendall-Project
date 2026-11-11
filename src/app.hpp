#pragma once
#include <SDL.h>

#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio.hpp"
#include "buttonconfig.hpp"
#include "config.hpp"
#include "core.hpp"
#include "detect.hpp"
#include "home.hpp"
#include "input.hpp"
#include "lan.hpp"
#include "landialog.hpp"
#include "netplay.hpp"
#include "menubar.hpp"
#include "paths.hpp"
#include "settingsdialog.hpp"
#include "text.hpp"
#include "update.hpp"
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
        ActOpen = 1, ActCloseGame, ActExit, ActGamesFolder, ActCoversFolder,
        ActRecent = 10,  // + index
        ActPause = 30, ActReset, ActFastForward,
        ActSaveState = 40, ActLoadState,
        ActSlot = 50,  // + slot number
        ActButtonConfig = 70, ActFullscreen, ActConsoleSettings, ActWiiUKeys, ActPairWiimotes,
        ActControls = 90, ActAbout,
        ActLayout = 100,  // + layout index
        ActLayoutSwap = 120,
        ActLanHost = 130, ActLanJoin, ActLanStart, ActLanDelayUp, ActLanDelayDown, ActLanLeave, ActLanHelp,
        ActShader = 150,  // + index in shaderList()
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
    SDL_Rect homeArea() const;  // the home screen (game list), under the menu bar
    float uiScale() const;
    void openFileDialog();
    // Wii U disc images are encrypted with keys from the player's own console (Cemu reads keys.txt).
    void importWiiUKeys();
    // Options > Pair Wii Remotes: pairing runs on a thread; the result is shown when it's done.
    void startWiimotePairing();
    void checkWiimotePairing();
    struct PairJob;
    std::shared_ptr<PairJob> pairJob_;
    // New version check: asks GitHub on a thread when Nintendall starts. The popup only shows
    // on the home screen, and never once a game has been started.
    void startUpdateCheck();
    void checkUpdateResult();
    struct UpdateJob;
    std::shared_ptr<UpdateJob> updateJob_;
    bool playedSinceLaunch_ = false;
    // Update Now (Windows): download the setup with a progress screen, then install and restart.
    void startUpdateDownload(const UpdateInfo& info);
    void checkUpdateDownload();
    void drawUpdateDownload();
    struct DownloadJob;
    std::shared_ptr<DownloadJob> download_;
    std::filesystem::path wiiUKeysFile() const;
    void writeWiiUGamePaths() const;  // tells Cemu where to look for updates and DLC
    bool needsWiiUKeys(const std::filesystem::path& game) const;
    void showMessage(const std::string& title, const std::string& text, bool error);
    void osd(const std::string& msg, unsigned ms = 2500);
    void saveConfig();
    void pace(double frameSeconds);

    // DS screen layout
    void loadLayout();
    void applyLayout();
    void fitWindowToGame();
    // "Separate Windows" layout: the second screen gets its own window.
    bool separateWindows() const;
    SDL_Rect screenCrop(bool bottom, int frameW, int frameH) const;  // one screen's part of the frame
    void updateScreenWindow();  // open / close the second window to match the layout
    void setLayout(int layout, bool swap);
    int16_t pointerState(unsigned id) const;
    void updateInputProfile();
    void connectPorts();
    bool realWiimotes() const;  // Wii game set to use real Wii Remotes
    // Wii U: Cemu's shader cache (system/Cemu/shaderCache). Shaders compiled by another build of
    // Cemu can draw parts of games black, so it's cleared when the built-in Cemu changes, and on
    // request (Console Settings > Clear Shader Cache).
    std::filesystem::path wiiUShaderCache() const;
    void clearWiiUShaderCache();
    bool sdlWiiDriverOff_ = false;
    // Shaders menu: chosen per console (the running game's, or the one shown on the home screen).
    System shaderSystem() const { return core_ ? system_ : home_.consoleSystem(); }
    Shader shaderFor(System s) const;

    // LAN games (GameCube / Wii, through Dolphin's netplay): LAN > Host Game / Join Game.
    void openLanDialog(LanDialog::Kind kind);
    std::vector<LanDialog::Game> lanGames();
    bool startLanGame(const LanDialog::Request& req);
    void updateLan();       // every frame: the session's state, messages, and its end
    bool lanLobbyShown() const;
    LanLobby::Info lanLobbyInfo() const;
    void leaveLan(const std::string& message);
    bool lanBlocks(const char* what);  // pausing etc. during a LAN game: says why not, returns true
    void lanStart();                   // host: Start Game
    void updateNetplay();
    bool netInputActive() const {
        return lan_.net && lan_.net->phase() == Netplay::Phase::Playing && !lan_.net->game().link;
    }
    // Link-cable LAN games (Game Boy / Color / Advance)
    void joinLinkGame();  // joining: load our own game, with the link cable to the host
    void linkFrame();     // one frame of our own game, with the other players' link packets
public:
    void linkPump();      // network: packets in, players joining / leaving (also from the emulator)
    void linkSend(unsigned to, const void* data, size_t size);
private:
    bool lanLinkLoad_ = false;  // startGame: load a link-cable game's emulator
    int16_t localInputState(unsigned port, unsigned device, unsigned index, unsigned id);
    NetInput localNetInput();
    NetInputs netInputs_{};  // the presses for the frame being run (LAN games, NES / SNES / N64)
    bool netReplay_ = false; // running frames again (rollback) or warming up: no picture or sound
    std::vector<std::pair<std::string, std::string>> lanCoreOptions_;  // the host's options, while loading
    std::map<std::string, std::pair<uintmax_t, uint32_t>> lanCrcs_;  // game file -> (size, CRC-32)
    struct Lan {
        bool active = false;  // a LAN game is loaded (lobby or playing); joining NES/SNES/N64: also before
        LanDialog::Request req;
        LanApi api;                   // GameCube / Wii: Dolphin's netplay
        std::unique_ptr<Netplay> net; // NES / SNES / N64: Nintendall's netplay
        bool netLoaded = false;       // joining: the host's game is loaded
        bool portsSet = false;        // the players' controller ports are plugged in (from frame 0)
        int warmup = 0;               // frames run after loading, before the lobby holds the game
        LanStatus status;
        std::vector<std::string> addresses;
        std::string errorText;
        bool switchedGame = false;  // joined with another game after the host's turned out different
        bool sawHost = false;       // joining: the host has been in the player list
        bool linkStarted = false;   // link-cable game: the emulator was told the session started
    };
    Lan lan_;
    const LanDialog::Request* lanPending_ = nullptr;  // while startGame loads a LAN game
    LanDialog lanDialog_;
    LanLobby lanLobby_;
    Library lanLibrary_;

    // Subsystems
    Paths paths_;
    Config cfg_;
    Video video_;
    Video screen2_;  // the second screen's window (Separate Windows layout only)
    Audio audio_;
    Input input_;
    Text text_;
    MenuBar menu_;
    Home home_;  // shown when no game is running
    ButtonConfig buttonConfig_;
    SettingsDialog settings_;
    bool settingsWasOpen_ = false;
    // Console Settings from the home screen: the console's emulator, loaded without a game while
    // the window is open (closing it saves the settings).
    std::unique_ptr<LibretroCore> settingsCore_;
    void openHomeSettings(System system);
    LibretroCore* settingsCore() { return core_ ? core_.get() : settingsCore_.get(); }
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
