#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "systems.hpp"

// The games folder: Documents/Nintendall/Games, with one folder per console, shown on the home screen.

// A console on the home screen, in release order (NES ... Wii U).
struct HomeConsole {
    System system;
    const char* folder;      // its folder inside the games folder
    const char* name;        // full name, e.g. "Game Boy Advance"
    const char* label;       // tab label, e.g. "GB Advance"
    const char* shortLabel;  // tab label when the window is narrow, e.g. "GBA"
};
const std::vector<HomeConsole>& homeConsoles();

// Documents/Nintendall/Games (or Games/ next to the program in portable mode).
std::filesystem::path defaultGamesFolder(const std::filesystem::path& programDir, bool portable);
// Makes the games folder and every console folder in it (if they're missing).
void createGameFolders(const std::filesystem::path& root);
// Shows a folder in Explorer / Finder / the file manager.
void openFolder(const std::filesystem::path& dir);

struct LibraryGame {
    std::filesystem::path path;  // file (or Wii U game folder) to start
    System system = System::Unknown;  // the console the game is for (FDS games sit in the NES folder)
    std::string title;           // shown under the cover
    std::string fileName;        // file name without extension, for finding the cover by name
    std::string gameId;          // the game's ID code (GameCube, Wii, Wii U, DS, 3DS)
    int disc = 0;                // GameCube / Wii: which disc this is (1, 2...); 0 = not known
    // A game on several discs is one game: `path` is its first disc, and this lists every disc
    // in order (empty for one-disc games).
    std::vector<std::filesystem::path> discs;
};

class Library {
public:
    // The games in a console's folder (and its subfolders), sorted by title.
    // Only new or changed files are read, so calling this often is cheap.
    std::vector<LibraryGame> scan(const std::filesystem::path& folder, System console);

private:
    struct Known {
        std::filesystem::file_time_type time;
        uintmax_t size = 0;
        LibraryGame game;
    };
    std::unordered_map<std::string, Known> known_;
};

// "Legend of Zelda, The (USA) [!]" -> "The Legend of Zelda"
std::string cleanTitle(const std::string& fileName);

// The disc number in a file name ("Tales of Symphonia (USA) (Disc 2)", "Game - Disc 2", "[Disk 2]")
// or 0, and the name without it ("Tales of Symphonia (USA)").
int discInName(const std::string& fileName, std::string* rest = nullptr);

// One game's name and ID, read from its file (or Wii U game folder).
LibraryGame readGameInfo(const std::filesystem::path& path, System console);

// Every disc of the game a GameCube / Wii disc belongs to, starting with that one (the game's other
// discs, from the same folder). Just the file itself for one-disc games.
std::vector<std::filesystem::path> gameDiscs(const std::filesystem::path& file, System console);
