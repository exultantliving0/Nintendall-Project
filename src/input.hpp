#pragma once
#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "libretro.h"

// Turns SDL keyboards + game controllers into libretro "RetroPad" input.
// Any controller SDL understands (Xbox, PlayStation, Switch, 8BitDo, generic
// USB pads...) works, and controllers can be plugged/unplugged at any time.
//
// Player 1 = keyboard + first controller, Player 2 = second controller, etc.
class Input {
public:
    static constexpr int kMaxPlayers = 4;

    void init(const std::string& mappingDbPath);
    void shutdown();
    // Returns true if the event was a controller connect/disconnect.
    bool handleDeviceEvent(const SDL_Event& e, std::string* message);

    int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    bool rumble(unsigned port, retro_rumble_effect effect, uint16_t strength);
    void stopAllRumble();

    void setKeyboardEnabled(bool on) { keyboardEnabled_ = on; }
    // Retro systems without analog sticks: let the left stick act as the D-pad.
    void setStickAsDpad(bool on) { stickAsDpad_ = on; }
    // Test hook: buttons forced down on player 1 (RetroPad bitmask).
    void setForcedButtons(uint16_t mask) { forced_ = mask; }

    int controllerCount() const { return int(pads_.size()); }
    std::string controllerName(int port) const;
    SDL_GameController* controllerForPort(int port) const;
    // Parse "a,b,start" into a RetroPad bitmask (used by --hold).
    static uint16_t parseButtons(const std::string& list);

private:
    struct Pad {
        SDL_GameController* gc = nullptr;
        SDL_JoystickID id = -1;
        uint16_t rumbleStrong = 0, rumbleWeak = 0;
    };
    uint16_t buttonMask(unsigned port) const;

    std::vector<Pad> pads_;
    bool keyboardEnabled_ = true;
    bool stickAsDpad_ = true;
    uint16_t forced_ = 0;
};
