#pragma once
#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "library.hpp"

// Box covers for the home screen. They live in the Box Covers folder (next to the Games folder,
// one folder per console), named like the game file: "Box Covers/NES/Super Mario Bros. (World).png".
// A picture already there is used as it is, so players can add or replace covers with their own
// (.png or .jpg). Missing ones are downloaded in the background and saved there, from the libretro
// thumbnail library (thumbnails.libretro.com): the USA release's cover, found by matching the
// game's name against the library's list of names. Games with no USA cover there get none.
class Covers {
public:
    ~Covers();
    // cacheDir: Nintendall's own files (name lists, notes of covers that don't exist).
    void start(const std::filesystem::path& cacheDir);
    // The Box Covers folder of the console being shown.
    void setFolder(const std::filesystem::path& dir) { folder_ = dir; }
    // The space a cover is shown in (pixels): covers are made exactly that size (stretched a little
    // if their shape is different), so they're sharp and all the same size.
    // A new size (cover size changed with Ctrl + / -, or the screen scale) reloads them.
    void setDisplaySize(int width, int height);
    // A game's cover, or nullptr while it's still loading or if none was found.
    SDL_Texture* get(const LibraryGame& game);
    // Makes textures from finished downloads. Call once per frame, before drawing.
    void update(SDL_Renderer* renderer);
    // Picks up covers added, replaced or deleted in the Box Covers folder (call every few seconds).
    void recheck(const std::vector<LibraryGame>& games);
    // Frees every texture and drops waiting requests (switching consoles, closing).
    void clear();

private:
    struct Shared;
    struct Entry {
        SDL_Texture* tex = nullptr;
        bool loading = true;
        std::string file;  // the picture shown ("" = none), and its time, to notice changes
        std::filesystem::file_time_type time{};
        Uint32 retryAt = 0;  // couldn't be looked up (offline, server trouble): look again then (SDL ticks)
    };
    std::shared_ptr<Shared> shared_;
    std::filesystem::path folder_;
    std::unordered_map<std::string, Entry> textures_;  // by game path
};

// The cover picture in `dir` for a game file name (.png, .jpg or .jpeg), or "" if there's none.
std::filesystem::path findCoverFile(const std::filesystem::path& dir, const std::string& fileName);

// The web address of a game's box cover (where Nintendall downloads it from), found with the lists
// of names already saved in cacheDir, without going online. "" if there's none. (Discord status.)
std::string coverUrl(const std::filesystem::path& cacheDir, System system, const std::string& fileName,
                     const std::string& title);
