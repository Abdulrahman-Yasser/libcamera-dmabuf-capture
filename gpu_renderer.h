#pragma once

#include "egl_context.h"

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

    bool init(const EGLState &egl,
              const libcamera::StreamConfiguration &sc,
              const std::vector<std::unique_ptr<libcamera::FrameBuffer>> &buffers);

    void render_frame(const libcamera::FrameBuffer *buf);

    bool save_snapshot(const char *path);

    const GpuStats &gpu_stats() const { return gpu_stats_; }

    void cleanup();
    ~GpuRenderer() { cleanup(); }

private:
    bool        load_ext_fns();
    EGLImageKHR create_egl_image(const libcamera::FrameBuffer *buf);

    const EGLState *egl_    = nullptr;
    int             W_      = 0;
    int             H_      = 0;
    int             stride_ = 0;

    GLuint prog_ = 0;
    GLuint fbo_  = 0;
    GLuint rbo_  = 0;

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
