#pragma once
#include <string>
#include <vector>

// LAN games: GameCube and Wii games played together on several computers, through Dolphin's own
// netplay. Nintendall's Dolphin (third_party/dolphin) exports the functions below; the host gets a
// host code from Dolphin's server, so friends can join over the internet without opening ports,
// and on the same network they can join with the host's address.

constexpr unsigned kLanPort = 2626;  // Dolphin's netplay port

class LibretroCore;

struct LanApi {
    void (*setup)(int mode, const char* target, unsigned port, const char* name, unsigned delay) = nullptr;
    void (*start)() = nullptr;
    void (*setDelay)(unsigned frames) = nullptr;
    int (*status)(char* text, unsigned size) = nullptr;
    bool ok() const { return setup && start && setDelay && status; }
};
// The core's LAN functions (all null if it doesn't have them).
LanApi lanApi(const LibretroCore& core);

enum class LanState { Off = 0, Connecting, Lobby, Starting, Playing, Ended, Error };

struct LanStatus {
    LanState state = LanState::Off;
    unsigned delay = 4;
    std::string code;        // the host code, once Dolphin's server has given one
    std::string codeState;   // "connecting", "ready" or "failed"
    std::string hostName;    // the host player
    std::vector<std::string> players;  // everyone else
    std::string error, errorKind;      // errorKind: "wronggame", "gamecopy", "connect", "port"
    std::string hostGameId, hostGame;  // the game the host picked
    std::string progress;              // e.g. "Sharing save data..."
    std::vector<std::string> messages; // "Alex joined", ... (new since the last call)
};
LanStatus readLanStatus(const LanApi& api);

// This computer's IPv4 addresses on its networks (not loopback), best first.
std::vector<std::string> localAddresses();
// The name to show other players by default (the computer user's name).
std::string defaultPlayerName();
