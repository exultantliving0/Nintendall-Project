#include "video.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "png.hpp"

bool Video::init(bool headless, bool fullscreen, int width, int height, std::string& error, const char* title,
                 bool vsync) {
    headless_ = headless;
    if (headless_) return true;

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");  // sharp pixels
    Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (fullscreen) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    window_ = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, width, height, flags);
    if (!window_) {
        error = std::string("Couldn't create window: ") + SDL_GetError();
        return false;
    }
    // Only one window may wait for vsync, or each would halve the frame rate.
    renderer_ = SDL_CreateRenderer(window_, -1, SDL_RENDERER_ACCELERATED | (vsync ? SDL_RENDERER_PRESENTVSYNC : 0));
    if (!renderer_) renderer_ = SDL_CreateRenderer(window_, -1, 0);
    if (!renderer_) {
        error = std::string("Couldn't create renderer: ") + SDL_GetError();
        return false;
    }
    vsync_ = vsync;
    SDL_SetRenderDrawBlendMode(renderer_, SDL_BLENDMODE_BLEND);
    onResize();
    return true;
}

void Video::shutdown() {
    if (overlayTex_) SDL_DestroyTexture(overlayTex_);
    if (glowTex_) SDL_DestroyTexture(glowTex_);
    overlayTex_ = glowTex_ = nullptr;
    overlayW_ = overlayH_ = glowW_ = glowH_ = 0;
    if (frameTex_) SDL_DestroyTexture(frameTex_);
    if (renderer_) SDL_DestroyRenderer(renderer_);
    if (window_) SDL_DestroyWindow(window_);
    frameTex_ = nullptr;
    renderer_ = nullptr;
    window_ = nullptr;
    texW_ = texH_ = 0;
    texFormat_ = 0;
    clearFrame();
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
    const bool sideways = rotation_ % 2 == 1;
    const double shownW = sideways ? frameH_ : frameW_, shownH = sideways ? frameW_ : frameH_;
    double aspect = aspect_ > 0 ? aspect_ : shownW / shownH;
    SDL_Rect src{0, 0, int(frameW_), int(frameH_)};
    if (crop_.w > 0 && crop_.h > 0 && rotation_ == 0) {
        const SDL_Rect full = src;
        if (!SDL_IntersectRect(&crop_, &full, &src)) return;
        if (cropAspect_ > 0) aspect = cropAspect_;
        else aspect *= (double(src.w) / src.h) / (shownW / shownH);  // keep the frame's pixel shape
    }
    int w = area.w, h = int(area.w / aspect);
    if (h > area.h) {
        h = area.h;
        w = int(area.h * aspect);
    }
    SDL_Rect box{area.x + (area.w - w) / 2, area.y + (area.h - h) / 2, w, h};
    gameRect_ = box;
    // Sharp pixels, or smoothed for Smooth and the CRT looks (a TV's picture is soft side to side).
    const bool smooth = shader_ != Shader::Off && shader_ != Shader::LcdGrid;
    SDL_SetTextureScaleMode(frameTex_, smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    if (shader_ != Shader::Off && shader_ != Shader::Smooth) {
        drawFiltered(src, box);
        return;
    }
    if (rotation_ == 0) {
        SDL_RenderCopy(renderer_, frameTex_, &src, &box);
        return;
    }
    // Rotate around the box centre. A sideways picture is drawn w/h-swapped, then turned.
    SDL_Rect dst = box;
    if (sideways) dst = {box.x + (w - h) / 2, box.y + (h - w) / 2, h, w};
    SDL_RenderCopyEx(renderer_, frameTex_, &src, &dst, -90.0 * rotation_, nullptr, SDL_FLIP_NONE);
}

// --- shaders --------------------------------------------------------------------

const std::vector<ShaderInfo>& shaderList() {
    static const std::vector<ShaderInfo> list = {
        {Shader::Off, "off", "Off (sharp pixels)"},
        {Shader::Smooth, "smooth", "Smooth"},
        {Shader::Scanlines, "crt-scanlines", "CRT: Scanlines"},
        {Shader::ApertureGrille, "crt-aperture", "CRT: Aperture Grille (PVM)"},
        {Shader::ShadowMask, "crt-shadowmask", "CRT: Shadow Mask (TV)"},
        {Shader::CurvedTV, "crt-curved", "CRT: Curved TV"},
        {Shader::LcdGrid, "lcd-grid", "LCD Grid (handhelds)"},
    };
    return list;
}

Shader shaderFromKey(const std::string& key) {
    for (const ShaderInfo& i : shaderList())
        if (key == i.key) return i.shader;
    return Shader::Off;
}

namespace {

bool isCrt(Shader s) {
    return s == Shader::Scanlines || s == Shader::ApertureGrille || s == Shader::ShadowMask || s == Shader::CurvedTV;
}

// The console's own line count: scanlines follow it even when the emulator draws the picture
// bigger (GameCube at 1080p still gets ~240 lines, like on a TV).
int nativeLines(int rows) {
    if (rows <= 300) return std::max(1, rows);
    return std::max(1, int(std::lround(rows / std::max(1.0, std::round(rows / 240.0)))));
}

uint32_t gray(float r, float g, float b) {
    auto c = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return 0xFF000000u | (c(r) << 16) | (c(g) << 8) | c(b);
}

}  // namespace

// The pattern multiplied over the picture, one texel per screen pixel (in the picture's own
// orientation: a sideways game's scanlines turn with it, like a rotated arcade monitor).
bool Video::updateOverlay(int w, int h, const SDL_Rect& src) {
    const int lines = isCrt(shader_) ? nativeLines(src.h) : src.h;
    const int cols = src.w;
    if (overlayTex_ && w == overlayW_ && h == overlayH_ && lines == overlayLines_ && cols == overlayCols_ &&
        shader_ == overlayShader_)
        return true;
    if (overlayTex_ && (w != overlayW_ || h != overlayH_)) {
        SDL_DestroyTexture(overlayTex_);
        overlayTex_ = nullptr;
    }
    if (!overlayTex_) {
        overlayTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, w, h);
        if (!overlayTex_) return false;
        SDL_SetTextureBlendMode(overlayTex_, SDL_BLENDMODE_MOD);
        SDL_SetTextureScaleMode(overlayTex_, SDL_ScaleModeNearest);
    }
    overlayW_ = w;
    overlayH_ = h;
    overlayLines_ = lines;
    overlayCols_ = cols;
    overlayShader_ = shader_;

    // Each row: how bright it is between two scanlines (a beam's soft edge). Fainter when the
    // window is too small to show them cleanly (under 3 screen pixels per line).
    const float perLine = float(h) / lines;
    std::vector<float> row(size_t(h), 1.0f);
    if (isCrt(shader_)) {
        const float depth = (shader_ == Shader::Scanlines ? 0.6f : 0.45f) * std::clamp((perLine - 1.5f) / 1.5f, 0.0f, 1.0f);
        for (int y = 0; y < h; ++y) {
            const float pos = (y + 0.5f) / perLine;
            const float d = pos - std::floor(pos) - 0.5f;  // distance from the line's centre, -0.5..0.5
            const float beam = std::exp(-d * d / (2 * 0.26f * 0.26f));
            row[size_t(y)] = 1.0f - depth * (1.0f - beam);
        }
    }
    std::vector<uint32_t> px(size_t(w) * h);
    for (int y = 0; y < h; ++y) {
        uint32_t* out = &px[size_t(y) * w];
        const float sl = row[size_t(y)];
        for (int x = 0; x < w; ++x) {
            float r = sl, g = sl, b = sl;
            switch (shader_) {
            case Shader::ApertureGrille: {  // red, green and blue phosphor stripes
                const int p = x % 3;
                const float off = 0.72f;
                r *= p == 0 ? 1.0f : off;
                g *= p == 1 ? 1.0f : off;
                b *= p == 2 ? 1.0f : off;
                break;
            }
            case Shader::ShadowMask:
            case Shader::CurvedTV: {  // slot mask: stripes, broken into staggered slots
                const int p = x % 3;
                const float off = 0.78f;
                r *= p == 0 ? 1.0f : off;
                g *= p == 1 ? 1.0f : off;
                b *= p == 2 ? 1.0f : off;
                if (y % 4 == ((x / 3) % 2) * 2) r *= 0.8f, g *= 0.8f, b *= 0.8f;
                break;
            }
            case Shader::LcdGrid: {  // thin dark lines between the screen's pixels
                const float cw = float(w) / cols, ch = float(h) / lines;
                const float fx = (x + 0.5f) / cw, fy = (y + 0.5f) / ch;
                const float dx = std::min(fx - std::floor(fx), std::ceil(fx) - fx) * cw;  // screen pixels to the edge
                const float dy = std::min(fy - std::floor(fy), std::ceil(fy) - fy) * ch;
                const float gx = std::clamp((cw - 2.0f) / 2.0f, 0.0f, 1.0f) * 0.3f;  // only with room for it
                const float gy = std::clamp((ch - 2.0f) / 2.0f, 0.0f, 1.0f) * 0.3f;
                const float f = (dx < 0.5f ? 1.0f - gx : 1.0f) * (dy < 0.5f ? 1.0f - gy : 1.0f);
                r = g = b = f;
                break;
            }
            default: break;
            }
            out[x] = gray(r, g, b);
        }
    }
    SDL_UpdateTexture(overlayTex_, nullptr, px.data(), w * 4);
    return true;
}

// A quarter-size copy of the picture, drawn back stretched (and so blurred) on top: the glow of
// a CRT's bright phosphors. It also brings back the light the scanlines and mask take away.
bool Video::updateGlow(const SDL_Rect& src) {
    if (!SDL_RenderTargetSupported(renderer_)) return false;
    const int w = std::max(8, src.w / 4), h = std::max(8, src.h / 4);
    if (glowTex_ && (w != glowW_ || h != glowH_)) {
        SDL_DestroyTexture(glowTex_);
        glowTex_ = nullptr;
    }
    if (!glowTex_) {
        glowTex_ = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_TARGET, w, h);
        if (!glowTex_) return false;
        SDL_SetTextureScaleMode(glowTex_, SDL_ScaleModeLinear);
        SDL_SetTextureBlendMode(glowTex_, SDL_BLENDMODE_ADD);
        glowW_ = w;
        glowH_ = h;
    }
    SDL_Texture* previous = SDL_GetRenderTarget(renderer_);
    if (SDL_SetRenderTarget(renderer_, glowTex_) != 0) return false;
    SDL_RenderCopy(renderer_, frameTex_, &src, nullptr);
    SDL_SetRenderTarget(renderer_, previous);
    return true;
}

// The picture as a grid of points: flat, or bulged like a TV's glass (Curved TV). Every layer
// (picture, pattern, glow) is drawn through the same grid, so they stay lined up.
void Video::drawFiltered(const SDL_Rect& src, const SDL_Rect& box) {
    const bool sideways = rotation_ % 2 == 1;
    const bool curved = shader_ == Shader::CurvedTV;
    const int nx = curved ? 32 : 1, ny = curved ? 24 : 1;

    struct Point {
        float x, y;  // on screen
        float u, v;  // in the picture (0..1, its own orientation)
        float shade;
    };
    std::vector<Point> pts;
    pts.reserve(size_t(nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i) {
            const float a = float(i) / nx, b = float(j) / ny;  // across the box
            float cx = a * 2 - 1, cy = b * 2 - 1;
            float shade = 1.0f;
            if (curved) {  // edges bow out, corners pull in; darker towards the corners
                const float k = 0.045f;
                const float px = cx * (1 - k * cy * cy), py = cy * (1 - k * 1.3f * cx * cx);
                shade = 1.0f - 0.22f * (std::pow(cx, 4.0f) + std::pow(cy, 4.0f)) / 2;
                cx = px;
                cy = py;
            }
            // Where this point of the box is in the picture, for a picture turned rotation_ * 90 degrees.
            float u = a, v = b;
            switch (rotation_ % 4) {
            case 1: u = 1 - b, v = a; break;
            case 2: u = 1 - a, v = 1 - b; break;
            case 3: u = b, v = 1 - a; break;
            default: break;
            }
            pts.push_back({box.x + (cx + 1) / 2 * box.w, box.y + (cy + 1) / 2 * box.h, u, v, shade});
        }
    std::vector<int> idx;
    idx.reserve(size_t(nx) * ny * 6);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int p = j * (nx + 1) + i;
            idx.insert(idx.end(), {p, p + 1, p + nx + 1, p + 1, p + nx + 2, p + nx + 1});
        }

    const bool glow = isCrt(shader_) && updateGlow(src);  // before drawing: it switches the render target
    std::vector<SDL_Vertex> verts(pts.size());
    auto draw = [&](SDL_Texture* tex, float u0, float v0, float du, float dv, float alpha, bool shaded) {
        for (size_t i = 0; i < pts.size(); ++i) {
            const Point& p = pts[i];
            const Uint8 c = Uint8(255 * (shaded ? p.shade : 1.0f));
            verts[i] = {{p.x, p.y}, {c, c, c, Uint8(255 * alpha)}, {u0 + p.u * du, v0 + p.v * dv}};
        }
        SDL_RenderGeometry(renderer_, tex, verts.data(), int(verts.size()), idx.data(), int(idx.size()));
    };

    // 1. The picture.
    SDL_SetTextureBlendMode(frameTex_, SDL_BLENDMODE_NONE);
    draw(frameTex_, float(src.x) / texW_, float(src.y) / texH_, float(src.w) / texW_, float(src.h) / texH_, 1.0f, true);
    // 2. Scanlines / mask / grid, at one texel per screen pixel.
    const int ow = sideways ? box.h : box.w, oh = sideways ? box.w : box.h;
    if (ow > 0 && oh > 0 && updateOverlay(ow, oh, src)) draw(overlayTex_, 0, 0, 1, 1, 1.0f, false);
    // 3. Glow (CRT looks).
    if (glow) {
        const float strength = shader_ == Shader::Scanlines ? 0.3f : 0.4f;
        draw(glowTex_, 0, 0, 1, 1, strength, true);
    }
}

bool Video::toFrame(int x, int y, double& fx, double& fy) const {
    const SDL_Rect& r = gameRect_;
    if (frameW_ == 0 || r.w <= 0 || r.h <= 0 || x < r.x || y < r.y || x >= r.x + r.w || y >= r.y + r.h) return false;
    const double u = double(x - r.x) / r.w, v = double(y - r.y) / r.h;
    if (crop_.w > 0 && crop_.h > 0 && rotation_ == 0) {
        fx = (crop_.x + u * crop_.w) / frameW_;
        fy = (crop_.y + v * crop_.h) / frameH_;
    } else {
        fx = u;
        fy = v;
    }
    return true;
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
