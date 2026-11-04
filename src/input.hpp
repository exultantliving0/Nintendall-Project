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
// unplugged at any time. Player N uses the Nth connected controller plus its
// own keyboard keys. Every button can be remapped (Options > Button Config).
class Input {
public:
    static constexpr int kMaxPlayers = 4;
    static constexpr int kButtons = 16;  // RetroPad button ids 0..15
    // Extra keyboard-only bindings that push the analog sticks:
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

    int controllerCount() const { return int(pads_.size()); }
    std::string controllerName(int port) const;
    SDL_GameController* controllerForPort(int port) const;
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
    // For stick directions: which real stick a controller uses ("Left stick" / "Right stick").
    static const char* stickName(int button) { return button >= kRStickUp ? "Right stick" : "Left stick"; }
    static std::string keyName(SDL_Scancode key);
    static std::string padName(PadBind bind);
    static uint16_t parseButtons(const std::string& list);

private:
    struct Pad {
        SDL_GameController* gc = nullptr;
        SDL_JoystickID id = -1;
        uint16_t rumbleStrong = 0, rumbleWeak = 0;
    };
    struct PlayerMap {
        SDL_Scancode keys[kBindable]{};
        PadBind pad[kBindable]{};  // stick directions (16-23) always use the real sticks
    };
    uint16_t buttonMask(unsigned port) const;
    int keyStick(unsigned port, unsigned stick, unsigned axis) const;
    static bool padActive(SDL_GameController* gc, PadBind b);

    std::vector<Pad> pads_;
    PlayerMap maps_[kMaxPlayers];
    bool keyboardEnabled_ = true;
    bool stickAsDpad_ = true;
    uint16_t forced_ = 0;
};
