#include "text.hpp"

#include <cmath>

namespace {
constexpr int kBasePixels[2] = {13, 20};  // Normal, Large at 96 DPI
constexpr size_t kMaxCache = 512;
}  // namespace

bool Text::init(SDL_Renderer* renderer, const std::vector<std::filesystem::path>& fontCandidates, std::string& error) {
    renderer_ = renderer;
    if (TTF_Init() != 0) {
        error = std::string("Couldn't start the font system: ") + TTF_GetError();
        return false;
    }
    std::error_code ec;
    for (const auto& p : fontCandidates) {
        if (!std::filesystem::is_regular_file(p, ec)) continue;
        fontPath_ = p;
        if (openFonts()) return true;
    }
    error = "Nintendall couldn't find its font (fonts/DejaVuSans.ttf). Reinstalling should fix this.";
    return false;
}

void Text::shutdown() {
    clearCache();
    for (auto*& f : fonts_) {
        if (f) TTF_CloseFont(f);
        f = nullptr;
    }
    if (TTF_WasInit()) TTF_Quit();
}

bool Text::openFonts() {
    for (int i = 0; i < 2; ++i) {
        if (fonts_[i]) TTF_CloseFont(fonts_[i]);
        fonts_[i] = TTF_OpenFont(fontPath_.u8string().c_str(), int(std::lround(kBasePixels[i] * scale_)));
        if (!fonts_[i]) return false;
        TTF_SetFontHinting(fonts_[i], TTF_HINTING_LIGHT);
    }
    clearCache();
    return true;
}

void Text::setScale(float scale) {
    if (std::fabs(scale - scale_) < 0.01f) return;
    scale_ = scale;
    openFonts();
}

void Text::clearCache() {
    for (auto& [k, e] : cache_)
        if (e.tex) SDL_DestroyTexture(e.tex);
    cache_.clear();
}

const Text::Entry* Text::get(const std::string& s, Size size) {
    if (s.empty() || !fonts_[size]) return nullptr;
    const std::string key = std::string(1, char('0' + size)) + s;
    auto it = cache_.find(key);
    if (it != cache_.end()) return &it->second;
    if (cache_.size() > kMaxCache) clearCache();

    // Render white; color is applied per draw with a color mod.
    SDL_Surface* surf = TTF_RenderUTF8_Blended(fonts_[size], s.c_str(), SDL_Color{255, 255, 255, 255});
    if (!surf) return nullptr;
    Entry e;
    e.tex = SDL_CreateTextureFromSurface(renderer_, surf);
    e.w = surf->w;
    e.h = surf->h;
    SDL_FreeSurface(surf);
    if (!e.tex) return nullptr;
    SDL_SetTextureBlendMode(e.tex, SDL_BLENDMODE_BLEND);
    return &cache_.emplace(key, e).first->second;
}

int Text::draw(int x, int y, const std::string& s, Color c, Size size) {
    const Entry* e = get(s, size);
    if (!e) return 0;
    SDL_SetTextureColorMod(e->tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(e->tex, c.a);
    SDL_Rect dst{x, y, e->w, e->h};
    SDL_RenderCopy(renderer_, e->tex, nullptr, &dst);
    return e->w;
}

int Text::width(const std::string& s, Size size) {
    const Entry* e = get(s, size);
    return e ? e->w : 0;
}

int Text::lineHeight(Size size) const { return fonts_[size] ? TTF_FontHeight(fonts_[size]) : 0; }
