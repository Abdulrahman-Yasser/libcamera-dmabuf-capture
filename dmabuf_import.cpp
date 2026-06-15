#include "dmabuf_import.h"

#include <EGL/eglext.h>
#include <GLES3/gl3.h>       /* GL_RGBA8, glReadPixels, etc. */
#include <GLES2/gl2ext.h>    /* GL_TEXTURE_EXTERNAL_OES, PFNGLEGLIMAGETARGETTEXTURE2DOESPROC */
#include <drm/drm_fourcc.h>
#include <png.h>

#include <iostream>
#include <vector>

using namespace libcamera;

// Extension function pointers — resolved at runtime via eglGetProcAddress
// because KHR/OES/EXT symbols are not guaranteed exported from libEGL.so.
static PFNEGLCREATEIMAGEKHRPROC            pfn_CreateImage   = nullptr;
static PFNEGLDESTROYIMAGEKHRPROC           pfn_DestroyImage  = nullptr;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_TexImage2DOES = nullptr;

static bool load_ext_fns()
{
    pfn_CreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    pfn_DestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    pfn_TexImage2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!pfn_CreateImage || !pfn_DestroyImage || !pfn_TexImage2DOES) {
        std::cerr << "[dmabuf] failed to resolve EGL/GL extension functions\n";
        return false;
    }
    return true;
}

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
        std::cerr << "[dmabuf] shader compile error:\n" << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
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
        std::cerr << "[dmabuf] program link error:\n" << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// Save RGBA pixels as PNG.
// glReadPixels returns rows bottom-to-top; PNG expects top-to-bottom,
// so rows are written in reverse order.
static bool save_png(const char *path, const uint8_t *rgba, int w, int h)
{
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("[dmabuf] fopen"); return false; }

    png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                              nullptr, nullptr, nullptr);
    if (!png) { fclose(fp); return false; }

    png_infop info = png_create_info_struct(png);
    if (!info) {
        png_destroy_write_struct(&png, nullptr);
        fclose(fp);
        return false;
    }

    if (setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)w, (png_uint_32)h, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_write_info(png, info);

    for (int y = h - 1; y >= 0; --y)
        png_write_row(png,
            reinterpret_cast<png_const_bytep>(rgba + (size_t)y * (size_t)w * 4));

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);
    return true;
}

bool import_and_save_png(const EGLState            &egl,
                          const FrameBuffer         *buf,
                          const StreamConfiguration &sc)
{
    std::cerr << "[dmabuf] step 1/7  loading extension functions\n";
    if (!load_ext_fns()) return false;

    const auto &planes = buf->planes();
    const int   W      = (int)sc.size.width;
    const int   H      = (int)sc.size.height;
    const int   stride = (int)sc.stride;

    // ── 1. Single NV12 EGLImage with both planes ─────────────────────────
    // DRM_FORMAT_NV12 with PLANE0 (Y) + PLANE1 (UV) is widely supported by
    // Mesa V3D. Two-component DRM formats like RG88 are not.
    std::cerr << "[dmabuf] step 2/7  creating NV12 EGLImage (fd=" << planes[0].fd.get()
              << " y_off=" << planes[0].offset
              << " uv_off=" << planes[1].offset
              << " pitch=" << stride << " " << W << "x" << H << ")\n";

    const EGLint attrs[] = {
        EGL_WIDTH,                      W,
        EGL_HEIGHT,                     H,
        EGL_LINUX_DRM_FOURCC_EXT,       DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT,      planes[0].fd.get(),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  (EGLint)planes[0].offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   stride,
        EGL_DMA_BUF_PLANE1_FD_EXT,      planes[1].fd.get(),
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,  (EGLint)planes[1].offset,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,   stride,
        EGL_NONE,
    };

    EGLImageKHR image = pfn_CreateImage(egl.dpy, EGL_NO_CONTEXT,
                                         EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (image == EGL_NO_IMAGE_KHR) {
        std::cerr << "[dmabuf] eglCreateImageKHR failed (0x"
                  << std::hex << eglGetError() << std::dec << ")\n";
        return false;
    }
    std::cerr << "[dmabuf] step 2/7  EGLImage OK\n";

    // ── 2. Bind NV12 EGLImage as GL_TEXTURE_EXTERNAL_OES ─────────────────
    // Mesa V3D performs NV12→RGB conversion in hardware on every texture
    // fetch — the shader receives RGB values directly via texture().
    std::cerr << "[dmabuf] step 3/7  binding texture\n";
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pfn_TexImage2DOES(GL_TEXTURE_EXTERNAL_OES, image);

    // ── 3. GLSL ES 3.0 shaders ───────────────────────────────────────────
    // Vertex: oversized triangle via gl_VertexID — no VBO required.
    // Fragment: GL_OES_EGL_image_external_essl3 (the ESSL3 variant) and
    //           texture() returns RGB directly thanks to Mesa V3D hardware
    //           NV12→RGB conversion on the sampler path.
    std::cerr << "[dmabuf] step 4/7  compiling shaders\n";

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
     *  - 3 horizontal lines at 25%, 50%, 75% from bottom
     *  - 2 vertical lines at 33% and 66% from left (lane boundaries)
     *  lw = line half-width in UV space: 0.005 ~ 6 pixels on a 1232-tall image.
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

    GLuint prog = build_program(kVS, kFS);
    if (!prog) {
        glDeleteTextures(1, &tex);
        pfn_DestroyImage(egl.dpy, image);
        return false;
    }
    std::cerr << "[dmabuf] step 4/7  shaders OK\n";

    // ── 4. FBO with GL_RGBA8 renderbuffer ────────────────────────────────
    std::cerr << "[dmabuf] step 5/7  creating FBO\n";
    GLuint fbo = 0, rbo = 0;
    glGenFramebuffers(1,  &fbo);
    glGenRenderbuffers(1, &rbo);
    glBindRenderbuffer(GL_RENDERBUFFER, rbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, W, H);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              GL_RENDERBUFFER, rbo);

    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[dmabuf] FBO incomplete\n";
        glDeleteFramebuffers(1,  &fbo);
        glDeleteRenderbuffers(1, &rbo);
        glDeleteProgram(prog);
        glDeleteTextures(1, &tex);
        pfn_DestroyImage(egl.dpy, image);
        return false;
    }
    std::cerr << "[dmabuf] step 5/7  FBO OK\n";

    // ── 5. Render fullscreen triangle ────────────────────────────────────
    std::cerr << "[dmabuf] step 6/7  rendering\n";
    glViewport(0, 0, W, H);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "uTexture"), 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();

    // ── 6. Read back pixels — DEBUG: remove in Stage 4 ───────────────────
    // glReadPixels is a CPU stall that defeats zero-copy. Only here to
    // verify shader output is visually correct before Stage 4 wiring.
    std::vector<uint8_t> pixels((size_t)W * (size_t)H * 4);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data()); // DEBUG: remove in Stage 4

    GLenum gl_err = glGetError();
    if (gl_err != GL_NO_ERROR)
        std::cerr << "[dmabuf] GL error after glReadPixels: 0x"
                  << std::hex << gl_err << std::dec << "\n";
    std::cerr << "[dmabuf] step 6/7  render + readback OK\n";

    // ── 7. Cleanup GL resources ───────────────────────────────────────────
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDeleteFramebuffers(1,  &fbo);
    glDeleteRenderbuffers(1, &rbo);
    glDeleteProgram(prog);
    glDeleteTextures(1, &tex);
    pfn_DestroyImage(egl.dpy, image);

    // ── 8. Save PNG ───────────────────────────────────────────────────────
    std::cerr << "[dmabuf] step 7/7  saving PNG\n";
    bool ok = save_png("/tmp/frame.png", pixels.data(), W, H);
    if (ok)
        std::cout << "Saved /tmp/frame.png — scp to laptop to verify\n";
    else
        std::cerr << "[dmabuf] save_png failed\n";
    return ok;
}
