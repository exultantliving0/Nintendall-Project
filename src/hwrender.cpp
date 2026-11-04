#include "hwrender.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>


retro_proc_address_t HwRender::procAddress(const char* sym) {
    return reinterpret_cast<retro_proc_address_t>(SDL_GL_GetProcAddress(sym));
}

template <typename T>
static bool load(T& fn, const char* name) {
    fn = reinterpret_cast<T>(SDL_GL_GetProcAddress(name));
    return fn != nullptr;
}

bool HwRender::loadGl() {
    return load(gl_.GenFramebuffers, "glGenFramebuffers") && load(gl_.DeleteFramebuffers, "glDeleteFramebuffers") &&
           load(gl_.BindFramebuffer, "glBindFramebuffer") && load(gl_.FramebufferTexture2D, "glFramebufferTexture2D") &&
           load(gl_.GenRenderbuffers, "glGenRenderbuffers") && load(gl_.DeleteRenderbuffers, "glDeleteRenderbuffers") &&
           load(gl_.BindRenderbuffer, "glBindRenderbuffer") && load(gl_.RenderbufferStorage, "glRenderbufferStorage") &&
           load(gl_.FramebufferRenderbuffer, "glFramebufferRenderbuffer") &&
           load(gl_.CheckFramebufferStatus, "glCheckFramebufferStatus") && load(gl_.GenTextures, "glGenTextures") &&
           load(gl_.DeleteTextures, "glDeleteTextures") && load(gl_.BindTexture, "glBindTexture") &&
           load(gl_.TexImage2D, "glTexImage2D") && load(gl_.TexParameteri, "glTexParameteri") &&
           load(gl_.ReadPixels, "glReadPixels") && load(gl_.PixelStorei, "glPixelStorei") &&
           load(gl_.GetIntegerv, "glGetIntegerv") && load(gl_.GetString, "glGetString");
}

bool HwRender::create(const retro_hw_render_callback& req, std::string& error) {
    destroy();
    request_ = req;

    int profile = 0, major = 0, minor = 0;
    switch (req.context_type) {
    case RETRO_HW_CONTEXT_OPENGL:  // legacy / compatibility profile
        profile = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
        major = 2;
        minor = 1;
        break;
    case RETRO_HW_CONTEXT_OPENGL_CORE:
        profile = SDL_GL_CONTEXT_PROFILE_CORE;
        major = req.version_major ? int(req.version_major) : 3;
        minor = req.version_major ? int(req.version_minor) : 3;
        break;
    case RETRO_HW_CONTEXT_OPENGLES2:
    case RETRO_HW_CONTEXT_OPENGLES3:
    case RETRO_HW_CONTEXT_OPENGLES_VERSION:
        profile = SDL_GL_CONTEXT_PROFILE_ES;
        major = req.context_type == RETRO_HW_CONTEXT_OPENGLES2 ? 2 : (req.version_major ? int(req.version_major) : 3);
        minor = req.context_type == RETRO_HW_CONTEXT_OPENGLES_VERSION ? int(req.version_minor) : 0;
        break;
    default:
        error = "the core asked for a graphics API Nintendall doesn't provide (only OpenGL is supported)";
        return false;
    }

    if (!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0) {
        error = std::string("no display available for OpenGL (") + SDL_GetError() + ")";
        return false;
    }
    SDL_GLContext previous = SDL_GL_GetCurrentContext();
    SDL_Window* previousWin = SDL_GL_GetCurrentWindow();

    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, req.debug_context ? SDL_GL_CONTEXT_DEBUG_FLAG : 0);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    window_ = SDL_CreateWindow("Nintendall GL", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, 16, 16,
                               SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (window_) context_ = SDL_GL_CreateContext(window_);
    SDL_GL_ResetAttributes();
    if (!context_) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "this PC can't create an OpenGL %d.%d context (%s)", major, minor, SDL_GetError());
        error = buf;
        destroy();
        if (previous) SDL_GL_MakeCurrent(previousWin, previous);
        return false;
    }
    SDL_GL_MakeCurrent(window_, context_);
    if (!loadGl()) {
        error = "the OpenGL driver is missing framebuffer support";
        destroy();
        return false;
    }
    std::fprintf(stderr, "GPU rendering: OpenGL %s on %s\n", reinterpret_cast<const char*>(gl_.GetString(GL_VERSION)),
                 reinterpret_cast<const char*>(gl_.GetString(GL_RENDERER)));
    return true;
}

void HwRender::makeCurrent() {
    if (context_ && SDL_GL_GetCurrentContext() != context_) SDL_GL_MakeCurrent(window_, context_);
}

void HwRender::freeFramebuffer() {
    if (!context_) return;
    if (fbo_) gl_.DeleteFramebuffers(1, &fbo_);
    if (colorTex_) gl_.DeleteTextures(1, &colorTex_);
    if (depthRb_) gl_.DeleteRenderbuffers(1, &depthRb_);
    fbo_ = colorTex_ = depthRb_ = 0;
    fbW_ = fbH_ = 0;
}

bool HwRender::resize(unsigned w, unsigned h) {
    if (!context_ || w == 0 || h == 0) return false;
    if (fbo_ && w <= fbW_ && h <= fbH_) return true;
    makeCurrent();
    w = std::max(w, fbW_);
    h = std::max(h, fbH_);

    // Keep the same framebuffer object (the core may have remembered its id); just replace its storage.
    if (!fbo_) gl_.GenFramebuffers(1, &fbo_);
    if (colorTex_) gl_.DeleteTextures(1, &colorTex_);
    if (depthRb_) gl_.DeleteRenderbuffers(1, &depthRb_);
    colorTex_ = depthRb_ = 0;

    GLint prevFbo = 0;
    gl_.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    gl_.BindFramebuffer(GL_FRAMEBUFFER, fbo_);

    gl_.GenTextures(1, &colorTex_);
    gl_.BindTexture(GL_TEXTURE_2D, colorTex_);
    gl_.TexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, GLsizei(w), GLsizei(h), 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl_.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    gl_.BindTexture(GL_TEXTURE_2D, 0);
    gl_.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, colorTex_, 0);

    if (request_.depth) {
        gl_.GenRenderbuffers(1, &depthRb_);
        gl_.BindRenderbuffer(GL_RENDERBUFFER, depthRb_);
        gl_.RenderbufferStorage(GL_RENDERBUFFER, request_.stencil ? GL_DEPTH24_STENCIL8 : GL_DEPTH_COMPONENT24,
                                GLsizei(w), GLsizei(h));
        gl_.BindRenderbuffer(GL_RENDERBUFFER, 0);
        gl_.FramebufferRenderbuffer(GL_FRAMEBUFFER, request_.stencil ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT,
                                    GL_RENDERBUFFER, depthRb_);
    }
    const bool ok = gl_.CheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    gl_.BindFramebuffer(GL_FRAMEBUFFER, GLuint(prevFbo));
    fbW_ = w;
    fbH_ = h;
    if (!ok) std::fprintf(stderr, "GPU rendering: framebuffer %ux%u is incomplete\n", w, h);
    return ok;
}

const uint32_t* HwRender::readFrame(unsigned w, unsigned h) {
    if (!context_ || !fbo_ || w == 0 || h == 0) return nullptr;
    w = std::min(w, fbW_);
    h = std::min(h, fbH_);
    makeCurrent();
    GLint prevRead = 0;
    gl_.GetIntegerv(GL_FRAMEBUFFER_BINDING, &prevRead);
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, fbo_);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl_.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    pixels_.resize(size_t(w) * h);
    // BGRA bytes == XRGB8888 on little-endian machines.
    gl_.ReadPixels(0, 0, GLsizei(w), GLsizei(h), GL_BGRA, GL_UNSIGNED_BYTE, pixels_.data());
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prevRead));
    if (!request_.bottom_left_origin) return pixels_.data();

    // OpenGL's first row is the bottom one: flip to top-down.
    flipped_.resize(pixels_.size());
    for (unsigned y = 0; y < h; ++y)
        std::memcpy(&flipped_[size_t(y) * w], &pixels_[size_t(h - 1 - y) * w], size_t(w) * 4);
    return flipped_.data();
}

void HwRender::destroy() {
    if (context_) {
        makeCurrent();
        freeFramebuffer();
        SDL_GL_MakeCurrent(window_, nullptr);
        SDL_GL_DeleteContext(context_);
    }
    if (window_) SDL_DestroyWindow(window_);
    context_ = nullptr;
    window_ = nullptr;
}
