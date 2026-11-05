#pragma once
#include <SDL.h>

#include <string>
#include <vector>

// Shows the operating system's normal "Open file" window.
// Returns the chosen path (UTF-8), or "" if cancelled. Sets `unavailable` when
// this system has no dialog tool (Linux without zenity/kdialog).
std::string openGameDialog(SDL_Window* parent, bool& unavailable);
// Same, for any kind of file: `kind` names the filter ("Key files"), `exts` lists extensions without dots.
std::string openFileDialog(SDL_Window* parent, const std::string& title, const std::string& kind,
                           const std::vector<std::string>& exts, bool& unavailable);
