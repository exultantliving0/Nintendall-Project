#include "input.hpp"

#include <algorithm>
#include <cctype>

namespace {

// Names used in the config file, indexed by RetroPad button id.
const char* const kIds[Input::kBindable] = {"b",  "y",  "select", "start", "up", "down", "left", "right",
                                            "a",  "x",  "l",      "r",     "l2", "r2",   "l3",   "r3",
                                            "stick_up", "stick_down", "stick_left", "stick_right",
                                            "rstick_up", "rstick_down", "rstick_left", "rstick_right"};
const char* const kLabels[Input::kBindable] = {"B",  "Y",  "Select", "Start", "Up", "Down", "Left", "Right",
                                               "A",  "X",  "L",      "R",     "L2", "R2",   "L3",   "R3",
                                               "Stick Up", "Stick Down", "Stick Left", "Stick Right",
                                               "Right Stick Up", "Right Stick Down", "Right Stick Left", "Right Stick Right"};

constexpr int16_t kStickThreshold = 16000;
constexpr int16_t kTriggerThreshold = 12000;

PadBind btn(SDL_GameControllerButton b) { return {PadBind::Button, uint8_t(b)}; }

}  // namespace

// ---------------------------------------------------------------------------
// Mapping

void Input::resetDefaults(int player) {
    PlayerMap& m = maps_[player];
    for (auto& k : m.keys) k = SDL_SCANCODE_UNKNOWN;
    if (player == 0) {
        m.keys[RETRO_DEVICE_ID_JOYPAD_UP] = SDL_SCANCODE_UP;
        m.keys[RETRO_DEVICE_ID_JOYPAD_DOWN] = SDL_SCANCODE_DOWN;
        m.keys[RETRO_DEVICE_ID_JOYPAD_LEFT] = SDL_SCANCODE_LEFT;
        m.keys[RETRO_DEVICE_ID_JOYPAD_RIGHT] = SDL_SCANCODE_RIGHT;
        m.keys[RETRO_DEVICE_ID_JOYPAD_A] = SDL_SCANCODE_X;
        m.keys[RETRO_DEVICE_ID_JOYPAD_B] = SDL_SCANCODE_Z;
        m.keys[RETRO_DEVICE_ID_JOYPAD_X] = SDL_SCANCODE_S;
        m.keys[RETRO_DEVICE_ID_JOYPAD_Y] = SDL_SCANCODE_A;
        m.keys[RETRO_DEVICE_ID_JOYPAD_L] = SDL_SCANCODE_Q;
        m.keys[RETRO_DEVICE_ID_JOYPAD_R] = SDL_SCANCODE_W;
        m.keys[RETRO_DEVICE_ID_JOYPAD_L2] = SDL_SCANCODE_1;
        m.keys[RETRO_DEVICE_ID_JOYPAD_R2] = SDL_SCANCODE_3;
        m.keys[RETRO_DEVICE_ID_JOYPAD_START] = SDL_SCANCODE_RETURN;
        m.keys[RETRO_DEVICE_ID_JOYPAD_SELECT] = SDL_SCANCODE_RSHIFT;
        m.keys[kStickUp] = SDL_SCANCODE_I;
        m.keys[kStickDown] = SDL_SCANCODE_K;
        m.keys[kStickLeft] = SDL_SCANCODE_J;
        m.keys[kStickRight] = SDL_SCANCODE_L;
        m.keys[kRStickUp] = SDL_SCANCODE_T;
        m.keys[kRStickDown] = SDL_SCANCODE_G;
        m.keys[kRStickLeft] = SDL_SCANCODE_F;
        m.keys[kRStickRight] = SDL_SCANCODE_H;
    }
    // Controllers: by physical position (bottom face button = RetroPad B, right = A...).
    m.pad[RETRO_DEVICE_ID_JOYPAD_B] = btn(SDL_CONTROLLER_BUTTON_A);
    m.pad[RETRO_DEVICE_ID_JOYPAD_A] = btn(SDL_CONTROLLER_BUTTON_B);
    m.pad[RETRO_DEVICE_ID_JOYPAD_Y] = btn(SDL_CONTROLLER_BUTTON_X);
    m.pad[RETRO_DEVICE_ID_JOYPAD_X] = btn(SDL_CONTROLLER_BUTTON_Y);
    m.pad[RETRO_DEVICE_ID_JOYPAD_SELECT] = btn(SDL_CONTROLLER_BUTTON_BACK);
    m.pad[RETRO_DEVICE_ID_JOYPAD_START] = btn(SDL_CONTROLLER_BUTTON_START);
    m.pad[RETRO_DEVICE_ID_JOYPAD_UP] = btn(SDL_CONTROLLER_BUTTON_DPAD_UP);
    m.pad[RETRO_DEVICE_ID_JOYPAD_DOWN] = btn(SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    m.pad[RETRO_DEVICE_ID_JOYPAD_LEFT] = btn(SDL_CONTROLLER_BUTTON_DPAD_LEFT);
    m.pad[RETRO_DEVICE_ID_JOYPAD_RIGHT] = btn(SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    m.pad[RETRO_DEVICE_ID_JOYPAD_L] = btn(SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
    m.pad[RETRO_DEVICE_ID_JOYPAD_R] = btn(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    m.pad[RETRO_DEVICE_ID_JOYPAD_L2] = {PadBind::AxisPos, uint8_t(SDL_CONTROLLER_AXIS_TRIGGERLEFT)};
    m.pad[RETRO_DEVICE_ID_JOYPAD_R2] = {PadBind::AxisPos, uint8_t(SDL_CONTROLLER_AXIS_TRIGGERRIGHT)};
    m.pad[RETRO_DEVICE_ID_JOYPAD_L3] = btn(SDL_CONTROLLER_BUTTON_LEFTSTICK);
    m.pad[RETRO_DEVICE_ID_JOYPAD_R3] = btn(SDL_CONTROLLER_BUTTON_RIGHTSTICK);
}

bool Input::hasKeyboardKeys(int player) const {
    for (auto k : maps_[player].keys)
        if (k != SDL_SCANCODE_UNKNOWN) return true;
    return false;
}

void Input::setKey(int player, int button, SDL_Scancode key) {
    PlayerMap& m = maps_[player];
    if (key != SDL_SCANCODE_UNKNOWN)
        for (auto& k : m.keys)
            if (k == key) k = SDL_SCANCODE_UNKNOWN;  // one key can't drive two buttons
    m.keys[button] = key;
}

void Input::setPad(int player, int button, PadBind bind) {
    PlayerMap& m = maps_[player];
    if (bind.kind != PadBind::None)
        for (auto& p : m.pad)
            if (p == bind) p = PadBind{};
    m.pad[button] = bind;
}

static std::string padToString(PadBind b) {
    switch (b.kind) {
    case PadBind::Button: return std::string("button:") + SDL_GameControllerGetStringForButton(SDL_GameControllerButton(b.index));
    case PadBind::AxisPos: return std::string("axis+:") + SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis(b.index));
    case PadBind::AxisNeg: return std::string("axis-:") + SDL_GameControllerGetStringForAxis(SDL_GameControllerAxis(b.index));
    default: return "none";
    }
}

static PadBind padFromString(const std::string& s, PadBind fallback) {
    if (s == "none") return {};
    const auto colon = s.find(':');
    if (colon == std::string::npos) return fallback;
    const std::string kind = s.substr(0, colon), name = s.substr(colon + 1);
    if (kind == "button") {
        const auto b = SDL_GameControllerGetButtonFromString(name.c_str());
        if (b != SDL_CONTROLLER_BUTTON_INVALID) return {PadBind::Button, uint8_t(b)};
    } else if (kind == "axis+" || kind == "axis-") {
        const auto a = SDL_GameControllerGetAxisFromString(name.c_str());
        if (a != SDL_CONTROLLER_AXIS_INVALID) return {kind == "axis+" ? PadBind::AxisPos : PadBind::AxisNeg, uint8_t(a)};
    }
    return fallback;
}

void Input::load(const Config& cfg) {
    for (int p = 0; p < kMaxPlayers; ++p) {
        resetDefaults(p);
        const std::string prefix = "p" + std::to_string(p + 1) + ".";
        for (int b = 0; b < kBindable; ++b) {
            const std::string k = cfg.get(prefix + "key." + kIds[b]);
            if (!k.empty()) maps_[p].keys[b] = k == "none" ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(k.c_str());
            const std::string pd = cfg.get(prefix + "pad." + kIds[b]);
            if (!pd.empty()) maps_[p].pad[b] = padFromString(pd, maps_[p].pad[b]);
        }
    }
}

void Input::save(Config& cfg) const {
    for (int p = 0; p < kMaxPlayers; ++p) {
        const std::string prefix = "p" + std::to_string(p + 1) + ".";
        for (int b = 0; b < kBindable; ++b) {
            const SDL_Scancode k = maps_[p].keys[b];
            cfg.set(prefix + "key." + kIds[b], k == SDL_SCANCODE_UNKNOWN ? "none" : SDL_GetScancodeName(k));
            if (!isStickDirection(b)) cfg.set(prefix + "pad." + kIds[b], padToString(maps_[p].pad[b]));
        }
    }
}

const char* Input::buttonLabel(int button) { return button >= 0 && button < kBindable ? kLabels[button] : "?"; }

std::string Input::keyName(SDL_Scancode key) {
    if (key == SDL_SCANCODE_UNKNOWN) return "";
    const char* n = SDL_GetScancodeName(key);
    return n && *n ? n : "Key " + std::to_string(int(key));
}

std::string Input::padName(PadBind b) {
    if (b.kind == PadBind::Button) {
        switch (SDL_GameControllerButton(b.index)) {
        case SDL_CONTROLLER_BUTTON_A: return "Bottom button";
        case SDL_CONTROLLER_BUTTON_B: return "Right button";
        case SDL_CONTROLLER_BUTTON_X: return "Left button";
        case SDL_CONTROLLER_BUTTON_Y: return "Top button";
        case SDL_CONTROLLER_BUTTON_BACK: return "Select / Back";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Home / Guide";
        case SDL_CONTROLLER_BUTTON_START: return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left stick click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right stick click";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "Left bumper";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "Right bumper";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-pad Right";
        default: return std::string("Button ") + std::to_string(int(b.index));
        }
    }
    if (b.kind == PadBind::AxisPos || b.kind == PadBind::AxisNeg) {
        const bool pos = b.kind == PadBind::AxisPos;
        switch (SDL_GameControllerAxis(b.index)) {
        case SDL_CONTROLLER_AXIS_TRIGGERLEFT: return "Left trigger";
        case SDL_CONTROLLER_AXIS_TRIGGERRIGHT: return "Right trigger";
        case SDL_CONTROLLER_AXIS_LEFTX: return pos ? "Left stick Right" : "Left stick Left";
        case SDL_CONTROLLER_AXIS_LEFTY: return pos ? "Left stick Down" : "Left stick Up";
        case SDL_CONTROLLER_AXIS_RIGHTX: return pos ? "Right stick Right" : "Right stick Left";
        case SDL_CONTROLLER_AXIS_RIGHTY: return pos ? "Right stick Down" : "Right stick Up";
        default: return "Axis";
        }
    }
    return "";
}

// ---------------------------------------------------------------------------
// Devices

void Input::init(const std::string& mappingDbPath) {
    for (int p = 0; p < kMaxPlayers; ++p) resetDefaults(p);
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

int Input::portForController(SDL_JoystickID id) const {
    for (size_t i = 0; i < pads_.size(); ++i)
        if (pads_[i].id == id) return int(i);
    return -1;
}

std::string Input::controllerName(int port) const {
    SDL_GameController* gc = controllerForPort(port);
    const char* n = gc ? SDL_GameControllerName(gc) : nullptr;
    return n ? n : "";
}

// ---------------------------------------------------------------------------
// Game input

bool Input::padActive(SDL_GameController* gc, PadBind b) {
    switch (b.kind) {
    case PadBind::Button: return SDL_GameControllerGetButton(gc, SDL_GameControllerButton(b.index));
    case PadBind::AxisPos: {
        const auto axis = SDL_GameControllerAxis(b.index);
        const bool trigger = axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
        return SDL_GameControllerGetAxis(gc, axis) > (trigger ? kTriggerThreshold : kStickThreshold);
    }
    case PadBind::AxisNeg: return SDL_GameControllerGetAxis(gc, SDL_GameControllerAxis(b.index)) < -kStickThreshold;
    default: return false;
    }
}

uint16_t Input::buttonMask(unsigned port) const {
    uint16_t mask = port == 0 ? forced_ : 0;
    const PlayerMap& m = maps_[port];
    if (keyboardEnabled_) {
        const Uint8* keys = SDL_GetKeyboardState(nullptr);
        for (int b = 0; b < kButtons; ++b)
            if (m.keys[b] != SDL_SCANCODE_UNKNOWN && keys[m.keys[b]]) mask |= uint16_t(1u << b);
    }
    if (SDL_GameController* gc = controllerForPort(int(port))) {
        for (int b = 0; b < kButtons; ++b)
            if (padActive(gc, m.pad[b])) mask |= uint16_t(1u << b);
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
        if (!gc)
            return index == RETRO_DEVICE_INDEX_ANALOG_LEFT || index == RETRO_DEVICE_INDEX_ANALOG_RIGHT
                       ? int16_t(keyStick(port, index, id))
                       : int16_t(0);
        auto clamp = [](int v) { return int16_t(std::max(-32767, std::min(32767, v))); };
        if (index == RETRO_DEVICE_INDEX_ANALOG_LEFT) {
            const int v = keyStick(port, index, id);
            if (v != 0) return int16_t(v);
            return clamp(SDL_GameControllerGetAxis(gc, id == RETRO_DEVICE_ID_ANALOG_X ? SDL_CONTROLLER_AXIS_LEFTX
                                                                                      : SDL_CONTROLLER_AXIS_LEFTY));
        }
        if (index == RETRO_DEVICE_INDEX_ANALOG_RIGHT) {
            const int v = keyStick(port, index, id);
            if (v != 0) return int16_t(v);
            return clamp(SDL_GameControllerGetAxis(gc, id == RETRO_DEVICE_ID_ANALOG_X ? SDL_CONTROLLER_AXIS_RIGHTX
                                                                                      : SDL_CONTROLLER_AXIS_RIGHTY));
        }
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

// A stick pushed by keyboard keys (full tilt), or 0 if none are held.
int Input::keyStick(unsigned port, unsigned stick, unsigned axis) const {
    if (!keyboardEnabled_) return 0;
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    const PlayerMap& m = maps_[port];
    auto held = [&](int b) { return m.keys[b] != SDL_SCANCODE_UNKNOWN && keys[m.keys[b]]; };
    const int base = stick == RETRO_DEVICE_INDEX_ANALOG_RIGHT ? kRStickUp : kStickUp;  // up, down, left, right
    if (axis == RETRO_DEVICE_ID_ANALOG_X) return (held(base + 3) ? 32767 : 0) - (held(base + 2) ? 32767 : 0);
    return (held(base + 1) ? 32767 : 0) - (held(base) ? 32767 : 0);
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
    uint16_t mask = 0;
    size_t start = 0;
    while (start <= list.size()) {
        const size_t comma = list.find(',', start);
        std::string tok = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
        std::transform(tok.begin(), tok.end(), tok.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        for (int b = 0; b < kButtons; ++b)
            if (tok == kIds[b]) mask |= uint16_t(1u << b);
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return mask;
}
