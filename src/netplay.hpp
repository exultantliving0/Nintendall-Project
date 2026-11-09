#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

// Nintendall's own netplay, for the consoles whose emulators can save and load their state
// instantly (NES, SNES, N64), the way RetroArch does it:
//
// - Every computer runs the game; only button presses go over the network (UDP, port 2627).
//   Friends join with the host's address, on the same network or a virtual LAN.
// - When the host starts, it sends its save state, so everyone starts from the same moment (and
//   with the host's save data).
// - Each player's presses are used `delay` frames after they happen. A friend's press that comes
//   in later than that is guessed (the last one seen); when it arrives and differs, the game goes
//   back to that frame (a save state taken every frame) and plays the frames again with the right
//   presses: "rollback". With a rollback window of 0 (N64, whose save states are big) the game
//   instead waits for everyone's presses, like Dolphin's netplay.
// - The host passes everyone's presses on to everyone else.

constexpr unsigned kNetplayPort = 2627;
constexpr int kNetplayMaxPlayers = 4;

// One player's controller for one frame.
struct NetInput {
    uint16_t buttons = 0;          // RETRO_DEVICE_ID_JOYPAD_* bits
    std::array<int16_t, 4> axes{};  // left x, left y, right x, right y
    bool operator==(const NetInput& o) const { return buttons == o.buttons && axes == o.axes; }
    bool operator!=(const NetInput& o) const { return !(*this == o); }
};
using NetInputs = std::array<NetInput, kNetplayMaxPlayers>;

class Netplay {
public:
    enum class Phase { Connecting, Lobby, Syncing, Playing, Ended, Error };

    struct GameInfo {
        int system = 0;       // System, as a number
        uint32_t crc = 0;     // CRC-32 of the game file
        std::string title;
        std::vector<std::pair<std::string, std::string>> options;  // the host's emulator options
        int window = 8;       // rollback frames (0 = wait for everyone)
        int delay = 2;        // input delay frames
    };

    // What the frame engine needs from the app.
    struct Callbacks {
        std::function<NetInput()> sampleLocal;                     // this computer's controller now
        std::function<std::vector<uint8_t>()> save;                 // save state
        std::function<bool(const std::vector<uint8_t>&)> load;     // load state
        std::function<void(const NetInputs&, bool replay)> run;    // run one frame with these presses
    };

    Netplay();
    ~Netplay();
    Netplay(const Netplay&) = delete;
    Netplay& operator=(const Netplay&) = delete;

    // Host: listen for players. False (with `error`) if the port can't be used.
    bool host(const std::string& name, const GameInfo& game, std::string& error);
    // Join "address" or "address:port". The game arrives with the host's welcome (see game()).
    bool join(const std::string& target, const std::string& name, std::string& error);

    // Network: call every main-loop iteration.
    void poll();

    Phase phase() const { return phase_; }
    bool isHost() const { return host_; }
    const std::string& error() const { return error_; }
    std::vector<std::string> takeMessages();
    // Player names by player number ("" = free); player 0 is the host.
    const std::vector<std::string>& players() const { return names_; }
    int localPlayer() const { return local_; }
    int playerCount() const;
    const GameInfo& game() const { return game_; }  // joining: valid once phase() >= Lobby

    // Host, in the lobby: start with this save state (sent to everyone).
    void start(const std::vector<uint8_t>& state);
    // Joining: the host's save state, once it has all arrived (true once). Load it, then call
    // stateLoaded().
    bool takeState(std::vector<uint8_t>& state);
    void stateLoaded();

    // Playing: runs the frames that are due (0 while waiting for other players, sometimes 2 to
    // catch up), with rollbacks when guessed presses turn out wrong. Returns frames run.
    int advance(const Callbacks& cb);
    int frame() const { return frame_; }

    // Leaving: tells the others.
    void leave();

private:
    struct Impl;
    std::unique_ptr<Impl> net_;

    // Engine
    NetInputs inputsFor(int frame) const;
    void receiveInputs(int player, int first, const std::vector<NetInput>& list);
    void sendInputs();
    void checkSync(const Callbacks& cb);
    void recordCheck(int frame, uint32_t crc);
    void handle(const uint8_t* data, size_t size, const void* from);
    void fail(const std::string& message);
    void end(const std::string& message);

    bool host_ = false;
    Phase phase_ = Phase::Connecting;
    std::string error_;
    std::vector<std::string> messages_;
    std::vector<std::string> names_;
    int local_ = 0;
    GameInfo game_;
    std::string name_;

    // Frames and presses
    int frame_ = 0;
    std::map<int, NetInputs> known_;               // presses known for sure, by frame
    std::array<int, kNetplayMaxPlayers> lastKnown_{};  // last frame known for each player
    std::map<int, NetInputs> used_;                // presses each frame was run with
    int rollbackFrom_ = 0x7fffffff;
    std::vector<std::vector<uint8_t>> ring_;       // save state at the start of recent frames
    std::vector<int> ringFrame_;
    int remoteFrame_ = 0;                          // the furthest frame another player has reached
    std::map<int, uint32_t> checks_;               // host: its own state checksums, by frame
    int lastCheck_ = 0;
    int rollbacks_ = 0;  // (for the log)
    bool desyncShown_ = false;
};

// CRC-32 of a game file (inside a .zip: of the game in it). 0 if it can't be read.
uint32_t netplayGameCrc(const std::string& pathUtf8);
