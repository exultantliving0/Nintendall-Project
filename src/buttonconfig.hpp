#pragma once
#include <SDL.h>

#include <array>
#include <vector>

#include "input.hpp"
#include "systems.hpp"
#include "text.hpp"
#include "video.hpp"

// Options > Button Config: a small window for remapping keyboard keys and
// controller buttons for each player. Click a box, then press the new key/button.
// It only lists the buttons (and players) of the system that's running.
class ButtonConfig {
public:
    // `rows` = the buttons to list (they depend on the system, and for Wii on the controller type).
    void open(System system, const std::vector<ButtonLabel>& rows);
    void close() { open_ = false; listenRow_ = -1; }
    bool isOpen() const { return open_; }
    // True once after the mapping was changed (so the app can save it).
    bool takeChanged() {
        const bool c = changed_;
        changed_ = false;
        return c;
    }

    // Uses every event while open (it's a modal window). Mouse coords in renderer pixels.
    void handleEvent(const SDL_Event& e, int mouseX, int mouseY, Input& input);
    void draw(Video& video, Text& text, const Input& input, int width, int height);

private:
    System system_ = System::Unknown;
    std::vector<ButtonLabel> rows_;  // buttons shown, in order
    int players_ = Input::kMaxPlayers;
    bool open_ = false;
    bool changed_ = false;
    int player_ = 0;
    int listenRow_ = -1, listenCol_ = -1;  // waiting for a key (col 0) or controller input (col 1)
    int mouseX_ = 0, mouseY_ = 0;

    SDL_Rect dialog_{};
    SDL_Rect tabs_[Input::kMaxPlayers]{};
    std::vector<std::array<SDL_Rect, 2>> cells_;
    SDL_Rect resetBtn_{}, closeBtn_{};
};
