#pragma once
#include <SDL.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "libretro.h"

struct Color {
    uint8_t r, g, b, a = 255;
};

// Window, game picture and simple 2D drawing for the menus.
// In headless mode (tests/benchmarks) no window is created; frames are just kept in memory.
class Video {
public:
    bool init(bool headless, bool fullscreen, int width, int height, std::string& error);
    void shutdown();

    // Game picture
    void uploadFrame(const void* data, unsigned w, unsigned h, size_t pitch, retro_pixel_format fmt);
    void setAspect(double aspect) { aspect_ = aspect; }  // of the picture as shown (after rotation)
    void setRotation(unsigned quarterTurnsCcw) { rotation_ = quarterTurnsCcw % 4; }
    unsigned rotation() const { return rotation_; }
    void clearFrame();
    bool hasFrame() const { return frameW_ > 0; }
    void drawGame(const SDL_Rect& area);  // letterboxed into `area`
    const SDL_Rect& gameRect() const { return gameRect_; }  // where the game was last drawn
    bool saveScreenshot(const std::filesystem::path& path) const;

    // 2D drawing (UI)
    void clear(Color c);
    void fillRect(int x, int y, int w, int h, Color c);
    void line(int x1, int y1, int x2, int y2, Color c);
    void present();
    SDL_Renderer* renderer() const { return renderer_; }

    int width() const { return outW_; }
    int height() const { return outH_; }
    void toggleFullscreen();
    bool isFullscreen() const;
    float pixelRatio() const;  // renderer pixels per window unit (2 on Retina Macs)
    void setVsync(bool on);
    bool vsync() const { return vsync_; }
    double displayHz() const;
    bool headless() const { return headless_; }
    SDL_Window* window() const { return window_; }
    void onResize();

private:
    void ensureTexture(unsigned w, unsigned h, Uint32 format);

    bool headless_ = false;
    bool vsync_ = false;
    SDL_Window* window_ = nullptr;
    SDL_Renderer* renderer_ = nullptr;
    SDL_Texture* frameTex_ = nullptr;
    int texW_ = 0, texH_ = 0;
    Uint32 texFormat_ = 0;
    int outW_ = 960, outH_ = 720;

    // Copy of the last game frame (for screenshots and headless mode).
    std::vector<uint8_t> frame_;
    unsigned frameW_ = 0, frameH_ = 0;
    size_t framePitch_ = 0;
    retro_pixel_format frameFmt_ = RETRO_PIXEL_FORMAT_XRGB8888;
    double aspect_ = 4.0 / 3.0;
    SDL_Rect gameRect_{0, 0, 0, 0};
    unsigned rotation_ = 0;
};
