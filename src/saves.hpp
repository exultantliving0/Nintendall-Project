#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "systems.hpp"

// The Saves folder (Documents/Nintendall/Saves, next to Games and Box Covers) has a folder per
// console, and in each of them a Saves folder (the games' own saves) and a States folder (save
// states). Battery saves (NES, SNES, N64, Game Boy, GBA, DS) are named like the game file:
// "Saves/SNES/Saves/Super Mario World (USA).srm" (Famicom Disk System disk saves: "<game>.ips" in
// Saves/NES/Saves). Saves from other emulators can be put there with the game's name
// ("Pokemon Emerald.sav"): the next time the game starts, Nintendall uses them. Save states:
// "Saves/SNES/States/Super Mario World (USA).state1" (slot 1).

// The files and folders in a folder (none if it can't be read). Unlike a loop over a
// directory_iterator, this never throws (an error thrown when a game closes would end Nintendall).
std::vector<std::filesystem::directory_entry> entriesIn(const std::filesystem::path& dir);

// Makes the Saves folder's console folders, each with its Saves and States folders.
void createSaveFolders(const std::filesystem::path& savesRoot);

// Files from older versions move into place: what was right in a console folder goes into its
// Saves folder, and the save states in `oldStates` (Nintendall's own data folder, named
// "<game>.<console key>.state<slot>") into the console's States folder. Nothing is written over:
// a file whose new place is taken stays where it was. Returns how many were moved.
int organizeSaveFolders(const std::filesystem::path& savesRoot, const std::filesystem::path& oldStates);

// A game's console folder: the folder the game is in inside the Games folder (so a .gbc game in
// Games/Game Boy goes in Saves/Game Boy), or else the console's own folder.
std::filesystem::path consoleFolderFor(const std::filesystem::path& savesRoot, const std::filesystem::path& gamesRoot,
                                       const std::filesystem::path& game, System system);
// Its Saves folder (the game's own saves) and States folder (save states).
std::filesystem::path savesFolderFor(const std::filesystem::path& savesRoot, const std::filesystem::path& gamesRoot,
                                     const std::filesystem::path& game, System system);
std::filesystem::path statesFolderFor(const std::filesystem::path& savesRoot, const std::filesystem::path& gamesRoot,
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

// Wii, Wii U and 3DS: the save files of each game are kept in Saves/<console>/Saves, one folder per game,
// laid out like the emulator's own save folder (and Dolphin's / Cemu's / Citra's, so their save
// folders can be put there as they are):
//   Wii    Saves/Wii/Saves/<game> [524d4345]/data/...      (Dolphin: Wii/title/00010000/<id>/data)
//   Wii U  Saves/Wii U/Saves/<game> [101c9400]/user/...    (Cemu: mlc01/usr/save/00050000/<id>)
//   3DS    Saves/Nintendo 3DS/Saves/00030700/data/...      (Citra: sdmc/.../title/00040000/<id>/data)
// A folder is named with the game's ID, and the game's name in front when Nintendall knows it.
// The emulator keeps working on its own copy: before a game starts and after it stops, the two
// are brought in step, each game's newer copy replacing the older one. Only save files go in
// Saves (no settings, caches or installed games).
struct SaveFolderSync {
    std::filesystem::path emulator;  // the emulator's folder of saves, one folder per game ID
    size_t idLength = 8;             // hex digits in a game's ID
    std::string inner;               // the part that is the save, inside a game's folder ("data", or "" for all)
    // More folders of saves (Wii: Dolphin's channels, title/00010001 and 00010004). A game's save
    // stays in whichever folder has it; a save only in the Saves folder goes to `emulator`, or to
    // `currentIn` if it's the current game's (a channel's save goes in its own folder).
    std::vector<std::filesystem::path> alsoIn;
    std::filesystem::path currentIn;
};
// currentId / currentName: the game being started or stopped, "" if unknown. names: game names by
// ID (the games in the Games folder). A save folder named with just the ID gets the game's name in
// front ("Mario Kart Wii [524d4345]"). Returns how many saves were copied each way.
struct SyncCount {
    int toSaves = 0, toEmulator = 0;
};
SyncCount syncSaveFolders(const std::filesystem::path& savesDir, const SaveFolderSync& emulator,
                          const std::string& currentId, const std::string& currentName,
                          const std::map<std::string, std::string>& names = {});
// The ID a game's saves have in the emulator (Wii: "524d4345" from RMCE01; Wii U: from the game
// folder's meta.xml), "" if it isn't known.
std::string saveFolderId(System console, const std::filesystem::path& game, const std::string& gameId);

// Save folders in Saves/<console>/Saves named another way than syncSaveFolders' are copied into the
// emulator when a game of that console starts (and then come back to Saves with the sync):
//   Wii:   a folder named like the game file, or with its ID ("RMCE01"), holding the save's files
//          (or Dolphin's "data" folder)
//   Wii U: a folder named like the game (folder games), or with the full title ID ("00050000101c9400")
//   3DS:   a folder named with the full title ID ("0004000000030700")
// The save that was there is kept in Saves/<console>/Saves/Imported, then the folder used is moved there.
struct EmulatorSaveFolders {
    std::filesystem::path wiiTitles;   // Dolphin's Wii/title/00010000
    std::filesystem::path cemuSaves;   // Cemu's mlc01/usr/save/00050000
    std::filesystem::path n3dsTitles;  // Azahar's sdmc/Nintendo 3DS/<0..0>/<0..0>/title/00040000
};
struct FolderImport {
    std::vector<std::string> used;  // folder names used
    std::string problem;            // why one couldn't be used ("" = none)
};
// Wii saves the way a Wii copies them to an SD card (Data Management: private/wii/title/<ID>/data.bin)
// and save sites offer them: a .bin file put in Saves/Wii/Saves (any name: "save.bin"), or a folder
// put there with a data.bin in it ("private", "RMCE"). The save is unpacked into the game's save
// folder there and into Dolphin's (`titles`: its Wii/title folder), replacing the save that was
// there, which is kept in Imported ("... (before)"); then the file (or folder) is moved to Imported.
// names: game names by ID, for naming a new save folder.
FolderImport importWiiDataBins(const std::filesystem::path& dir, const std::filesystem::path& titles,
                               const std::map<std::string, std::string>& names);

// game: the game being started (its file name and ID are matched to folders named like it).
FolderImport importSaveFolders(const std::filesystem::path& dir, System console, const EmulatorSaveFolders& to,
                               const std::filesystem::path& game, const std::string& gameId);
