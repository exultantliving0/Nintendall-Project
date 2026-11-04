#include "video.hpp"

#include <algorithm>
#include <cstring>

#include "png.hpp"

bool Video::init(bool headless, bool fullscreen, int width, int height, std::string& error) {
    headless_ = headless;
    if (headless_) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // sharp pixels
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window_ = SDL_CreateWindow("Nintendall", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!window_) {
        error = std::string("Couldn't create window: ") + SDL_GetError();
        return false;
    }
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, 0);
    if (!renderer_) {
        error = std::string("Couldn't create renderer: ") + SDL_GetError();
        return false;
    }
    vsync_ = true;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    onResize();
    return true;
}

void Video::shutdown() {
    if (frameTex_) SDL_DestroyTexture(frameTex_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    frameTex_ = nullptr;
    renderer_ = nullptr;
    window_ = nullptr;
}

void Video::onResize() {
    if (renderer_) SDL_GetRendererOutputSize(renderer_, &outW_, &outH_);
}

void Video::toggleFullscreen() {
    if (!window_) return;
    const bool fs = SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP;
    SDL_SetWindowFullscreen(window_, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
    onResize();
}

bool Video::isFullscreen() const {
    return window_ && (SDL_GetWindowFlags(window_) & SDL_WINDOW_FULLSCREEN_DESKTOP);
}

float Video::pixelRatio() const {
    if (!window_) return 1.0f;
    int ww = 0, wh = 0;
    SDL_GetWindowSize(window_, &ww, &wh);
    return ww > 0 ? float(outW_) / float(ww) : 1.0f;
}

void Video::setVsync(bool on) {
    if (!renderer_ || on == vsync_) return;
    if (SDL_RenderSetVSync(renderer_, on ? 1 : 0) == 0) vsync_ = on;
}

double Video::displayHz() const {
    SDL_DisplayMode mode{};
    const int idx = window_ ? SDL_GetWindowDisplayIndex(window_) : 0;
    if (SDL_GetCurrentDisplayMode(idx < 0 ? 0 : idx, &mode) == 0 && mode.refresh_rate > 0) return mode.refresh_rate;
    return 60.0;
}

// --- game picture -----------------------------------------------------------

static Uint32 sdlFormat(retro_pixel_format f) {
    switch (f) {
    case RETRO_PIXEL_FORMAT_XRGB8888: return SDL_PIXELFORMAT_XRGB8888;
    case RETRO_PIXEL_FORMAT_RGB565: return SDL_PIXELFORMAT_RGB565;
    default: return SDL_PIXELFORMAT_XRGB1555;
    }
}

static size_t bytesPerPixel(retro_pixel_format f) { return f == RETRO_PIXEL_FORMAT_XRGB8888 ? 4 : 2; }

void Video::ensureTexture(unsigned w, unsigned h, Uint32 format) {
    if (frameTex_ && format == texFormat_ && int(w) <= texW_ && int(h) <= texH_) return;
    if (frameTex_) SDL_DestroyTexture(frameTex_);
    texW_ = std::max<int>(int(w), texW_);
    texH_ = std::max<int>(int(h), texH_);
    if (format != texFormat_) {  // new format: start the size from scratch
        texW_ = int(w);
        texH_ = int(h);
    }
    texFormat_ = format;
    frameTex_ = SDL_CreateTexture(renderer_, format, SDL_TEXTUREACCESS_STREAMING, texW_, texH_);
}

void Video::uploadFrame(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt) {
    // Keep a tightly-packed copy for screenshots.
    const size_t rowBytes = w * bytesPerPixel(fmt);
    frame_.resize(rowBytes * h);
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(&frame_[y * rowBytes], static_cast<const uint8_t*>(data) + y * pitch, rowBytes);
    frameW_ = w;
    frameH_ = h;
    framePitch_ = rowBytes;
    frameFmt_ = fmt;

    if (!renderer_) return;
    ensureTexture(w, h, sdlFormat(fmt));
    if (!frameTex_) return;
    SDL_Rect r{0, 0, int(w), int(h)};
    SDL_UpdateTexture(frameTex_, &r, data, int(pitch));
}

void Video::clearFrame() {
    frameW_ = frameH_ = 0;
    frame_.clear();
}

void Video::drawGame(const SDL_Rect& area) {
    if (!renderer_ || !frameTex_ || frameW_ == 0 || area.w <= 0 || area.h <= 0) return;
    const double aspect = aspect_ > 0 ? aspect_ : double(frameW_) / frameH_;
    int w = area.w, h = int(area.w / aspect);
    if (h > area.h) {
        h = area.h;
        w = int(area.h * aspect);
    }
    SDL_Rect src{0, 0, int(frameW_), int(frameH_)};
    SDL_Rect dst{area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
    SDL_RenderCopy(renderer_, frameTex_, &src, &dst);
}

bool Video::saveScreenshot(const std::filesystem::path& path) const {
    if (frameW_ == 0) return false;
    std::vector<uint8_t> rgb(size_t(frameW_) * frameH_ * 3);
    for (unsigned y = 0; y < frameH_; ++y) {
        const uint8_t* row = &frame_[y * framePitch_];
        for (unsigned x = 0; x < frameW_; ++x) {
            uint8_t* o = &rgb[(size_t(y) * frameW_ + x) * 3];
            if (frameFmt_ == RETRO_PIXEL_FORMAT_XRGB8888) {
                uint32_t p;
                std::memcpy(&p, row + x * 4, 4);
                o[0] = uint8_t(p >> 16);
                o[1] = uint8_t(p >> 8);
                o[2] = uint8_t(p);
            } else {
                uint16_t p;
                std::memcpy(&p, row + x * 2, 2);
                if (frameFmt_ == RETRO_PIXEL_FORMAT_RGB565) {
                    o[0] = uint8_t(((p >> 11) & 31) * 255 / 31);
                    o[1] = uint8_t(((p >> 5) & 63) * 255 / 63);
                    o[2] = uint8_t((p & 31) * 255 / 31);
                } else {
                    o[0] = uint8_t(((p >> 10) & 31) * 255 / 31);
                    o[1] = uint8_t(((p >> 5) & 31) * 255 / 31);
                    o[2] = uint8_t((p & 31) * 255 / 31);
                }
            }
        }
    }
    return writePng(path, int(frameW_), int(frameH_), rgb.data());
}

// --- 2D drawing ---------------------------------------------------------------

void Video::clear(Color c) {
    if (!renderer_) return;
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, 255);
    SDL_RenderClear(renderer_);
}

void Video::fillRect(int x, int y, int w, int h, Color c) {
    if (!renderer_) return;
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_Rect r{x, y, w, h};
    SDL_RenderFillRect(renderer_, &r);
}

void Video::line(int x1, int y1, int x2, int y2, Color c) {
    if (!renderer_) return;
    SDL_SetRenderDrawColor(renderer_, c.r, c.g, c.b, c.a);
    SDL_RenderDrawLine(renderer_, x1, y1, x2, y2);
}

void Video::present() {
    if (renderer_) SDL_RenderPresent(renderer_);
}
