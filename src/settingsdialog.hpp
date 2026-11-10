#pragma once
#include <SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#include "consolesettings.hpp"
#include "core.hpp"
#include "text.hpp"
#include "video.hpp"

// Options > Console Settings: picture, console and sound settings for the
// system that's running, taken from its emulator and given friendly names.
// Also opens from the home screen (gameRunning false): the emulator is loaded without a game,
// and the settings apply the next time one of that console's games starts.
class SettingsDialog {
public:
    void open(System system, LibretroCore& core, bool gameRunning = true);
    // Wii U: Cemu's graphic pack folders (system/Cemu/graphicPacks and customGraphicPacks), to
    // find the loaded game's Resolution pack.
    void setWiiUPackFolders(const std::filesystem::path& bundled, const std::filesystem::path& custom) {
        bundledPacks_ = bundled;
        customPacks_ = custom;
    }
    void close() { open_ = false; }
    bool isOpen() const { return open_; }
    // True once after the player clicked "Restart Game".
    bool takeRestartRequest() {
        const bool r = restartRequested_;
        restartRequested_ = false;
        return r;
    }
    bool restartPending() const { return pendingRestart_; }
    // True once after the player clicked "Clear Shader Cache" (Wii U).
    bool takeClearCacheRequest() {
        const bool r = clearCacheRequested_;
        clearCacheRequested_ = false;
        return r;
    }

    // Modal: uses every event while open. Mouse coordinates in renderer pixels.
    void handleEvent(const SDL_Event& e, int mouseX, int mouseY, LibretroCore& core);
    void draw(Video& video, Text& text, LibretroCore& core, int width, int height);

private:
    struct Row {
        std::string header;                  // non-empty = section title row
        Setting setting;                     // for setting rows
        std::vector<SettingChoice> choices;  // resolved against the core's options
        SDL_Rect valueBox{}, prevBox{}, nextBox{};
    };
    int currentChoice(const Row& r, LibretroCore& core) const;
    void findResolutionPack(std::map<std::string, CoreOption>& options, std::string& enableKey,
                            std::string& presetKey) const;
    void apply(Row& r, int choice, LibretroCore& core);

    System system_ = System::Unknown;
    std::filesystem::path bundledPacks_, customPacks_;
    std::vector<Row> rows_;
    bool open_ = false;
    bool gameRunning_ = true;
    bool pendingRestart_ = false;
    bool restartRequested_ = false;
    bool clearCacheRequested_ = false;
    int scroll_ = 0, maxScroll_ = 0, rowH_ = 1;
    int mouseX_ = 0, mouseY_ = 0;
    SDL_Rect dialog_{}, list_{}, resetBtn_{}, restartBtn_{}, closeBtn_{}, cacheBtn_{};
};
