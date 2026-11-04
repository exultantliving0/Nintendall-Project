#include "hwrender.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

#ifndef GL_PIXEL_PACK_BUFFER
#define GL_PIXEL_PACK_BUFFER 0x88EB
#endif
#ifndef GL_PIXEL_PACK_BUFFER_BINDING
#define GL_PIXEL_PACK_BUFFER_BINDING 0x88ED
#endif
#ifndef GL_STREAM_READ
#define GL_STREAM_READ 0x88E1
#endif
#ifndef GL_MAP_READ_BIT
#define GL_MAP_READ_BIT 0x0001
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_RENDERBUFFER_BINDING
#define GL_RENDERBUFFER_BINDING 0x8CA7
#endif

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
    fast_ = load(gl_.BlitFramebuffer, "glBlitFramebuffer") && load(gl_.GenBuffers, "glGenBuffers") &&
            load(gl_.DeleteBuffers, "glDeleteBuffers") && load(gl_.BindBuffer, "glBindBuffer") &&
            load(gl_.BufferData, "glBufferData") && load(gl_.MapBufferRange, "glMapBufferRange") &&
            load(gl_.UnmapBuffer, "glUnmapBuffer") && load(gl_.IsEnabled, "glIsEnabled") &&
            load(gl_.Enable, "glEnable") && load(gl_.Disable, "glDisable");
    if (SDL_getenv("NINTENDALL_SLOW_READBACK")) fast_ = false;  // for comparing in tests
    std::fprintf(stderr, "GPU rendering: OpenGL %s on %s%s\n", reinterpret_cast<const char*>(gl_.GetString(GL_VERSION)),
                 reinterpret_cast<const char*>(gl_.GetString(GL_RENDERER)), fast_ ? "" : " (slow frame copy)");
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

const uint32_t* HwRender::readFrame(unsigned w, unsigned h, unsigned maxW, unsigned maxH, unsigned& outW,
                                    unsigned& outH) {
    if (!context_ || !fbo_ || w == 0 || h == 0) return nullptr;
    w = std::min(w, fbW_);
    h = std::min(h, fbH_);
    makeCurrent();
    if (fast_) {
        // Never enlarge on the GPU: the window's own scaler does that for free.
        const unsigned ow = maxW ? std::clamp(maxW, 1u, w) : w;
        const unsigned oh = maxH ? std::clamp(maxH, 1u, h) : h;
        return readFrameFast(w, h, ow, oh, outW, outH);
    }
    outW = w;
    outH = h;
    return readFrameSlow(w, h);
}

const uint32_t* HwRender::readFrameSlow(unsigned w, unsigned h) {
    GLint prevRead = 0;
    gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
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

bool HwRender::ensureTarget(Target& t, unsigned w, unsigned h) {
    if (t.fbo && t.w == w && t.h == h) return true;
    if (!t.fbo) gl_.GenFramebuffers(1, &t.fbo);
    if (t.rb) gl_.DeleteRenderbuffers(1, &t.rb);
    gl_.GenRenderbuffers(1, &t.rb);
    gl_.BindRenderbuffer(GL_RENDERBUFFER, t.rb);
    gl_.RenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, GLsizei(w), GLsizei(h));
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
    gl_.FramebufferRenderbuffer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, t.rb);
    t.w = w;
    t.h = h;
    return gl_.CheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

const uint32_t* HwRender::mapPbo(Pbo& p, unsigned& rw, unsigned& rh) {
    p.pending = false;
    const size_t bytes = size_t(p.w) * p.h * 4;
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, p.id);
    const void* src = gl_.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, ptrdiff_t(bytes), GL_MAP_READ_BIT);
    if (!src) return nullptr;
    pixels_.resize(size_t(p.w) * p.h);
    std::memcpy(pixels_.data(), src, bytes);
    gl_.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
    rw = p.w;
    rh = p.h;
    return pixels_.data();
}

const uint32_t* HwRender::readFrameFast(unsigned w, unsigned h, unsigned outW, unsigned outH, unsigned& rw,
                                        unsigned& rh) {
    // Save the bits of GL state we touch; the core keeps drawing with its own state afterwards.
    GLint prevRead = 0, prevDraw = 0, prevPack = 0, prevRb = 0, prevAlign = 4, prevRowLen = 0;
    gl_.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    gl_.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &prevDraw);
    gl_.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
    gl_.GetIntegerv(GL_RENDERBUFFER_BINDING, &prevRb);
    gl_.GetIntegerv(GL_PACK_ALIGNMENT, &prevAlign);
    gl_.GetIntegerv(GL_PACK_ROW_LENGTH, &prevRowLen);
    const bool scissor = gl_.IsEnabled(GL_SCISSOR_TEST);  // would clip the blits
    if (scissor) gl_.Disable(GL_SCISSOR_TEST);

    // 1. Shrink in halving steps (each one averages 2x2 pixels), so detail from upscaling
    //    turns into smooth edges instead of shimmering.
    GLuint src = fbo_;
    unsigned sw = w, sh = h;
    int flip = 0;
    while (sw > outW * 2 || sh > outH * 2) {
        const unsigned nw = sw > outW * 2 ? (sw + 1) / 2 : sw;
        const unsigned nh = sh > outH * 2 ? (sh + 1) / 2 : sh;
        Target& t = scratch_[flip];
        flip ^= 1;
        if (!ensureTarget(t, nw, nh)) break;
        gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, src);
        gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, t.fbo);
        gl_.BlitFramebuffer(0, 0, GLint(sw), GLint(sh), 0, 0, GLint(nw), GLint(nh), GL_COLOR_BUFFER_BIT, GL_LINEAR);
        src = t.fbo;
        sw = nw;
        sh = nh;
    }
    // 2. Final size, turned upright (OpenGL stores the bottom row first).
    const bool needFlip = request_.bottom_left_origin;
    GLuint readFrom = src;
    if (needFlip || sw != outW || sh != outH) {
        if (ensureTarget(out_, outW, outH)) {
            gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, src);
            gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, out_.fbo);
            const GLint y0 = needFlip ? GLint(outH) : 0, y1 = needFlip ? 0 : GLint(outH);
            gl_.BlitFramebuffer(0, 0, GLint(sw), GLint(sh), 0, y0, GLint(outW), y1, GL_COLOR_BUFFER_BIT,
                                sw == outW && sh == outH ? GL_NEAREST : GL_LINEAR);
            readFrom = out_.fbo;
        }
    }
    // 3. Start copying it to memory without waiting, and pick up the previous frame's copy.
    Pbo& cur = pbo_[pboNext_];
    Pbo& prev = pbo_[pboNext_ ^ 1];
    pboNext_ ^= 1;
    const size_t bytes = size_t(outW) * outH * 4;
    if (!cur.id) gl_.GenBuffers(1, &cur.id);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, cur.id);
    if (cur.capacity < bytes) {
        gl_.BufferData(GL_PIXEL_PACK_BUFFER, ptrdiff_t(bytes), nullptr, GL_STREAM_READ);
        cur.capacity = bytes;
    }
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, readFrom);
    gl_.PixelStorei(GL_PACK_ALIGNMENT, 4);
    gl_.PixelStorei(GL_PACK_ROW_LENGTH, 0);
    gl_.ReadPixels(0, 0, GLsizei(outW), GLsizei(outH), GL_BGRA, GL_UNSIGNED_BYTE, nullptr);  // BGRA = XRGB8888
    cur.w = outW;
    cur.h = outH;
    cur.pending = true;
    // Nothing older in flight (first frame, or it was already shown as a duplicate): show this one
    // next time instead.
    const uint32_t* result = prev.pending ? mapPbo(prev, rw, rh) : nullptr;

    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(prevPack));
    gl_.PixelStorei(GL_PACK_ALIGNMENT, prevAlign);
    gl_.PixelStorei(GL_PACK_ROW_LENGTH, prevRowLen);
    gl_.BindRenderbuffer(GL_RENDERBUFFER, GLuint(prevRb));
    gl_.BindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(prevRead));
    gl_.BindFramebuffer(GL_DRAW_FRAMEBUFFER, GLuint(prevDraw));
    if (scissor) gl_.Enable(GL_SCISSOR_TEST);
    return result;
}

const uint32_t* HwRender::takePending(unsigned& outW, unsigned& outH) {
    if (!context_ || !fast_) return nullptr;
    Pbo& last = pbo_[pboNext_ ^ 1];
    if (!last.pending) return nullptr;
    makeCurrent();
    GLint prevPack = 0;
    gl_.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
    const uint32_t* px = mapPbo(last, outW, outH);
    gl_.BindBuffer(GL_PIXEL_PACK_BUFFER, GLuint(prevPack));
    return px;
}

void HwRender::freeFastPath() {
    for (Target* t : {&scratch_[0], &scratch_[1], &out_}) {
        if (t->fbo) gl_.DeleteFramebuffers(1, &t->fbo);
        if (t->rb) gl_.DeleteRenderbuffers(1, &t->rb);
        *t = Target{};
    }
    for (Pbo& p : pbo_) {
        if (p.id) gl_.DeleteBuffers(1, &p.id);
        p = Pbo{};
    }
    pboNext_ = 0;
}

void HwRender::destroy() {
    if (context_) {
        makeCurrent();
        freeFramebuffer();
        if (fast_) freeFastPath();
        SDL_GL_MakeCurrent(window_, nullptr);
        SDL_GL_DeleteContext(context_);
    }
    if (window_) SDL_DestroyWindow(window_);
    context_ = nullptr;
    window_ = nullptr;
}
