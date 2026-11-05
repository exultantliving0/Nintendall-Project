#include "paths.hpp"

#include <SDL.h>

namespace fs = std::filesystem;

std::string coreFileName(const std::string& coreName) {
#if defined(_WIN32)
    return coreName + "_libretro.dll";
#elif defined(__APPLE__)
    return coreName + "_libretro.dylib";
#else
    return coreName + "_libretro.so";
#endif
}

void Paths::init() {
    if (char* b = SDL_GetBasePath()) {
        base = fs::u8path(b);
        SDL_free(b);
    } else {
        base = fs::current_path();
    }

    // Portable mode: drop a "portable.txt" next to the program to keep all data beside it.
    std::error_code ec;
    if (fs::exists(base / "portable.txt", ec)) {
        portable = true;
        data = base;
    } else if (char* p = SDL_GetPrefPath("", "Nintendall")) {
        data = fs::u8path(p);
        SDL_free(p);
    } else {
        data = base;
    }

    saves = data / "saves";
    states = data / "states";
    system = data / "system";
    config = data / "config";
    screenshots = data / "screenshots";
    cache = data / "cache";
    for (const auto& d : {saves, states, system, config, screenshots, cache}) fs::create_directories(d, ec);

    // The emulators ship in cores/ next to the program. The folder above is also
    // checked so multi-config builds (build/Release/nintendall) find build/cores.
    fs::path exeDir = base;
    if (!exeDir.has_filename()) exeDir = exeDir.parent_path();
    coreDirs = {base / "cores", exeDir.parent_path() / "cores"};
    for (const auto& d : {base / "system", exeDir.parent_path() / "system"})
        if (fs::is_directory(d / "dolphin-emu", ec)) {
            bundledSystem = d;
            break;
        }
}

fs::path Paths::systemDirFor(const std::string& coreName) const {
    // Dolphin reads its Sys folder from <system>/dolphin-emu/Sys; we ship that folder.
    if (coreName == "dolphin" && !bundledSystem.empty()) return bundledSystem;
    return system;
}

fs::path Paths::findCore(const std::string& coreName) const {
    std::error_code ec;
    for (const auto& dir : coreDirs) {
        fs::path p = dir / coreFileName(coreName);
        if (fs::is_regular_file(p, ec)) return p;
    }
    return {};
}
