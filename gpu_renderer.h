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

    // Adjust the seam-stitch blend zone half-width at runtime — meters in
    // dual mode (centered on the baseline midline, world X=0), degrees of
    // angular half-width in multi mode (centered on each camera's facing
    // bearing). Pushes into whichever of prog_dual_/prog_multi_/
    // prog_pyr_warp_ is built (the pyramid warp pass computes the same
    // per-camera angular weight kFS_MULTI does, at level 0).
    void set_stitch_overlap(float half_width);
    // Controls crossover sharpness: 0=gradual, 0.49=near-instant cut. Shared
    // meaning/formula in dual, multi, and multi-pyramid modes.
    void set_blend_edge(float edge);
    // Testing escape hatch (multi/multi-pyramid only, no-op for dual, which
    // has no facing/wedge concept): when on, every camera's blend weight is
    // forced to 1 wherever its own homography validly maps a fragment,
    // ignoring facing/uOverlap/uBlendEdge entirely -- lets yaw be swept
    // through the full 360 degrees during testing without the coverage
    // wedge hiding content outside a camera's real physical facing sector.
    void set_free_yaw(bool on);

    // Uploads the per-camera BEV-pixel -> camera-image homographies built by
    // ground_to_image_H() (see ipm.h). Column-major float[9], dual mode only.
    void set_ipm(const float H_left[9], const float H_right[9]);
    // BEV ground-sampling density, pixels per meter. This only updates the
    // GPU-side uniform for readback/debug -- the actual geometry change
    // requires rebuilding the homographies on the CPU and re-uploading them
    // (set_ipm()/set_bev()/set_ipm_multi()/set_ipm_multi_pyramid()), since
    // px_per_m is baked into those matrices. Shared/global uniform — pushes
    // into whichever of prog_dual_/prog_multi_/prog_pyr_warp_ is built.
    void set_px_per_m(float px_per_m);

    // Single-camera forward BEV (single-file mode only) — the same
    // ground_to_image_H() backward-warp idea as the dual-camera stitch
    // above, just one camera and no blend. Kept as its own program/method
    // pair (rather than a "camera count" flag bolted onto kFS_DUAL) so it
    // composes cleanly with the planned front/back/left/right 4-camera BEV
    // blend later, instead of being a throwaway shortcut.
    bool init_bev();  // compiles the single-camera BEV shader; call once after init().
    void set_bev_enabled(bool on) { bev_enabled_ = on; }
    bool bev_enabled() const { return bev_enabled_; }
    void set_bev(const float H[9]);  // column-major, from ground_to_image_H()

    // N-camera surround-view BEV (--src mode): every camera independently
    // positioned/oriented, warped via its own homography, and blended by an
    // angular weight (see kFS_MULTI / BEV_ALGORITHM.md) instead of kFS_DUAL's
    // single-axis 2-camera blend. Hard compile-time cap: GLES 3.0 only
    // guarantees 16 texture image units, i.e. 8 camera pairs (Y+UV each).
    static constexpr int kMaxCameras = 8;

    bool init_multi(const EGLState &egl, int w, int h, int stride, int num_cameras);
    void render_frame_multi(const std::vector<DmaBufFrame> &frames);
    // Uploads one slot's homography + facing bearing (degrees, see
    // BEV_ALGORITHM.md's yaw->facing formula) — cheap, called per keypress
    // for just the changed slot, not all N.
    void set_ipm_multi(int slot, const float H[9], float facing_deg);
    int  num_cameras_multi() const { return num_cameras_multi_; }

    // N-camera surround-view BEV, multi-band (Laplacian pyramid) blend —
    // a separate, comparable alternative to init_multi()/render_frame_multi()
    // above (kFS_MULTI's single-pass angular-weighted "feathering" blend).
    // Blends different spatial-frequency bands with different blend-zone
    // widths, so it can hide brightness/color seam mismatch that a single
    // blend width cannot. Genuine multi-pass GPU pipeline (warp -> Gaussian
    // pyramid -> Laplacian pyramid -> per-level cross-camera blend ->
    // reconstruct) instead of one draw call, so the camera/level counts are
    // deliberately capped lower than kMaxCameras for this first cut —
    // confirm before raising either.
    static constexpr int kPyramidMaxCameras = 4;
    static constexpr int kPyramidLevels     = 3;

    // Requires a real color-renderable RGBA16F target (checked here via an
    // actual glCheckFramebufferStatus() probe at every pyramid level's
    // resolution, not just a GL_EXT_color_buffer_half_float/_float
    // extension-string check) — returns false with a specific stderr
    // message if that's not usable on this driver. No RGBA8 fallback is
    // implemented; that's the next thing to build if this ever fails on
    // real hardware, not something hedged against upfront.
    bool init_multi_pyramid(const EGLState &egl, int w, int h, int stride,
                            int num_cameras, int num_levels = kPyramidLevels);
    void render_frame_multi_pyramid(const std::vector<DmaBufFrame> &frames);
    // Same cheap, per-keypress contract as set_ipm_multi().
    void set_ipm_multi_pyramid(int slot, const float H[9], float facing_deg);
    int  num_cameras_multi_pyramid() const { return num_cameras_multi_pyramid_; }

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
    GLuint prog_bev_  = 0;   // sampler2D single-camera BEV path (single file)
    GLint  bev_u_H_     = -1;
    bool   bev_enabled_ = false;
    GLuint fbo_     = 0;
    GLuint fbo_tex_ = 0;   // FBO color attachment (GL_TEXTURE_2D, sampleable)

    // Camera 0 textures (single and dual modes).
    GLuint tex_y_   = 0;
    GLuint tex_uv_  = 0;
    // Camera 1 textures (dual mode only).
    GLuint tex_y2_  = 0;
    GLuint tex_uv2_ = 0;

    // N-camera surround-view (--src mode). One texture pair per slot, plus
    // cached uniform locations per slot (looked up once in init_multi(), not
    // re-queried by name on every set_ipm_multi() call — these get called
    // once per keypress per slot, so caching avoids a glGetUniformLocation
    // round trip on every tune).
    GLuint prog_multi_ = 0;
    GLuint tex_y_multi_[kMaxCameras]  = {};
    GLuint tex_uv_multi_[kMaxCameras] = {};
    GLint  u_H_multi_[kMaxCameras]      = {};
    GLint  u_facing_multi_[kMaxCameras] = {};
    int    num_cameras_multi_ = 0;

    // N-camera surround-view, multi-band (Laplacian pyramid) blend mode.
    // Own programs and own per-camera source textures — deliberately
    // independent of prog_multi_/tex_y_multi_ above, so this path never
    // requires init_multi() to have been called at all.
    GLuint prog_pyr_warp_       = 0;   // per-camera: homography warp + angular weight -> RGBA16F
    GLuint prog_pyr_downsample_ = 0;   // 5x5 Gaussian blur+downsample, one level
    GLuint prog_pyr_laplace_    = 0;   // Laplacian level = G_l - upsample(G_l+1)
    GLuint prog_pyr_blend_      = 0;   // per-level, cross-camera weighted blend
    GLuint prog_pyr_recon_      = 0;   // upsample(result_l+1) + blended_l
    GLuint fbo_pyr_             = 0;   // one reusable FBO, re-attached per pass

    GLuint tex_y_pyr_[kPyramidMaxCameras]  = {};
    GLuint tex_uv_pyr_[kPyramidMaxCameras] = {};
    // [camera][level] Gaussian pyramid: rgb=warped color, a=blend weight.
    GLuint tex_pyr_gauss_[kPyramidMaxCameras][kPyramidLevels]     = {};
    // [camera][level] Laplacian (band-pass) levels, levels 0..K-2 only.
    GLuint tex_pyr_lap_[kPyramidMaxCameras][kPyramidLevels - 1]   = {};
    // [level] cross-camera-blended result at that level, pre-reconstruction.
    GLuint tex_pyr_blended_[kPyramidLevels]                       = {};
    // [level] reconstruction chain output, levels 0..K-2 (level 0 could
    // write straight to fbo_tex_, but is kept as its own texture too so the
    // reconstruction loop's last iteration isn't a special case).
    GLuint tex_pyr_result_[kPyramidLevels - 1]                    = {};

    // prog_pyr_warp_ draws ONE camera at a time (reusing the same program,
    // same "uH"/"uFacingDeg" uniforms, for each of the N per-camera warp
    // passes) rather than kFS_MULTI's one-draw-does-all-N-cameras approach
    // -- so unlike u_H_multi_/u_facing_multi_ above, there's only one
    // uniform location to cache (looked up once in init_multi_pyramid()),
    // and the actual per-slot VALUES must be kept on the CPU side so
    // render_frame_multi_pyramid() can re-upload the right one before each
    // camera's warp draw call.
    GLint u_H_pyr_loc_      = -1;
    GLint u_facing_pyr_loc_ = -1;
    float H_pyr_[kPyramidMaxCameras][9] = {};
    float facing_pyr_[kPyramidMaxCameras] = {};
    // Downsample pass's texel-size uniform changes per pyramid level, every
    // frame -- cached for the same reason as u_H_pyr_loc_ above. Every other
    // pyramid-shader uniform (sampler unit assignments, uNumCameras) is set
    // once in init_multi_pyramid() and never touched again, so needs no
    // cached location.
    GLint u_pyr_texel_loc_ = -1;

    int num_cameras_multi_pyramid_ = 0;
    int num_levels_multi_pyramid_  = 0;

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
