#pragma once
#include <SDL.h>
#include <SDL_ttf.h>

#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include "video.hpp"

// Draws text with a real (TrueType) font. Rendered strings are cached as textures.
class Text {
public:
    enum Size { Normal, Large };

    bool init(SDL_Renderer* renderer, const std::vector<std::filesystem::path>& fontCandidates, std::string& error);
    void shutdown();
    // UI scale (1.0 = 96 DPI). Reloads the fonts at the new pixel size.
    void setScale(float scale);
    float scale() const { return scale_; }

    // Draws `s` with its top-left at (x, y). Returns the width drawn.
    int draw(int x, int y, const std::string& s, Color c, Size size = Normal);
    int width(const std::string& s, Size size = Normal);
    int lineHeight(Size size = Normal) const;

private:
    struct Entry {
        SDL_Texture* tex = nullptr;
        int w = 0, h = 0;
    };
    const Entry* get(const std::string& s, Size size);
    void clearCache();
    bool openFonts();

    SDL_Renderer* renderer_ = nullptr;
    std::filesystem::path fontPath_;
    TTF_Font* fonts_[2] = {nullptr, nullptr};
    float scale_ = 1.0f;
    std::unordered_map<std::string, Entry> cache_;
};
