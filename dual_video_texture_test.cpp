// dual_video_texture_test.cpp
//
// Step 1 validation harness: decode two pre-recorded H.264 .mp4 files on the
// CPU with FFmpeg, upload each stream to its own GL texture in a headless
// EGL/GBM (surfaceless) context, render both side-by-side into an FBO, and
// dump periodic PNG snapshots to prove both streams decode and update
// independently. No warping/stitching here — that's a later step.
//
// Standalone: does not include/import any of the existing Stage 1-3 files.

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <gbm.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "extern/stb_image_write.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

// ── Config ──────────────────────────────────────────────────────────────────

static constexpr int   kSaveEveryNFrames = 30;
static constexpr int   kNumSnapshots     = 5;
static const char      kOutDir[]         = "out";

// ── EGL/GBM headless context ─────────────────────────────────────────────────

struct EGLHeadless {
    int         drm_fd  = -1;
    gbm_device *gbm_dev = nullptr;
    EGLDisplay  dpy     = EGL_NO_DISPLAY;
    EGLContext  ctx     = EGL_NO_CONTEXT;
};

static bool has_ext(const char *haystack, const char *needle)
{
    return haystack && needle && strstr(haystack, needle) != nullptr;
}

static bool setup_egl_headless(EGLHeadless &egl)
{
    egl.drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (egl.drm_fd < 0) {
        perror("[egl] open /dev/dri/renderD128");
        return false;
    }

    egl.gbm_dev = gbm_create_device(egl.drm_fd);
    if (!egl.gbm_dev) {
        std::cerr << "[egl] gbm_create_device failed\n";
        return false;
    }
    std::cout << "[egl] GBM backend: " << gbm_device_get_backend_name(egl.gbm_dev) << "\n";

    auto get_platform_display = reinterpret_cast<PFNEGLGETPLATFORMDISPLAYEXTPROC>(
        eglGetProcAddress("eglGetPlatformDisplayEXT"));
    if (get_platform_display) {
        egl.dpy = get_platform_display(EGL_PLATFORM_GBM_MESA,
                                        static_cast<void *>(egl.gbm_dev), nullptr);
    } else {
        egl.dpy = eglGetDisplay(reinterpret_cast<EGLNativeDisplayType>(egl.gbm_dev));
    }
    if (egl.dpy == EGL_NO_DISPLAY) {
        std::cerr << "[egl] failed to get EGL display\n";
        return false;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(egl.dpy, &major, &minor)) {
        std::cerr << "[egl] eglInitialize failed: 0x" << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }
    std::cout << "[egl] EGL version : " << major << "." << minor << "\n";
    std::cout << "[egl] EGL vendor  : " << eglQueryString(egl.dpy, EGL_VENDOR)  << "\n";

    const char *egl_exts = eglQueryString(egl.dpy, EGL_EXTENSIONS);
    if (!has_ext(egl_exts, "EGL_KHR_surfaceless_context")) {
        std::cerr << "[egl] EGL_KHR_surfaceless_context not supported (driver too old)\n";
        return false;
    }

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        std::cerr << "[egl] eglBindAPI(EGL_OPENGL_ES_API) failed: 0x"
                  << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    static const EGLint cfg_attribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_NONE
    };
    EGLConfig cfg   = nullptr;
    EGLint    n_cfg = 0;
    if (!eglChooseConfig(egl.dpy, cfg_attribs, &cfg, 1, &n_cfg) || n_cfg == 0) {
        std::cerr << "[egl] no EGL config with EGL_OPENGL_ES3_BIT\n";
        return false;
    }

    // Request GLES 3.2 explicitly (needed for #version 320 es).
    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 2,
        EGL_NONE
    };
    egl.ctx = eglCreateContext(egl.dpy, cfg, EGL_NO_CONTEXT, ctx_attribs);
    if (egl.ctx == EGL_NO_CONTEXT) {
        std::cerr << "[egl] eglCreateContext failed: 0x" << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    if (!eglMakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl.ctx)) {
        std::cerr << "[egl] eglMakeCurrent (surfaceless) failed: 0x"
                  << std::hex << eglGetError() << std::dec << "\n";
        return false;
    }

    std::cout << "[egl] GL renderer : " << glGetString(GL_RENDERER) << "\n";
    std::cout << "[egl] GL version  : " << glGetString(GL_VERSION)  << "\n";
    return true;
}

static void teardown_egl_headless(EGLHeadless &egl)
{
    if (egl.dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(egl.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (egl.ctx != EGL_NO_CONTEXT) eglDestroyContext(egl.dpy, egl.ctx);
        eglTerminate(egl.dpy);
        eglReleaseThread();
    }
    if (egl.gbm_dev) gbm_device_destroy(egl.gbm_dev);
    if (egl.drm_fd >= 0) close(egl.drm_fd);
    egl = {};
}

static bool check_gl_error(const char *where)
{
    bool ok = true;
    GLenum err;
    while ((err = glGetError()) != GL_NO_ERROR) {
        std::cerr << "[gl] error 0x" << std::hex << err << std::dec << " at " << where << "\n";
        ok = false;
    }
    return ok;
}

// ── Shaders ───────────────────────────────────────────────────────────────────

static const char kVertexShaderSrc[] = R"glsl(
#version 320 es
layout(location = 0) in vec2 aPos;
layout(location = 1) in vec2 aTexCoord;
out vec2 vTexCoord;
void main() {
    vTexCoord   = aTexCoord;
    gl_Position = vec4(aPos, 0.0, 1.0);
}
)glsl";

static const char kFragmentShaderSrc[] = R"glsl(
#version 320 es
precision mediump float;
in vec2 vTexCoord;
uniform sampler2D uTex;
out vec4 fragColor;
void main() {
    fragColor = texture(uTex, vTexCoord);
}
)glsl";

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, nullptr);
    glCompileShader(shader);

    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
        std::cerr << "[shader] compile error: " << log << "\n";
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }

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
        std::cerr << "[program] link error: " << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Fullscreen quad in clip space, texcoord v=0 at the top (matches FFmpeg's
// top-down decoded row order) so glReadPixels + stbi's vertical flip on
// write produce a right-side-up PNG.
static GLuint build_quad_vao(GLuint &vbo_out)
{
    static const float verts[] = {
        // pos          // uv
        -1.f, -1.f,     0.f, 1.f,
         1.f, -1.f,     1.f, 1.f,
        -1.f,  1.f,     0.f, 0.f,
         1.f,  1.f,     1.f, 0.f,
    };

    GLuint vao = 0, vbo = 0;
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);

    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void *)(2 * sizeof(float)));

    glBindVertexArray(0);
    vbo_out = vbo;
    return vao;
}

// ── GL texture helpers ────────────────────────────────────────────────────────

// Allocates storage once; caller uploads pixel data separately via
// update_texture(). Kept apart so the texture object is created exactly once.
static GLuint create_rgba_texture(int w, int h)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    check_gl_error("create_rgba_texture");
    return tex;
}

// Reuses the existing texture object (glTexSubImage2D) — no realloc per frame.
// row_stride_bytes may exceed w*4 (av_image_alloc pads rows for SIMD safety),
// so GL_UNPACK_ROW_LENGTH must be told the real stride, not just the width.
static void update_texture(GLuint tex, int w, int h, const uint8_t *rgba, int row_stride_bytes)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, row_stride_bytes / 4);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    check_gl_error("update_texture");
}

static bool create_color_fbo(GLuint &fbo, GLuint &color_tex, int w, int h)
{
    color_tex = create_rgba_texture(w, h);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[gl] FBO incomplete: 0x" << std::hex << status << std::dec << "\n";
        return false;
    }
    return true;
}

static void draw_textured_quad(GLuint prog, GLuint vao, GLuint tex, int vp_x, int vp_y, int vp_w, int vp_h)
{
    glViewport(vp_x, vp_y, vp_w, vp_h);
    glUseProgram(prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);
}

// ── FFmpeg decode ─────────────────────────────────────────────────────────────

struct VideoStream {
    std::string      path;
    AVFormatContext *fmt_ctx        = nullptr;
    AVCodecContext  *dec_ctx        = nullptr;
    SwsContext      *sws_ctx        = nullptr;
    AVFrame         *frame          = nullptr;
    AVPacket        *pkt            = nullptr;
    int              stream_idx     = -1;
    int              width          = 0;
    int              height         = 0;
    AVPixelFormat    pix_fmt        = AV_PIX_FMT_NONE;

    // RGBA scratch buffer, valid after first decode. Allocated via
    // av_image_alloc rather than a tightly-sized std::vector: sws_scale's
    // SIMD paths can write a few bytes past the exact width*height*4 bound
    // when a dimension isn't aligned to its internal block size (e.g. a
    // height that isn't a multiple of 16), corrupting the heap. av_image_alloc
    // pads rows/buffer to a safe alignment, so rgba_linesize may exceed
    // width*4 — always use it (not width*4) as the GL upload row length.
    uint8_t         *rgba_data[4]     = { nullptr, nullptr, nullptr, nullptr };
    int              rgba_linesize[4] = { 0, 0, 0, 0 };
};

static std::string av_err_str(int errnum)
{
    char buf[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(errnum, buf, sizeof(buf));
    return std::string(buf);
}

static bool open_video(VideoStream &vs, const std::string &path)
{
    vs.path = path;

    int ret = avformat_open_input(&vs.fmt_ctx, path.c_str(), nullptr, nullptr);
    if (ret < 0) {
        std::cerr << "[ffmpeg] avformat_open_input(" << path << ") failed: " << av_err_str(ret) << "\n";
        return false;
    }

    ret = avformat_find_stream_info(vs.fmt_ctx, nullptr);
    if (ret < 0) {
        std::cerr << "[ffmpeg] avformat_find_stream_info(" << path << ") failed: " << av_err_str(ret) << "\n";
        return false;
    }

    // av_find_best_stream's decoder_ret parameter became `const AVCodec **`
    // in FFmpeg 5.0 (libavformat 59); older releases want `AVCodec **`.
#if LIBAVFORMAT_VERSION_MAJOR >= 59
    const AVCodec *decoder = nullptr;
#else
    AVCodec *decoder = nullptr;
#endif
    ret = av_find_best_stream(vs.fmt_ctx, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (ret < 0) {
        std::cerr << "[ffmpeg] no video stream in " << path << ": " << av_err_str(ret) << "\n";
        return false;
    }
    vs.stream_idx = ret;

    vs.dec_ctx = avcodec_alloc_context3(decoder);
    if (!vs.dec_ctx) {
        std::cerr << "[ffmpeg] avcodec_alloc_context3 failed for " << path << "\n";
        return false;
    }

    ret = avcodec_parameters_to_context(vs.dec_ctx, vs.fmt_ctx->streams[vs.stream_idx]->codecpar);
    if (ret < 0) {
        std::cerr << "[ffmpeg] avcodec_parameters_to_context failed: " << av_err_str(ret) << "\n";
        return false;
    }

    vs.dec_ctx->thread_count = 1;  // single-threaded: simpler, deterministic decode for this harness

    ret = avcodec_open2(vs.dec_ctx, decoder, nullptr);
    if (ret < 0) {
        std::cerr << "[ffmpeg] avcodec_open2 failed for " << path << ": " << av_err_str(ret) << "\n";
        return false;
    }

    vs.frame = av_frame_alloc();
    vs.pkt   = av_packet_alloc();
    if (!vs.frame || !vs.pkt) {
        std::cerr << "[ffmpeg] av_frame_alloc/av_packet_alloc failed\n";
        return false;
    }

    std::cout << "[ffmpeg] opened " << path << " (decoder: " << decoder->name << ")\n";
    return true;
}

// Decodes the next frame, converts to RGBA into vs.rgba, and loops the video
// back to the start on EOF so playback continues indefinitely. Returns false
// only on unrecoverable setup failure (e.g. sws_getContext failing).
static bool decode_next_rgba_frame(VideoStream &vs)
{
    while (true) {
        int ret = av_read_frame(vs.fmt_ctx, vs.pkt);
        if (ret < 0) {
            // EOF (or transient read error) — loop back to the start.
            av_seek_frame(vs.fmt_ctx, vs.stream_idx, 0, AVSEEK_FLAG_BACKWARD);
            avcodec_flush_buffers(vs.dec_ctx);
            continue;
        }

        if (vs.pkt->stream_index != vs.stream_idx) {
            av_packet_unref(vs.pkt);
            continue;
        }

        ret = avcodec_send_packet(vs.dec_ctx, vs.pkt);
        av_packet_unref(vs.pkt);
        if (ret < 0 && ret != AVERROR(EAGAIN)) {
            std::cerr << "[ffmpeg] avcodec_send_packet(" << vs.path << "): " << av_err_str(ret) << "\n";
            continue;
        }

        ret = avcodec_receive_frame(vs.dec_ctx, vs.frame);
        if (ret == AVERROR(EAGAIN)) continue;  // need another packet
        if (ret < 0) {
            std::cerr << "[ffmpeg] avcodec_receive_frame(" << vs.path << "): " << av_err_str(ret) << "\n";
            continue;
        }

        // Got a decoded frame.
        if (!vs.sws_ctx) {
            vs.width   = vs.frame->width;
            vs.height  = vs.frame->height;
            vs.pix_fmt = static_cast<AVPixelFormat>(vs.frame->format);

            int ret2 = av_image_alloc(vs.rgba_data, vs.rgba_linesize,
                                       vs.width, vs.height, AV_PIX_FMT_RGBA, 32);
            if (ret2 < 0) {
                std::cerr << "[ffmpeg] av_image_alloc failed for " << vs.path
                           << ": " << av_err_str(ret2) << "\n";
                return false;
            }

            vs.sws_ctx = sws_getContext(vs.width, vs.height, vs.pix_fmt,
                                         vs.width, vs.height, AV_PIX_FMT_RGBA,
                                         SWS_BILINEAR, nullptr, nullptr, nullptr);
            if (!vs.sws_ctx) {
                std::cerr << "[ffmpeg] sws_getContext failed for " << vs.path << "\n";
                return false;
            }
        }

        sws_scale(vs.sws_ctx, vs.frame->data, vs.frame->linesize, 0, vs.height,
                  vs.rgba_data, vs.rgba_linesize);

        av_frame_unref(vs.frame);
        return true;
    }
}

static void close_video(VideoStream &vs)
{
    if (vs.rgba_data[0]) av_freep(&vs.rgba_data[0]);
    if (vs.sws_ctx) sws_freeContext(vs.sws_ctx);
    if (vs.frame)   av_frame_free(&vs.frame);
    if (vs.pkt)     av_packet_free(&vs.pkt);
    if (vs.dec_ctx) avcodec_free_context(&vs.dec_ctx);
    if (vs.fmt_ctx) avformat_close_input(&vs.fmt_ctx);
    vs = {};
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char **argv)
{
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <left.mp4> <right.mp4>\n";
        return 1;
    }

    if (mkdir(kOutDir, 0755) != 0 && errno != EEXIST) {
        perror("[main] mkdir(out)");
        return 1;
    }

    VideoStream left, right;
    if (!open_video(left, argv[1]) || !open_video(right, argv[2])) {
        return 1;
    }

    // Prime both decoders with their first frame so we know the ACTUAL
    // decoded width/height/pix_fmt before creating any GL resources.
    if (!decode_next_rgba_frame(left) || !decode_next_rgba_frame(right)) {
        std::cerr << "[main] failed to decode first frame\n";
        return 1;
    }

    std::cout << "[video] left  \"" << argv[1] << "\": " << left.width  << "x" << left.height
               << " (" << av_get_pix_fmt_name(left.pix_fmt)  << ")\n";
    std::cout << "[video] right \"" << argv[2] << "\": " << right.width << "x" << right.height
               << " (" << av_get_pix_fmt_name(right.pix_fmt) << ")\n";

    EGLHeadless egl;
    if (!setup_egl_headless(egl)) {
        std::cerr << "[main] EGL/GBM setup failed\n";
        return 1;
    }

    GLuint prog = build_program(kVertexShaderSrc, kFragmentShaderSrc);
    if (!prog) {
        std::cerr << "[main] shader build failed\n";
        return 1;
    }

    GLuint quad_vbo = 0;
    GLuint quad_vao = build_quad_vao(quad_vbo);

    GLuint tex_left  = create_rgba_texture(left.width,  left.height);
    GLuint tex_right = create_rgba_texture(right.width, right.height);
    update_texture(tex_left,  left.width,  left.height,  left.rgba_data[0],  left.rgba_linesize[0]);
    update_texture(tex_right, right.width, right.height, right.rgba_data[0], right.rgba_linesize[0]);

    const int fbo_w = left.width + right.width;
    const int fbo_h = std::max(left.height, right.height);

    GLuint fbo = 0, fbo_color = 0;
    if (!create_color_fbo(fbo, fbo_color, fbo_w, fbo_h)) {
        std::cerr << "[main] FBO creation failed\n";
        return 1;
    }
    std::cout << "[gl] side-by-side FBO: " << fbo_w << "x" << fbo_h << "\n";

    std::vector<uint8_t> readback(static_cast<size_t>(fbo_w) * fbo_h * 4);
    stbi_flip_vertically_on_write(1);  // glReadPixels rows are bottom-up

    int saved = 0;
    int frame_idx = 0;
    while (saved < kNumSnapshots) {
        if (frame_idx > 0) {
            // Frame 0 was already decoded during priming above.
            if (!decode_next_rgba_frame(left) || !decode_next_rgba_frame(right)) {
                std::cerr << "[main] decode failed at frame " << frame_idx << "\n";
                return 1;
            }
            update_texture(tex_left,  left.width,  left.height,  left.rgba_data[0],  left.rgba_linesize[0]);
            update_texture(tex_right, right.width, right.height, right.rgba_data[0], right.rgba_linesize[0]);
        }

        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        draw_textured_quad(prog, quad_vao, tex_left,  0,            0, left.width,  fbo_h);
        draw_textured_quad(prog, quad_vao, tex_right, left.width,   0, right.width, fbo_h);

        if (frame_idx % kSaveEveryNFrames == 0) {
            glFinish();
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glReadPixels(0, 0, fbo_w, fbo_h, GL_RGBA, GL_UNSIGNED_BYTE, readback.data());
            if (!check_gl_error("glReadPixels")) {
                std::cerr << "[main] glReadPixels failed at frame " << frame_idx << "\n";
                return 1;
            }

            char path[128];
            std::snprintf(path, sizeof(path), "%s/sidebyside_%04d.png", kOutDir, frame_idx);
            if (!stbi_write_png(path, fbo_w, fbo_h, 4, readback.data(), fbo_w * 4)) {
                std::cerr << "[main] stbi_write_png failed for " << path << "\n";
                return 1;
            }

            std::cout << "[frame " << frame_idx << "] saved " << path
                       << " | left " << left.width  << "x" << left.height
                       << " | right " << right.width << "x" << right.height << "\n";
            ++saved;
        }

        ++frame_idx;
    }

    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &fbo_color);
    glDeleteTextures(1, &tex_left);
    glDeleteTextures(1, &tex_right);
    glDeleteBuffers(1, &quad_vbo);
    glDeleteVertexArrays(1, &quad_vao);
    glDeleteProgram(prog);
    teardown_egl_headless(egl);

    close_video(left);
    close_video(right);

    std::cout << "[main] done — " << saved << " PNG(s) written to " << kOutDir << "/\n";
    return 0;
}
