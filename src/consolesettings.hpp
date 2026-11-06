#pragma once
#include <string>
#include <utility>
#include <vector>

#include "systems.hpp"

// The hand-picked emulator settings shown in Options > Console Settings.
// Each setting maps friendly choices onto one or more of the core's own options.

struct SettingChoice {
    std::string label;
    std::vector<std::pair<std::string, std::string>> set;  // core option key -> value
};

struct Setting {
    std::string label;
    std::vector<std::string> keys;       // core options this setting changes
    std::vector<SettingChoice> choices;  // empty = list the core's own values for keys[0]
    bool restart = false;                // only takes effect after restarting the game
    std::string hint;                    // short extra explanation (may be empty)
};

struct SettingGroup {
    std::string title;
    std::vector<Setting> settings;
};

const std::vector<SettingGroup>& consoleSettings(System s);

// Nintendall's own option (kept in the core's options file): draw with "opengl" or "vulkan".
// Offered for GameCube / Wii, 3DS and Wii U.
inline constexpr const char* kGraphicsApiOption = "nintendall_graphics_api";
bool canUseVulkan(System s);

// Friendly label for a raw core option value ("enabled" -> "On", ...).
std::string prettyValue(const std::string& value);
