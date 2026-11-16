#pragma once
#include <filesystem>
#include <string>
#include <vector>

#include "library.hpp"

// What's known about a game: its name, release date, players and the web address of the back of
// its box. It comes from the gameinfo folder shipped next to the program (made from
// TheGamesDB's database by tools/make_game_info.py), found by the game's name.
struct GameInfo {
    std::string title;     // its name in the database
    std::string released;  // "1985-10-18" ("" = not known)
    std::string players;   // "2" ("" = not known)
    std::string backUrl;   // picture of the back of the box ("" = none)
};

// Looks a game up in `dir` (the gameinfo folder). False if it isn't there.
// The console's file is read the first time and kept (call from one thread only).
bool findGameInfo(const std::filesystem::path& dir, const LibraryGame& game, GameInfo& out);

// The web address of a game's manual on Digitpress (a small PDF that loads fast), from the
// gameinfo folder's manuals.txt, found by any of `names`. "" if it isn't there.
std::string findDigitpressManual(const std::filesystem::path& dir, System system, const std::vector<std::string>& names);

// A game name as the database's files key it: lower case, only letters and numbers, without "the"
// ("Legend of Zelda, The - Ocarina of Time" -> "legendofzeldaocarinaoftime").
std::string gameInfoKey(const std::string& name);

// The words of a name, folded like gameInfoKey (lower case, accents dropped, "&" as "and").
std::vector<std::string> nameWords(const std::string& name);
