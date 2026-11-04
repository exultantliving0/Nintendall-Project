// Nintendall - a multi-system game player built on libretro cores.
//
// Usage:
//   nintendall                      open the game library
//   nintendall <game file>          detect the system and play it
//   nintendall <folder>             open the library in that folder
//   nintendall --detect <files...>  print what system each file is for
//
// Options:
//   --fullscreen         start fullscreen
//   --frames <n>         run n frames without a window, then exit (testing)
//   --screenshot <file>  with --frames: save the last frame as PNG
//   --hold <buttons>     with --frames: hold buttons on player 1, e.g. a,start

#include <SDL.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "app.hpp"
#include "detect.hpp"
#include "paths.hpp"
#include "systems.hpp"

namespace fs = std::filesystem;

static int detectOnly(const std::vector<std::string>& files) {
    Paths paths;
    paths.init();
    int unknown = 0;
    for (const auto& f : files) {
        const Detection d = detectGame(fs::u8path(f));
        const SystemInfo& si = systemInfo(d.system);
        std::string status = "-";
        if (d.system != System::Unknown) {
            status = std::string(si.emulator) +
                     (si.needsGpu ? " (coming soon)" : paths.findCore(si.core).empty() ? " (MISSING)" : "");
        } else {
            ++unknown;
        }
        std::printf("%-34s %-5s %-9s %-30s %s\n", fs::u8path(f).filename().u8string().c_str(),
                    d.system == System::Unknown ? "?" : si.name, d.fromHeader ? "[header]" : "[ext]", status.c_str(),
                    d.detail.c_str());
    }
    return unknown ? 2 : 0;
}

static void usage() {
    std::puts(
        "Nintendall " NINTENDALL_VERSION "\n"
        "usage: nintendall [game-or-folder] [--fullscreen]\n"
        "       nintendall --detect FILE...\n"
        "       nintendall GAME --frames N [--screenshot out.png] [--hold a,start]");
}

int main(int argc, char** argv) {
    LaunchOptions opt;
    std::string start;
    std::vector<std::string> detectFiles;
    bool detect = false;

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else if (a == "--detect") {
            detect = true;
        } else if (a == "--fullscreen") {
            opt.fullscreen = true;
        } else if (a == "--frames") {
            opt.frames = std::atoi(next().c_str());
            opt.headless = true;
        } else if (a == "--screenshot") {
            opt.screenshot = next();
        } else if (a == "--hold") {
            opt.holdButtons = Input::parseButtons(next());
        } else if (detect) {
            detectFiles.push_back(a);
        } else {
            start = a;
        }
    }

    if (detect) return detectOnly(detectFiles);
    if (opt.headless && start.empty()) {
        std::fprintf(stderr, "--frames needs a game file\n");
        return 1;
    }

    // Position-based controller layout: the bottom face button is always "B" on the
    // RetroPad, whatever is printed on it (Xbox A, PlayStation Cross, Nintendo B).
    SDL_SetHint(SDL_HINT_GAMECONTROLLER_USE_BUTTON_LABELS, "0");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    const Uint32 subsystems = opt.headless ? 0 : (SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    if (SDL_Init(subsystems) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_EventState(SDL_DROPFILE, SDL_ENABLE);

    App app;
    const int rc = app.run(start.empty() ? fs::path() : fs::u8path(start), opt);
    SDL_Quit();
    return rc;
}
