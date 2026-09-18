// C ABI of libbev_view.so, the ivi-homescreen platform-view face of this
// pipeline. Dart (flutter/bev_view, via @Native) is the only consumer.
//
// The library publishes the "views/bev-view" ihs_pv platform view. Nothing
// loads it at the point the shell scans for factories, so the Dart side calls
// bev_register_ihs_pv() once before creating a view: that forces the load and
// installs the factory against the shell's host.
//
// A view's creation params are UTF-8 "key=value" lines (see bev_params.h), sent
// raw as the platform view's params bytes.
//
// Every bev_view_* control addresses a live view by its platform-view id, is
// safe from any thread, and is applied on that view's render thread before its
// next frame. An unknown id is ignored.

#ifndef BEV_VIEW_API_H_
#define BEV_VIEW_API_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BEV_EXPORT __attribute__((visibility("default")))

// Who allocated the buffers a view renders into. Values of BevViewStats.allocator.
enum {
    BEV_ALLOCATOR_NONE = 0,
    BEV_ALLOCATOR_VULKAN = 1,       // exportable images on the shell's Vulkan device
    BEV_ALLOCATOR_SHELL_GBM = 2,    // scanout-capable bos on the shell's gbm device (DRM backends)
    BEV_ALLOCATOR_RENDER_NODE = 3,  // bos on the view's own render node (wayland-egl)
};

typedef struct BevViewStats {
    uint64_t frames_rendered;   // frames the pipeline produced
    uint64_t frames_submitted;  // frames the shell accepted
    double fps;                 // submitted frames over the last ~1 s
    double overlap;             // surround mode's current seam controls; 0 otherwise
    double blend_edge;
    int32_t source_width;       // pipeline output (BEV canvas / camera frame)
    int32_t source_height;
    int32_t view_width;         // ring buffer size, physical pixels
    int32_t view_height;
    uint32_t granted_kind;      // IhsPvKind of the current grant
    uint32_t fourcc;            // DRM fourcc of the ring
    uint64_t modifier;          // DRM format modifier of the ring
    int32_t allocator;          // BEV_ALLOCATOR_*
    int32_t running;            // 1 while the pipeline is producing frames

    // Release-fence accounting, for the compositor side of a tear.
    //
    // A producer waits on the compositor's release fence before drawing over a
    // ring slot. Read them as a pair: zero timeouts proves nothing on its own,
    // because a run that never waited reports zero too. The healthy check is
    // waits > 0 && timeouts == 0.
    //
    // Timeouts mean the release never arrived and the slot was redrawn anyway
    // -- a tear beats a hang -- which on a scanned-out buffer is visible
    // tearing. ivi-homescreen #530 is this: on the GL-composited path a frame
    // is "delivered" but never reaches a plane, so its eventfd is never
    // signalled.
    uint64_t release_waits;
    uint64_t release_timeouts;
    double release_wait_ms;     // total time spent waiting
} BevViewStats;

// Install the "views/bev-view" factory. Platform-thread only; idempotent.
BEV_EXPORT void bev_register_ihs_pv(void);

// Remove the factory. Live views keep running until disposed.
BEV_EXPORT void bev_unregister_ihs_pv(void);

// Copy view @view_id's counters into @out. Returns 1, or 0 for an unknown view.
BEV_EXPORT int32_t bev_view_stats(int64_t view_id, BevViewStats *out);

// Surround mode seam blend width: degrees of angular half-width, 1..360.
BEV_EXPORT void bev_view_set_overlap(int64_t view_id, double value);

// Surround mode crossover sharpness, 0 (gradual) .. 0.49 (near-instant cut).
BEV_EXPORT void bev_view_set_blend_edge(int64_t view_id, double value);

// Surround mode: ignore each camera's facing wedge (testing aid).
BEV_EXPORT void bev_view_set_free_yaw(int64_t view_id, int32_t enable);

// Surround mode: move the car icon, world meters from the canvas centre.
BEV_EXPORT void bev_view_set_car_center(int64_t view_id, double x_m, double y_m);

// Queue a PNG snapshot of the pipeline output to @path. Returns 1 if queued.
BEV_EXPORT int32_t bev_view_snapshot(int64_t view_id, const char *path);

#ifdef __cplusplus
}  // extern "C"
#endif

#endif  // BEV_VIEW_API_H_
