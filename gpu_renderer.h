#pragma once

#include "egl_context.h"
#include "dmabuf_frame.h"

#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <libcamera/libcamera.h>

#include <cfloat>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct FrameGLResources {
    EGLImageKHR image   = EGL_NO_IMAGE_KHR;
    GLuint      texture = 0;
};

class GpuRenderer {
public:
    // True GPU execution time collected via GL_EXT_disjoint_timer_query.
    struct GpuStats {
        uint64_t frames     = 0;
        double   sum_ms     = 0.0;
        double   min_ms     = DBL_MAX;
        double   max_ms     = 0.0;
        // First WARMUP_N frames kept separately to show GPU cache warm-up.
        static constexpr int WARMUP_N = 10;
        double   warmup_ms[WARMUP_N] = {};
        int      warmup_count        = 0;
        bool     available           = false; // false if ext not supported
    };

    // Camera mode: pre-caches one EGLImage per buffer in the libcamera pool.
    bool init(const EGLState &egl,
              const libcamera::StreamConfiguration &sc,
              const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers);

    // File mode: single camera, system-memory NV12.
    bool init(const EGLState &egl, int w, int h, int stride);

    // File mode: dual camera, side-by-side output (left | right).
    bool init_dual(const EGLState &egl, int w, int h, int stride);

    void render_frame(const libcamera::FrameBuffer *buf);
    void render_frame(const DmaBufFrame &frame);
    void render_frame(const DmaBufFrame &left, const DmaBufFrame &right);

    // Adjust the seam-stitch blend zone width at runtime (dual mode only).
    // fraction: 0 = no overlap (hard cut), 1 = full blend (both cameras everywhere).
    void set_stitch_overlap(float fraction);
    // Controls crossover sharpness: 0=gradual, 0.49=near-instant cut.
    void set_blend_edge(float edge);

    bool save_snapshot(const char *path);

    // Sampleable color attachment of the render FBO (GL_TEXTURE_2D) — used to
    // composite the rendered frame onto a window surface in preview mode.
    GLuint fbo_texture() const { return fbo_tex_; }
    // Pixel dimensions of that FBO — needed by preview mode to letterbox/
    // pillarbox correctly when the window's aspect ratio doesn't match.
    int    width()  const { return W_; }
    int    height() const { return H_; }

    const GpuStats &gpu_stats() const { return gpu_stats_; }

    void cleanup();
    ~GpuRenderer() { cleanup(); }

private:
    bool        load_ext_fns();
    EGLImageKHR create_egl_image(const libcamera::FrameBuffer *buf);
    EGLImageKHR create_egl_image(const DmaBufFrame &frame);
    void        cache_frame(int fd, EGLImageKHR img);
    void        upload_nv12(const DmaBufFrame &f, GLuint tex_y, GLuint tex_uv,
                            int unit_y, int unit_uv);
    void        collect_timer();

    const EGLState *egl_    = nullptr;
    int             W_      = 0;
    int             H_      = 0;
    int             stride_ = 0;

    GLuint prog_      = 0;   // OES path (camera / DMA-BUF)
    GLuint prog_2d_   = 0;   // sampler2D path (single file)
    GLuint prog_dual_ = 0;   // sampler2D × 4 path (dual file)
    GLuint fbo_     = 0;
    GLuint fbo_tex_ = 0;   // FBO color attachment (GL_TEXTURE_2D, sampleable)

    // Camera 0 textures (single and dual modes).
    GLuint tex_y_   = 0;
    GLuint tex_uv_  = 0;
    // Camera 1 textures (dual mode only).
    GLuint tex_y2_  = 0;
    GLuint tex_uv2_ = 0;

    std::unordered_map<int, FrameGLResources> fd_cache_;

    // GPU timer query state (GL_EXT_disjoint_timer_query).
    // Single-buffered: at 15–30 fps the GPU finishes long before the next frame.
    GLuint    timer_query_   = 0;
    bool      timer_pending_ = false;
    GpuStats  gpu_stats_;

    PFNEGLCREATEIMAGEKHRPROC            pfn_CreateImage   = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC           pfn_DestroyImage  = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC pfn_TexImage2DOES = nullptr;

    // Optional — only loaded if GL_EXT_disjoint_timer_query is present.
    PFNGLGENQUERIESEXTPROC          pfn_GenQueries = nullptr;
    PFNGLDELETEQUERIESEXTPROC       pfn_DelQueries = nullptr;
    PFNGLBEGINQUERYEXTPROC          pfn_BeginQuery = nullptr;
    PFNGLENDQUERYEXTPROC            pfn_EndQuery   = nullptr;
    PFNGLGETQUERYOBJECTUI64VEXTPROC pfn_GetQuery64 = nullptr;
};
