#pragma once
#include <SDL.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "covers.hpp"
#include "library.hpp"
#include "text.hpp"
#include "video.hpp"

// The home screen shown when no game is running: a row of consoles (NES ... Wii U) and, under it,
// the games in that console's folder with their box covers. Click a game (or pick it with the
// arrow keys / a controller and press Enter / A) to play it.
class Home {
public:
    // gamesRoot: the Games folder; coversRoot: the Box Covers folder (both with a folder per console);
    // coverCache: Nintendall's own cover notes.
    void start(const std::filesystem::path& gamesRoot, const std::filesystem::path& coversRoot,
               const std::filesystem::path& coverCache, const std::string& consoleKey);
    void shutdown();  // frees the cover textures (before the renderer goes away)

    std::string consoleKey() const;  // the console being shown, e.g. "gba"
    System consoleSystem() const;
    const std::filesystem::path& gamesRoot() const { return root_; }
    const std::filesystem::path& coversRoot() const { return coversRoot_; }
    std::filesystem::path consoleFolder() const;
    void refresh();  // look in the folder again
    std::string titleFor(const std::filesystem::path& game) const;  // its name on the home screen ("" if not there)

    // Cover size (Ctrl + / Ctrl - / Ctrl 0), as a step in kCoverSizes; 0 = normal.
    int coverSize() const { return coverSize_; }
    void setCoverSize(int step);

    void draw(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY);
    // True if the home screen used the event. `launch` is set when a game was picked.
    bool handleEvent(const SDL_Event& e, int mouseX, int mouseY, Text& t, const SDL_Rect& area,
                     std::filesystem::path& launch);

private:
    struct Layout {
        float s = 1;
        std::vector<SDL_Rect> tabs;
        std::vector<std::string> labels;
        SDL_Rect tabBar{}, header{}, folderButton{}, grid{};
        int margin = 0, cardW = 0, coverH = 0, cardH = 0, gapX = 0, gapY = 0, cols = 1, x0 = 0, maxScroll = 0;
    };
    Layout layout(Text& t, const SDL_Rect& area) const;
    SDL_Rect cardRect(const Layout& L, int i) const;  // whole card (cover + title), scrolled
    int cardAt(const Layout& L, int x, int y) const;
    void selectConsole(int index);
    void moveSelection(const Layout& L, int dx, int dy);
    void scrollTo(const Layout& L, int i);
    void zoom(Text& t, const SDL_Rect& area, int step);  // change the cover size, keeping the same games in view
    // The title split into lines (remembered: measuring text is slow).
    const std::vector<std::string>& titleLines(Text& t, const LibraryGame& g, int width, int maxLines);

    Library library_;
    Covers covers_;
    std::filesystem::path root_, coversRoot_;
    int console_ = 0;
    int coverSize_ = 0;
    std::vector<LibraryGame> games_;
    int scroll_ = 0;
    int selected_ = -1;   // picked with the keyboard / a controller
    bool showSelected_ = false;
    int pressed_ = -1;    // card the mouse button went down on
    int stickX_ = 0, stickY_ = 0;  // left stick direction last seen (moves once per push)
    Uint32 lastScan_ = 0, lastDraw_ = 0;
    std::unordered_map<std::string, std::vector<std::string>> lines_;  // titles split into lines
    float linesScale_ = 0;
};
