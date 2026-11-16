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

// The game info panel: right-click a box on the home screen and it slides in from the right with
//   Add Custom Cover  pick a picture on the computer: it becomes the game's box cover
//   Back of Box       a picture of the back of the box (click it to see it big), downloaded once (LaunchBox Games Database or TheGamesDB) and
//                     kept in "Box Covers/<console>/Back" (a picture already there is used as it is, never replaced)
//   Manual            the game's instruction booklet, page by page (click a page to see it big): from Digitpress
//                     or the Internet Archive, downloaded once and kept with Nintendall's own files
// Downloads happen in the background; the panel shows what it has so far.
class InfoPanel {
public:
    ~InfoPanel();
    // infoDir: the gameinfo folder; cacheDir: Nintendall's own files (the manuals downloaded).
    void start(const std::filesystem::path& infoDir, const std::filesystem::path& cacheDir);
    // Frees the pictures (before the renderer goes away).
    void shutdown();

    // Slides in with a game's info. coverFolder: this console's Box Covers folder.
    void open(const LibraryGame& game, const std::filesystem::path& coverFolder);
    void close();
    bool shown() const { return open_ || anim_ > 0.0f; }  // open, or still sliding away
    bool isOpen() const { return open_; }
    bool bigPage() const { return open_ && big_; }  // a manual page shown over the whole home screen
    const std::filesystem::path& game() const { return game_.path; }
    // True once after a custom cover was added (the home screen then shows it).
    bool takeCoverAdded() {
        const bool added = coverAdded_;
        coverAdded_ = false;
        return added;
    }
    // The panel's place (fully open) in the home screen's area.
    SDL_Rect rect(Text& t, const SDL_Rect& area) const;

    void draw(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY);
    // True if the panel used the event (it takes every click, key and button while it's open).
    bool handleEvent(const SDL_Event& e, int mouseX, int mouseY, Text& t, const SDL_Rect& area);

private:
    enum Tab { Back, Manual, TabCount };
    enum class State { Loading, Ready, None, Offline };
    struct Shared;
    struct Layout {
        float s = 1;
        SDL_Rect panel{}, close{}, cover{}, content{}, prev{}, next{}, browser{};
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
    void addCover(SDL_Window* window);
    void drawBig(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY);
    bool bigEvent(const SDL_Event& e, int mouseX, int mouseY, const SDL_Rect& area);
    const std::vector<std::string>& wrapped(Text& t, const std::string& text, int width);

    std::shared_ptr<Shared> shared_;
    LibraryGame game_;
    std::filesystem::path coverFolder_;
    bool open_ = false;
    float anim_ = 0.0f;  // 0 = hidden, 1 = fully open
    Uint32 lastTick_ = 0;
    int tab_ = Back;
    int scroll_ = 0, maxScroll_ = 0;
    unsigned generation_ = 0;

    // Add Custom Cover: what happened, shown under the button for a while.
    std::string coverNote_;
    bool coverNoteBad_ = false;
    Uint32 coverNoteUntil_ = 0;
    bool coverAdded_ = false;

    // What's known so far.
    State infoState_ = State::Loading, backState_ = State::Loading, manualState_ = State::Loading;
    GameInfo info_;
    SDL_Texture* back_ = nullptr;
    int backW_ = 0, backH_ = 0;
    std::string manualFrom_, manualLink_;  // where the manual is from ("Digitpress"), its web page
    int manualPages_ = 0;                  // how many pages
    std::vector<std::string> manualUrls_;  // the pages' pictures on the Internet Archive (else they're in a PDF)
    int manualProgress_ = -1;              // downloading the manual: percent done (-1 = not known)
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
