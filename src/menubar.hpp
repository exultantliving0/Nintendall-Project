#pragma once
#include <SDL.h>

#include <string>
#include <vector>

#include "text.hpp"
#include "video.hpp"

struct MenuItem {
    std::string label;     // empty label = separator line
    std::string shortcut;  // shown on the right, e.g. "F5"
    int action = 0;        // what the app should do when it's clicked
    bool enabled = true;
    bool checked = false;
};

struct Menu {
    std::string title;
    std::vector<MenuItem> items;
};

// A classic desktop-style menu bar (File, Emulation, ...) drawn inside the window.
class MenuBar {
public:
    // Rebuilt by the app every frame so check marks / greyed-out items stay current.
    void setMenus(std::vector<Menu> menus) { menus_ = std::move(menus); }

    // Mouse coordinates are in renderer pixels. Returns true if the menu used the
    // event; `action` is set (> 0) when an item was chosen.
    bool handleEvent(const SDL_Event& e, int mouseX, int mouseY, int& action);
    void draw(Video& video, Text& text, int width);
    int height(const Text& text) const;
    bool isOpen() const { return open_ >= 0; }
    void openFirst() { open_ = 0; hover_ = -1; }
    void close() { open_ = -1; hover_ = -1; }

private:
    bool selectable(int item) const;
    void moveHover(int dir);

    std::vector<Menu> menus_;
    std::vector<SDL_Rect> titleRects_;
    std::vector<SDL_Rect> itemRects_;
    SDL_Rect dropRect_{};
    int barHeight_ = 0;
    int open_ = -1;        // index of the open menu
    int hover_ = -1;       // highlighted item in the open menu
    int hoverTitle_ = -1;  // title under the mouse
    bool pressedOnTitle_ = false;
};
