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

    // Copy the frame the core just drew (w x h) into memory as top-down XRGB8888.
    const uint32_t* readFrame(unsigned w, unsigned h);

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
    };
    bool loadGl();
    void freeFramebuffer();

    retro_hw_render_callback request_{};
    SDL_Window* window_ = nullptr;
    SDL_GLContext context_ = nullptr;
    Gl gl_{};
    GLuint fbo_ = 0, colorTex_ = 0, depthRb_ = 0;
    unsigned fbW_ = 0, fbH_ = 0;
    std::vector<uint32_t> pixels_, flipped_;
};
