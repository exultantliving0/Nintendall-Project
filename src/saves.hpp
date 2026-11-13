#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "systems.hpp"

// The Saves folder (Documents/Nintendall/Saves, next to Games and Box Covers) has a folder per
// console. Battery saves (NES, SNES, N64, Game Boy, GBA, DS) live there, named like the game file:
// "Saves/SNES/Super Mario World (USA).srm". Saves from other emulators can be put there with the
// game's name ("Pokemon Emerald.sav"): the next time the game starts, Nintendall uses them.

// Makes the Saves folder's console folders.
void createSaveFolders(const std::filesystem::path& savesRoot);

// The console folder for a game's save: the folder the game is in inside the Games folder (so a
// .gbc game in Games/Game Boy saves in Saves/Game Boy), or else the console's own folder.
std::filesystem::path savesFolderFor(const std::filesystem::path& savesRoot, const std::filesystem::path& gamesRoot,
                                     const std::filesystem::path& game, System system);

// Looks in `dir` for another emulator's save of the game named `name` (.sav, .dsv, Project64's
// .eep / .sra / .fla / .mpk...). If there is one, it's put into `ram` (the emulator's save memory,
// already holding the emulator's empty save: parts the files don't have are kept) and the files
// used are returned. Empty = nothing to use.
std::vector<std::filesystem::path> importSave(const std::filesystem::path& dir, const std::string& name, System system,
                                              std::vector<uint8_t>& ram);

// Moves files (or folders) that were used into the Imported folder next to them (so they're used
// only once, and are still there if they're wanted again). Returns false if one couldn't be moved.
bool moveToImported(const std::vector<std::filesystem::path>& files);

// Wii, Wii U and 3DS saves are folders inside the emulators' own files. Save folders from
// other emulators put in Saves/<console> are copied there when a game of that console starts:
//   Wii (Dolphin):          the game's folder from Wii/title/00010000 ("524d4345"), or one named
//                           like the game file or with its ID ("RMCE01")
//   Wii U (Cemu):           the game's folder from mlc01/usr/save/00050000 ("101c9400")
//   3DS (Citra / Azahar):   the game's folder from sdmc/Nintendo 3DS/.../title/00040000 ("00030700")
// The save that was there is kept in Saves/<console>/Imported, then the folder used is moved there.
struct EmulatorSaveFolders {
    std::filesystem::path wiiTitles;   // Dolphin's Wii/title/00010000
    std::filesystem::path cemuSaves;   // Cemu's mlc01/usr/save/00050000
    std::filesystem::path n3dsTitles;  // Azahar's sdmc/Nintendo 3DS/<0..0>/<0..0>/title/00040000
};
struct FolderImport {
    std::vector<std::string> used;  // folder names used
    std::string problem;            // why one couldn't be used ("" = none)
};
// game: the game being started (its file name and ID are matched to folders named like it).
FolderImport importSaveFolders(const std::filesystem::path& dir, System console, const EmulatorSaveFolders& to,
                               const std::filesystem::path& game, const std::string& gameId);
