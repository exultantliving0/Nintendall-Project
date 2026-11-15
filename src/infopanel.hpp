#pragma once
#include <SDL.h>

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "gameinfo.hpp"
#include "library.hpp"
#include "text.hpp"
#include "video.hpp"

// The game info panel: right-click a box on the home screen and it slides in from the left with
// three pages:
//   About        the game's description, release date and players (shipped gameinfo files)
//   Back of Box  a picture of the back of the box (click it to see it big), downloaded from TheGamesDB once and kept in
//                "Box Covers/<console>/Back" (a picture already there is used as it is, never replaced)
//   Manual       the game's instruction booklet, page by page, from the Internet Archive's scans
//                (click a page to see it big)
// Downloads happen in the background; the panel shows what it has so far.
class InfoPanel {
public:
    ~InfoPanel();
    // infoDir: the gameinfo folder; cacheDir: Nintendall's own files (which manual each game has).
    void start(const std::filesystem::path& infoDir, const std::filesystem::path& cacheDir);
    // Frees the pictures (before the renderer goes away).
    void shutdown();

    // Slides in with a game's info. backFolder: where pictures of the backs of this console's boxes are.
    void open(const LibraryGame& game, const std::filesystem::path& backFolder);
    void close();
    bool shown() const { return open_ || anim_ > 0.0f; }  // open, or still sliding away
    bool isOpen() const { return open_; }
    bool bigPage() const { return open_ && big_; }  // a manual page shown over the whole home screen
    const std::filesystem::path& game() const { return game_.path; }
    // The panel's place (fully open) in the home screen's area.
    SDL_Rect rect(Text& t, const SDL_Rect& area) const;

    void draw(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY);
    // True if the panel used the event (it takes every click, key and button while it's open).
    bool handleEvent(const SDL_Event& e, int mouseX, int mouseY, Text& t, const SDL_Rect& area);

private:
    enum Tab { About, Back, Manual };
    enum class State { Loading, Ready, None, Offline };
    struct Shared;
    struct Layout {
        float s = 1;
        SDL_Rect panel{}, close{}, content{}, prev{}, next{}, browser{};
        std::vector<SDL_Rect> tabs;
        std::vector<std::string> titleLines;
        std::string subtitle;
        int titleY = 0, subtitleY = 0;
    };
    Layout layout(Text& t, const SDL_Rect& area);
    void setTab(int tab);
    void setPage(int page);
    void scrollBy(int dy);
    void takeResults(SDL_Renderer* r);
    void requestPage(int page);
    void freePictures();
    void drawBig(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY);
    bool bigEvent(const SDL_Event& e, int mouseX, int mouseY, const SDL_Rect& area);
    const std::vector<std::string>& wrapped(Text& t, const std::string& text, int width);

    std::shared_ptr<Shared> shared_;
    LibraryGame game_;
    bool open_ = false;
    float anim_ = 0.0f;  // 0 = hidden, 1 = fully open
    Uint32 lastTick_ = 0;
    int tab_ = About;
    int scroll_ = 0, maxScroll_ = 0;
    unsigned generation_ = 0;

    // What's known so far.
    State infoState_ = State::Loading, backState_ = State::Loading, manualState_ = State::Loading;
    GameInfo info_;
    SDL_Texture* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    std::string manualId_, manualTitle_;
    std::vector<std::string> manualPages_;  // the page pictures' addresses
    int page_ = 0;
    struct Page {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
        bool failed = false;
    };
    std::map<int, Page> pages_;  // loaded (or asked for) pages, near the one shown
    SDL_Rect pageRect_{};        // where the page is in the panel (click it to see it big)
    bool big_ = false;           // the page (or back of the box) shown big, over the whole home screen

    // Wrapped text, remembered (measuring text is slow).
    std::string wrapKey_;
    std::vector<std::string> wrapLines_;
};
