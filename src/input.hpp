#pragma once
#include <SDL.h>

#include <cstdint>
#include <string>
#include <vector>

#include "config.hpp"
#include "libretro.h"

// A controller input bound to a game button.
struct PadBind {
    enum Kind : uint8_t { None, Button, AxisPos, AxisNeg };
    Kind kind = None;
    uint8_t index = 0;  // SDL_GameControllerButton or SDL_GameControllerAxis
    bool operator==(const PadBind& o) const { return kind == o.kind && index == o.index; }
};

// Turns SDL keyboards + game controllers into libretro "RetroPad" input.
// Any controller SDL understands works, and controllers can be plugged in or
// unplugged at any time. By default player N uses the Nth connected controller
// plus its own keyboard keys; Button Config can give a player a specific
// controller (or none). Every button can be remapped (Options > Button Config).
class Input {
public:
    static constexpr int kMaxPlayers = 4;
    static constexpr int kButtons = 16;  // RetroPad button ids 0..15
    // Analog stick directions. Keys push the stick fully; controller inputs bound to them
    // (by default the real sticks) push it as far as they're tilted / pressed.
    // left stick = 3DS Circle Pad / N64 Control Stick, right stick = N64 C buttons / 3DS C-Stick.
    static constexpr int kStickUp = 16, kStickDown = 17, kStickLeft = 18, kStickRight = 19;
    static constexpr int kRStickUp = 20, kRStickDown = 21, kRStickLeft = 22, kRStickRight = 23;
    static constexpr int kBindable = 24;

    void init(const std::string& mappingDbPath);
    void shutdown();
    // Returns true if the event was a controller connect/disconnect.
    bool handleDeviceEvent(const SDL_Event& e, std::string* message);

    int16_t state(unsigned port, unsigned device, unsigned index, unsigned id) const;
    bool rumble(unsigned port, retro_rumble_effect effect, uint16_t strength);
    void stopAllRumble();

    void setKeyboardEnabled(bool on) { keyboardEnabled_ = on; }
    void setStickAsDpad(bool on) { stickAsDpad_ = on; }
    void setForcedButtons(uint16_t mask) { forced_ = mask; }  // test hook (--hold)

    // Connected controllers, in the order they were plugged in.
    struct ControllerInfo {
        SDL_JoystickID id;
        std::string name;  // "#2" added when two have the same name
        int player;        // player using it, or -1
    };
    std::vector<ControllerInfo> controllers() const;
    // Give `player` this controller (any player using it gets this player's old one).
    // id -1 = no controller (keyboard only).
    void assignController(int player, SDL_JoystickID id);
    bool keyboardOnly(int player) const { return choice_[player].kind == Choice::None; }
    // The player chose a controller that isn't plugged in right now.
    bool waitingForController(int player) const { return choice_[player].kind == Choice::Pad && slot_[player] < 0; }

    std::string controllerName(int port) const;
    SDL_GameController* controllerForPort(int port) const;
    // Motion sensors of a player's controller (Switch Pro, DualShock 4, DualSense...): switches the
    // gyro or accelerometer on; false if the controller has none.
    bool enableSensor(int port, bool gyro);
    // A reading in libretro's units and axes (accelerometer in g, gyro in radians per second;
    // X right, Y up, Z towards the player, which SDL uses too). 0 without a sensor.
    float sensor(int port, unsigned id) const;
    int portForController(SDL_JoystickID id) const;

    // Button mapping
    SDL_Scancode key(int player, int button) const { return maps_[player].keys[button]; }
    PadBind pad(int player, int button) const { return maps_[player].pad[button]; }
    void setKey(int player, int button, SDL_Scancode key);
    void setPad(int player, int button, PadBind bind);
    void resetDefaults(int player);
    bool hasKeyboardKeys(int player) const;  // any keyboard key mapped for this player
    void load(const Config& cfg);
    void save(Config& cfg) const;

    static const char* buttonLabel(int button);  // "Up", "A", "Start"...
    static bool isStickDirection(int button) { return button >= kStickUp && button <= kRStickRight; }
    static std::string keyName(SDL_Scancode key);
    static std::string padName(PadBind bind);
    static uint16_t parseButtons(const std::string& list);

private:
    struct Pad {
        SDL_GameController* gc = nullptr;
        SDL_JoystickID id = -1;
        std::string guid;  // same for every controller of one model; remembered between runs
        uint16_t rumbleStrong = 0, rumbleWeak = 0;
    };
    // Which controller a player wants: the default (next free one), a chosen one, or none.
    struct Choice {
        enum Kind : uint8_t { Auto, Pad, None };
        Kind kind = Auto;
        std::string guid;        // chosen model (to find it again after a restart / replug)
        SDL_JoystickID id = -1;  // the exact chosen controller while it stays plugged in
    };
    struct PlayerMap {
        SDL_Scancode keys[kBindable]{};
        PadBind pad[kBindable]{};
    };
    uint16_t buttonMask(unsigned port) const;
    int keyStick(unsigned port, unsigned stick, unsigned axis) const;
    static bool padActive(SDL_GameController* gc, PadBind b);
    static int padAmount(SDL_GameController* gc, PadBind b);  // 0..32767, how far it's pushed
    int padStick(unsigned port, unsigned stick, unsigned axis) const;
    void reassign();  // work out which controller each player uses
    const Pad* padForPort(int port) const;

    std::vector<Pad> pads_;
    Choice choice_[kMaxPlayers];
    int slot_[kMaxPlayers] = {-1, -1, -1, -1};  // index into pads_ per player, -1 = none
    PlayerMap maps_[kMaxPlayers];
    bool keyboardEnabled_ = true;
    bool stickAsDpad_ = true;
    uint16_t forced_ = 0;
};
