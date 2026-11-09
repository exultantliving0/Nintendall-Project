#pragma once
#include <SDL.h>

#include <filesystem>
#include <string>
#include <vector>

#include "lan.hpp"
#include "systems.hpp"
#include "text.hpp"
#include "video.hpp"

// LAN > Host Game / Join Game: pick the game and type a name (and, to join, the host code).
class LanDialog {
public:
    enum class Kind { Host, Join };
    struct Game {
        std::filesystem::path path;
        std::string title;
        std::string gameId;
        System system = System::Unknown;
    };
    struct Request {
        Kind kind = Kind::Host;
        Game game;
        std::string name, target;
        int delay = 4;
    };

    void open(Kind kind, std::vector<Game> games, int selected, const std::string& name, const std::string& target,
              int delay);
    void close();
    bool isOpen() const { return open_; }
    // The player clicked Host / Join: what to start (true once).
    bool takeRequest(Request& out);

    // Modal: uses every event while open. Mouse coordinates in renderer pixels.
    void handleEvent(const SDL_Event& e, int mouseX, int mouseY);
    void draw(Video& v, Text& t, int width, int height);

private:
    struct Field {
        std::string text;
        SDL_Rect box{};
    };
    void confirm();
    void focusNext();
    Field* focusedField();

    Kind kind_ = Kind::Host;
    bool open_ = false;
    bool requested_ = false;
    std::vector<Game> games_;
    int selected_ = -1;
    int listScroll_ = 0;
    int delay_ = 4;
    Field target_, name_;
    int focus_ = 0;  // 0 = first text field, 1 = second (join) / the only one (host)
    std::string error_;
    int mouseX_ = 0, mouseY_ = 0;
    int rowH_ = 1, listRows_ = 6;
    SDL_Rect dialog_{}, list_{}, delayPrev_{}, delayNext_{}, delayBox_{}, okBtn_{}, cancelBtn_{};
};

// The lobby over the game while a LAN game waits for players (and when it couldn't start).
class LanLobby {
public:
    enum class Click { None, Start, Leave, Copy };
    struct Info {
        bool host = false;
        std::string gameTitle;
        std::string target;                  // joining: the code / address typed
        std::vector<std::string> addresses;  // hosting: this computer's addresses
        std::string errorText;               // shown instead of the lobby when set
    };
    Click handleEvent(const SDL_Event& e, int mouseX, int mouseY, const LanStatus& st, const Info& info);
    void draw(Video& v, Text& t, const SDL_Rect& area, int mouseX, int mouseY, const LanStatus& st, const Info& info);

private:
    SDL_Rect startBtn_{}, leaveBtn_{}, copyBtn_{};
    bool startEnabled_ = false;
};

// Splits text into lines no wider than `width`.
std::vector<std::string> wrapText(Text& t, const std::string& text, int width);
