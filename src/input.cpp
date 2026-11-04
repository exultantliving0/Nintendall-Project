#include "input.hpp"

#include <algorithm>
#include <cctype>

namespace {

// RetroPad button -> controller button, by physical position
// (bottom face button = RetroPad B, right = A, left = Y, top = X).
struct PadMap {
    unsigned retro;
    SDL_GameControllerButton button;
};
const PadMap kPadMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_B, SDL_CONTROLLER_BUTTON_A},
    {RETRO_DEVICE_ID_JOYPAD_A, SDL_CONTROLLER_BUTTON_B},
    {RETRO_DEVICE_ID_JOYPAD_Y, SDL_CONTROLLER_BUTTON_X},
    {RETRO_DEVICE_ID_JOYPAD_X, SDL_CONTROLLER_BUTTON_Y},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, SDL_CONTROLLER_BUTTON_BACK},
    {RETRO_DEVICE_ID_JOYPAD_START, SDL_CONTROLLER_BUTTON_START},
    {RETRO_DEVICE_ID_JOYPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP},
    {RETRO_DEVICE_ID_JOYPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
    {RETRO_DEVICE_ID_JOYPAD_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    {RETRO_DEVICE_ID_JOYPAD_L, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
    {RETRO_DEVICE_ID_JOYPAD_R, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
    {RETRO_DEVICE_ID_JOYPAD_L3, SDL_CONTROLLER_BUTTON_LEFTSTICK},
    {RETRO_DEVICE_ID_JOYPAD_R3, SDL_CONTROLLER_BUTTON_RIGHTSTICK},
};

// Keyboard defaults for player 1 (same layout as other popular emulators).
struct KeyMap {
    unsigned retro;
    SDL_Scancode key;
};
const KeyMap kKeyMap[] = {
    {RETRO_DEVICE_ID_JOYPAD_UP, SDL_SCANCODE_UP},       {RETRO_DEVICE_ID_JOYPAD_DOWN, SDL_SCANCODE_DOWN},
    {RETRO_DEVICE_ID_JOYPAD_LEFT, SDL_SCANCODE_LEFT},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, SDL_SCANCODE_RIGHT},
    {RETRO_DEVICE_ID_JOYPAD_A, SDL_SCANCODE_X},         {RETRO_DEVICE_ID_JOYPAD_B, SDL_SCANCODE_Z},
    {RETRO_DEVICE_ID_JOYPAD_X, SDL_SCANCODE_S},         {RETRO_DEVICE_ID_JOYPAD_Y, SDL_SCANCODE_A},
    {RETRO_DEVICE_ID_JOYPAD_L, SDL_SCANCODE_Q},         {RETRO_DEVICE_ID_JOYPAD_R, SDL_SCANCODE_W},
    {RETRO_DEVICE_ID_JOYPAD_L2, SDL_SCANCODE_1},        {RETRO_DEVICE_ID_JOYPAD_R2, SDL_SCANCODE_3},
    {RETRO_DEVICE_ID_JOYPAD_START, SDL_SCANCODE_RETURN}, {RETRO_DEVICE_ID_JOYPAD_SELECT, SDL_SCANCODE_RSHIFT},
    {RETRO_DEVICE_ID_JOYPAD_SELECT, SDL_SCANCODE_BACKSPACE},
};

constexpr int16_t kStickThreshold = 16000;
constexpr int16_t kTriggerThreshold = 12000;

}  // namespace

void Input::init(const std::string& mappingDbPath) {
    // Extra controller mappings (community gamecontrollerdb.txt) if the user provides one.
    if (!mappingDbPath.empty()) SDL_GameControllerAddMappingsFromFile(mappingDbPath.c_str());
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        SDL_Event e{};
        e.type = SDL_CONTROLLERDEVICEADDED;
        e.cdevice.which = i;
        handleDeviceEvent(e, nullptr);
    }
}

void Input::shutdown() {
    for (auto& p : pads_) SDL_GameControllerClose(p.gc);
    pads_.clear();
}

bool Input::handleDeviceEvent(const SDL_Event& e, std::string* message) {
    if (e.type == SDL_CONTROLLERDEVICEADDED) {
        if (!SDL_IsGameController(e.cdevice.which)) return true;
        const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(e.cdevice.which);
        for (const auto& p : pads_)
            if (p.id == id) return true;  // already open
        SDL_GameController* gc = SDL_GameControllerOpen(e.cdevice.which);
        if (!gc) return true;
        Pad p;
        p.gc = gc;
        p.id = id;
        pads_.push_back(p);
        if (message) {
            const char* n = SDL_GameControllerName(gc);
            *message = std::string(n ? n : "Controller") + " connected (Player " + std::to_string(pads_.size()) + ")";
        }
        return true;
    }
    if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
        auto it = std::find_if(pads_.begin(), pads_.end(), [&](const Pad& p) { return p.id == e.cdevice.which; });
        if (it != pads_.end()) {
            if (message) {
                const char* n = SDL_GameControllerName(it->gc);
                *message = std::string(n ? n : "Controller") + " disconnected";
            }
            SDL_GameControllerClose(it->gc);
            pads_.erase(it);
        }
        return true;
    }
    return false;
}

SDL_GameController* Input::controllerForPort(int port) const {
    return port >= 0 && port < int(pads_.size()) ? pads_[size_t(port)].gc : nullptr;
}

std::string Input::controllerName(int port) const {
    SDL_GameController* gc = controllerForPort(port);
    const char* n = gc ? SDL_GameControllerName(gc) : nullptr;
    return n ? n : "";
}

uint16_t Input::buttonMask(unsigned port) const {
    uint16_t mask = 0;
    if (port == 0) {
        mask |= forced_;
        if (keyboardEnabled_) {
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            for (const auto& k : kKeyMap)
                if (keys[k.key]) mask |= uint16_t(1u << k.retro);
        }
    }
    if (SDL_GameController* gc = controllerForPort(int(port))) {
        for (const auto& m : kPadMap)
            if (SDL_GameControllerGetButton(gc, m.button)) mask |= uint16_t(1u << m.retro);
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > kTriggerThreshold)
            mask |= 1u << RETRO_DEVICE_ID_JOYPAD_L2;
        if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > kTriggerThreshold)
            mask |= 1u << RETRO_DEVICE_ID_JOYPAD_R2;
        if (stickAsDpad_) {
            const int x = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
            const int y = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
            if (x < -kStickThreshold) mask |= 1u << RETRO_DEVICE_ID_JOYPAD_LEFT;
            if (x > kStickThreshold) mask |= 1u << RETRO_DEVICE_ID_JOYPAD_RIGHT;
            if (y < -kStickThreshold) mask |= 1u << RETRO_DEVICE_ID_JOYPAD_UP;
            if (y > kStickThreshold) mask |= 1u << RETRO_DEVICE_ID_JOYPAD_DOWN;
        }
    }
    return mask;
}

int16_t Input::state(unsigned port, unsigned device, unsigned index, unsigned id) const {
    if (port >= kMaxPlayers) return 0;
    switch (device & RETRO_DEVICE_MASK) {
    case RETRO_DEVICE_JOYPAD: {
        const uint16_t mask = buttonMask(port);
        if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return int16_t(mask);
        return id < 16 ? int16_t((mask >> id) & 1) : 0;
    }
    case RETRO_DEVICE_ANALOG: {
        SDL_GameController* gc = controllerForPort(int(port));
        if (!gc) return 0;
        auto clamp = [](int v) { return int16_t(std::max(-32767, std::min(32767, v))); };
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT)
            return clamp(SDL_GameControllerGetAxis(gc, id == RETRO_DEVICE_ID_ANALOG_X ? SDL_CONTROLLER_AXIS_LEFTX
                                                                                      : SDL_CONTROLLER_AXIS_LEFTY));
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT)
            return clamp(SDL_GameControllerGetAxis(gc, id == RETRO_DEVICE_ID_ANALOG_X ? SDL_CONTROLLER_AXIS_RIGHTX
                                                                                      : SDL_CONTROLLER_AXIS_RIGHTY));
        if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
            if (id == RETRO_DEVICE_ID_JOYPAD_L2) return clamp(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT));
            if (id == RETRO_DEVICE_ID_JOYPAD_R2) return clamp(SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT));
            return ((buttonMask(port) >> id) & 1) ? 32767 : 0;
        }
        return 0;
    }
    default:
        return 0;
    }
}

bool Input::rumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    if (port >= pads_.size()) return false;
    Pad& p = pads_[port];
    (effect == RETRO_RUMBLE_STRONG ? p.rumbleStrong : p.rumbleWeak) = strength;
    return SDL_GameControllerRumble(p.gc, p.rumbleStrong, p.rumbleWeak, (p.rumbleStrong || p.rumbleWeak) ? 5000 : 0) == 0;
}

void Input::stopAllRumble() {
    for (auto& p : pads_) {
        p.rumbleStrong = p.rumbleWeak = 0;
        SDL_GameControllerRumble(p.gc, 0, 0, 0);
    }
}

uint16_t Input::parseButtons(const std::string& list) {
    static const struct { const char* name; unsigned id; } names[] = {
        {"b", RETRO_DEVICE_ID_JOYPAD_B},       {"y", RETRO_DEVICE_ID_JOYPAD_Y},
        {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"start", RETRO_DEVICE_ID_JOYPAD_START},
        {"up", RETRO_DEVICE_ID_JOYPAD_UP},     {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
        {"left", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
        {"a", RETRO_DEVICE_ID_JOYPAD_A},       {"x", RETRO_DEVICE_ID_JOYPAD_X},
        {"l", RETRO_DEVICE_ID_JOYPAD_L},       {"r", RETRO_DEVICE_ID_JOYPAD_R},
    };
    uint16_t mask = 0;
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        std::string tok = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::transform(tok.begin(), tok.end(), tok.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (const auto& n : names)
            if (tok == n.name) mask |= uint16_t(1u << n.id);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return mask;
}
