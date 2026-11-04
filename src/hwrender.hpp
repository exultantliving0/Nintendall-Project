#pragma once
#include <SDL.h>
#include <SDL_opengl.h>

#include <cstdint>
#include <string>
#include <vector>

#include "libretro.h"

// GPU ("hardware") rendering for libretro cores that draw with OpenGL
// (3DS, and later N64 / GameCube / Wii U).
//
// The core gets its own OpenGL context on a hidden window and draws into an
// offscreen framebuffer we own. After each frame we copy the picture back to
// memory and hand it to the normal video path. That keeps the visible window
// independent of OpenGL, so it works with any SDL renderer (Direct3D on Windows).
//
// To keep upscaled games fast, the copy is made cheap: the picture is first
// shrunk on the GPU to the size it's shown at (an upscaled frame can be 20x
// bigger than the window), flipped upright on the GPU, and read back
// asynchronously so the CPU never waits for the GPU to finish drawing.
class HwRender {
public:
    ~HwRender() { destroy(); }

    // Create the GL context the core asked for. Returns false if this PC can't provide it.
    bool create(const retro_hw_render_callback& request, std::string& error);
    // (Re)allocate the framebuffer so it can hold at least w x h pixels.
    bool resize(unsigned w, unsigned h);
    void makeCurrent();
    void destroy();
    bool active() const { return context_ != nullptr; }

    uintptr_t framebuffer() const { return fbo_; }
    static retro_proc_address_t procAddress(const char* sym);

    // Copy the frame the core just drew (w x h) into memory as top-down XRGB8888, shrunk to fit
    // within maxW x maxH (0 = no limit). Returns the previous frame's pixels when the copy is
    // asynchronous, or nullptr if nothing is ready yet. outW/outH receive the returned size.
    const uint32_t* readFrame(unsigned w, unsigned h, unsigned maxW, unsigned maxH, unsigned& outW, unsigned& outH);
    // The last frame whose copy is still in flight (for duplicate frames), or nullptr.
    const uint32_t* takePending(unsigned& outW, unsigned& outH);

private:
    struct Gl {
        void(APIENTRY* GenFramebuffers)(GLsizei, GLuint*);
        void(APIENTRY* DeleteFramebuffers)(GLsizei, const GLuint*);
        void(APIENTRY* BindFramebuffer)(GLenum, GLuint);
        void(APIENTRY* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
        void(APIENTRY* GenRenderbuffers)(GLsizei, GLuint*);
        void(APIENTRY* DeleteRenderbuffers)(GLsizei, const GLuint*);
        void(APIENTRY* BindRenderbuffer)(GLenum, GLuint);
        void(APIENTRY* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
        void(APIENTRY* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
        GLenum(APIENTRY* CheckFramebufferStatus)(GLenum);
        void(APIENTRY* GenTextures)(GLsizei, GLuint*);
        void(APIENTRY* DeleteTextures)(GLsizei, const GLuint*);
        void(APIENTRY* BindTexture)(GLenum, GLuint);
        void(APIENTRY* TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void*);
        void(APIENTRY* TexParameteri)(GLenum, GLenum, GLint);
        void(APIENTRY* ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void*);
        void(APIENTRY* PixelStorei)(GLenum, GLint);
        void(APIENTRY* GetIntegerv)(GLenum, GLint*);
        const unsigned char*(APIENTRY* GetString)(GLenum);
        // Optional (fast path): GL 3.0 / ES 3.0
        void(APIENTRY* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
        void(APIENTRY* GenBuffers)(GLsizei, GLuint*);
        void(APIENTRY* DeleteBuffers)(GLsizei, const GLuint*);
        void(APIENTRY* BindBuffer)(GLenum, GLuint);
        void(APIENTRY* BufferData)(GLenum, ptrdiff_t, const void*, GLenum);
        void*(APIENTRY* MapBufferRange)(GLenum, ptrdiff_t, ptrdiff_t, GLbitfield);
        GLboolean(APIENTRY* UnmapBuffer)(GLenum);
        GLboolean(APIENTRY* IsEnabled)(GLenum);
        void(APIENTRY* Enable)(GLenum);
        void(APIENTRY* Disable)(GLenum);
    };
    // A GPU-side image we can blit into and read from.
    struct Target {
        GLuint fbo = 0, rb = 0;
        unsigned w = 0, h = 0;
    };
    // A pixel buffer receiving an asynchronous read.
    struct Pbo {
        GLuint id = 0;
        size_t capacity = 0;
        unsigned w = 0, h = 0;
        bool pending = false;
    };
    bool loadGl();
    void freeFramebuffer();
    void freeFastPath();
    bool ensureTarget(Target& t, unsigned w, unsigned h);
    const uint32_t* readFrameFast(unsigned w, unsigned h, unsigned outW, unsigned outH, unsigned& rw, unsigned& rh);
    const uint32_t* readFrameSlow(unsigned w, unsigned h);
    const uint32_t* mapPbo(Pbo& p, unsigned& rw, unsigned& rh);

    retro_hw_render_callback request_{};
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    Gl gl_{};
    GLuint fbo_ = 0, colorTex_ = 0, depthRb_ = 0;
    unsigned fbW_ = 0, fbH_ = 0;
    std::vector<uint32_t> pixels_, flipped_;
    bool fast_ = false;  // blit + pixel buffers available
    Target scratch_[2], out_;
    Pbo pbo_[2];
    int pboNext_ = 0;
};
