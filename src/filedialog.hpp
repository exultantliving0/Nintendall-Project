#pragma once
#include <SDL.h>

#include <string>

// Shows the operating system's normal "Open file" window.
// Returns the chosen path (UTF-8), or "" if cancelled. Sets `unavailable` when
// this system has no dialog tool (Linux without zenity/kdialog).
std::string openGameDialog(SDL_Window* parent, bool& unavailable);
