#pragma once
#include <filesystem>
#include <string>

// What Nintendall was doing, kept in a small file (last-run.txt in the user data folder) so that if
// it closes unexpectedly, the next start knows: that file is then kept as crash.txt. On Windows a
// crash also writes where it happened (the program part and the calls that led there).

// Starts the log for this run; returns what the previous run was doing when it closed
// unexpectedly ("" if it closed normally or there was no previous run).
std::string crashLogStart(const std::filesystem::path& dataDir);
// What Nintendall is doing now ("closing the GameCube game: stopping the emulator").
void crashStep(const std::string& what);
// Nintendall is closing normally.
void crashLogEnd();
