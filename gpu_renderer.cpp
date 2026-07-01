#include "gpu_renderer.h"
#include "perf_timer.h"

#include <drm/drm_fourcc.h>
#include <png.h>

#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace libcamera;

// ── Shader source ─────────────────────────────────────────────────────────────

static const char kVS[] = R"glsl(
#version 300 es
out vec2 vTexCoord;
void main() {
    const vec2 pos[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vTexCoord   = pos[gl_VertexID] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
}
)glsl";

static const char kFS[] = R"glsl(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTexture;
in  vec2 vTexCoord;
out vec4 fragColor;
void main() {
    /* Flip Y: DMA-BUF origin is top-left, GL UV origin is bottom-left. */
    vec2 uv  = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec3 rgb = texture(uTexture, uv).rgb;

    /* Red tint: boost red channel, suppress green and blue. */
    vec3 tinted = vec3(min(rgb.r * 1.5, 1.0), rgb.g * 0.5, rgb.b * 0.5);

    /* Parking guide grid:
     *  3 horizontal lines at 25%, 50%, 75% — depth cues
     *  2 vertical lines at 33%, 66%        — lane boundaries
     */
    float lw = 0.005;
    float h1 = 1.0 - step(lw, abs(uv.y - 0.25));
    float h2 = 1.0 - step(lw, abs(uv.y - 0.50));
    float h3 = 1.0 - step(lw, abs(uv.y - 0.75));
    float v1 = 1.0 - step(lw, abs(uv.x - 0.33));
    float v2 = 1.0 - step(lw, abs(uv.x - 0.66));
    float grid = max(max(max(h1, h2), max(h3, v1)), v2);

    fragColor = vec4(mix(tinted, vec3(1.0), grid), 1.0);
}
)glsl";

// ── Helpers ───────────────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[gpu] shader compile error:\n" << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[gpu] program link error:\n" << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ── GpuRenderer ───────────────────────────────────────────────────────────────

bool GpuRenderer::load_ext_fns()
{
    pfn_CreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    pfn_DestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    pfn_TexImage2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!pfn_CreateImage || !pfn_DestroyImage || !pfn_TexImage2DOES) {
        std::cerr << "[gpu] failed to resolve EGL/GL extension functions\n";
        return false;
    }

    // Optional: GL_EXT_disjoint_timer_query for true GPU execution time.
    pfn_GenQueries = reinterpret_cast<PFNGLGENQUERIESEXTPROC>(
        eglGetProcAddress("glGenQueriesEXT"));
    pfn_DelQueries = reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(
        eglGetProcAddress("glDeleteQueriesEXT"));
    pfn_BeginQuery = reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(
        eglGetProcAddress("glBeginQueryEXT"));
    pfn_EndQuery   = reinterpret_cast<PFNGLENDQUERYEXTPROC>(
        eglGetProcAddress("glEndQueryEXT"));
    pfn_GetQuery64 = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(
        eglGetProcAddress("glGetQueryObjectui64vEXT"));

    gpu_stats_.available = pfn_GenQueries && pfn_DelQueries &&
                           pfn_BeginQuery  && pfn_EndQuery   && pfn_GetQuery64;
    if (!gpu_stats_.available)
        std::printf("[gpu] GL_EXT_disjoint_timer_query not available — GPU time not measured\n");

    return true;
}

EGLImageKHR GpuRenderer::create_egl_image(const FrameBuffer *buf)
{
    const auto &planes = buf->planes();
    const EGLint attrs[] = {
        EGL_WIDTH,                      W_,
        EGL_HEIGHT,                     H_,
        EGL_LINUX_DRM_FOURCC_EXT,       DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT,      planes[0].fd.get(),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  (EGLint)planes[0].offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   stride_,
        EGL_DMA_BUF_PLANE1_FD_EXT,      planes[1].fd.get(),
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,  (EGLint)planes[1].offset,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,   stride_,
        EGL_NONE,
    };
    return pfn_CreateImage(egl_->dpy, EGL_NO_CONTEXT,
                           EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
}

bool GpuRenderer::init(const EGLState &egl,
                       const StreamConfiguration &sc,
                       const std::vector<std::unique_ptr<FrameBuffer>> &buffers)
{
    egl_    = &egl;
    W_      = (int)sc.size.width;
    H_      = (int)sc.size.height;
    stride_ = (int)sc.stride;

    if (!load_ext_fns()) return false;

    // ── Compile shader once ───────────────────────────────────────────────
    double t0 = now_ms();
    prog_ = build_program(kVS, kFS);
    if (!prog_) return false;
    std::printf("[gpu] shader compile+link : %.1f ms (one-time)\n", now_ms() - t0);

    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "uTexture"), 0);

    // ── Create FBO once ───────────────────────────────────────────────────
    glGenFramebuffers(1,  &fbo_);
    glGenRenderbuffers(1, &rbo_);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W_, H_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rbo_);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[gpu] FBO incomplete\n";
        return false;
    }
    glViewport(0, 0, W_, H_);

    // ── GPU timer query object (one, single-buffered) ─────────────────────
    if (gpu_stats_.available)
        pfn_GenQueries(1, &timer_query_);

    // ── Pre-cache EGLImage + texture per buffer ───────────────────────────
    // eglCreateImageKHR is called here ONCE per buffer, not per frame.
    // The fd→EGLImage binding is stable for the lifetime of the buffer pool.
    for (const auto &buf : buffers) {
        int fd = buf->planes()[0].fd.get();

        EGLImageKHR img = create_egl_image(buf.get());
        if (img == EGL_NO_IMAGE_KHR) {
            std::cerr << "[gpu] eglCreateImageKHR failed for fd=" << fd
                      << " (0x" << std::hex << eglGetError() << std::dec << ")\n";
            return false;
        }

        GLuint tex = 0;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        pfn_TexImage2DOES(GL_TEXTURE_EXTERNAL_OES, img);

        fd_cache_[fd] = {img, tex};
        std::printf("[gpu] cached fd=%-3d → EGLImage+texture\n", fd);
    }

    std::printf("[gpu] renderer ready: %dx%d, %zu buffer(s) cached\n",
                W_, H_, fd_cache_.size());
    return true;
}

void GpuRenderer::render_frame(const FrameBuffer *buf)
{
    // Collect last frame's GPU time. At 15–30 fps the GPU finishes in ~11 ms;
    // the next render_frame() arrives 33–67 ms later so the result is ready
    // without any stall. GL_QUERY_RESULT returns instantly here.
    if (timer_pending_ && gpu_stats_.available) {
        GLuint64 ns = 0;
        pfn_GetQuery64(timer_query_, GL_QUERY_RESULT_EXT, &ns);
        double ms = (double)ns * 1e-6;

        auto &s = gpu_stats_;
        if (s.warmup_count < GpuStats::WARMUP_N)
            s.warmup_ms[s.warmup_count++] = ms;
        s.sum_ms += ms;
        if (ms < s.min_ms) s.min_ms = ms;
        if (ms > s.max_ms) s.max_ms = ms;
        ++s.frames;
        timer_pending_ = false;
    }

    int fd = buf->planes()[0].fd.get();
    auto it = fd_cache_.find(fd);
    if (it == fd_cache_.end()) {
        std::cerr << "[gpu] unknown fd=" << fd << " — frame skipped\n";
        return;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, it->second.texture);

    if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }

    glFlush(); // submit to GPU; don't stall CPU
}

bool GpuRenderer::save_snapshot(const char *path)
{
    // glFlush was already called in render_frame(); wait for GPU to finish.
    glFinish();

    std::vector<uint8_t> pixels((size_t)W_ * (size_t)H_ * 4);
    double t0 = now_ms();
    glReadPixels(0, 0, W_, H_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::fprintf(stderr, "[gpu] glReadPixels error: 0x%x\n", err);
        return false;
    }
    std::printf("[perf] glReadPixels : %.1f ms\n", now_ms() - t0);

    // Write PNG — rows are bottom-to-top from glReadPixels, flip for PNG.
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("[gpu] fopen"); return false; }

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               nullptr, nullptr, nullptr);
    png_infop   info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)W_, (png_uint_32)H_, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, 1); // fast encode, larger file
    png_write_info(png, info);

    for (int y = H_ - 1; y >= 0; --y)
        png_write_row(png,
            reinterpret_cast<png_const_bytep>(pixels.data() + (size_t)y * W_ * 4));

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);

    std::printf("[gpu] snapshot saved → %s\n", path);
    return true;
}

void GpuRenderer::cleanup()
{
    if (!egl_) return;

    for (auto &[fd, res] : fd_cache_) {
        if (res.texture) glDeleteTextures(1, &res.texture);
        if (res.image != EGL_NO_IMAGE_KHR)
            pfn_DestroyImage(egl_->dpy, res.image);
    }
    fd_cache_.clear();

    if (timer_query_ && pfn_DelQueries) { pfn_DelQueries(1, &timer_query_); timer_query_ = 0; }
    if (fbo_)  { glDeleteFramebuffers(1,  &fbo_);  fbo_  = 0; }
    if (rbo_)  { glDeleteRenderbuffers(1, &rbo_);  rbo_  = 0; }
    if (prog_) { glDeleteProgram(prog_);            prog_ = 0; }

    egl_ = nullptr;
}
