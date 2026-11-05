#pragma once
#include <SDL.h>

#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>

#include "library.hpp"

// Box covers for the home screen, downloaded in the background and kept in a cache folder.
//   GameCube, Wii, Wii U, DS, 3DS: GameTDB (art.gametdb.com), found by the game's ID code.
//   Every console (and the above when there's no ID): the libretro thumbnail library
//   (thumbnails.libretro.com), found by matching the file name against its list of game names.
class Covers {
public:
    ~Covers();
    void start(const std::filesystem::path& cacheDir);
    // A game's cover, or nullptr while it's still loading or if none was found.
    SDL_Texture* get(const LibraryGame& game);
    // Makes textures from finished downloads. Call once per frame, before drawing.
    void update(SDL_Renderer* renderer);
    // Frees every texture and drops waiting requests (switching consoles, closing).
    void clear();

private:
    struct Shared;
    std::shared_ptr<Shared> shared_;
    std::unordered_map<std::string, SDL_Texture*> textures_;  // by game path; nullptr = loading / none
};
