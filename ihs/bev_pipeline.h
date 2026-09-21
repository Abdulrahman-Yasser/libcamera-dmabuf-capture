#pragma once

// The CLI's run modes, as something a platform view can own: opens the sources
// its BevParams name, builds the GpuRenderer, and renders one frame at a time
// into the renderer's FBO. Non-interactive -- the runtime knobs main.cpp binds
// to keys arrive through bev_view_api.h instead.
//
// Everything but interrupt() runs on the view's render thread with its EGL
// context current.

#include "bev_config.h"
#include "bev_params.h"
#include "capture_session.h"
#include "egl_context.h"
#include "file_source.h"
#include "gpu_renderer.h"

#include <libcamera/libcamera.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

class BevPipeline {
public:
    BevPipeline() = default;
    ~BevPipeline() { shutdown(); }

    BevPipeline(const BevPipeline &) = delete;
    BevPipeline &operator=(const BevPipeline &) = delete;

    bool init(const EGLState &egl, const BevParams &params);

    // Renders the next frame. Paces itself: blocks for the camera's next
    // frame, or sleeps to a recording's frame rate. Returns false when the
    // source has nothing more to give (or interrupt() was called).
    bool render_next();

    // Wakes a render_next() blocked on the camera. Any thread.
    void interrupt();

    void shutdown();

    // Seam blend controls. Tracked here because GpuRenderer can't be asked
    // for them back, and the initial values come from the config file.
    void  set_overlap(float value);
    void  set_blend_edge(float value);
    float overlap() const    { return overlap_; }
    float blend_edge() const { return blend_edge_; }

    GpuRenderer *renderer() { return renderer_.get(); }
    GLuint output_texture() const { return renderer_ ? renderer_->fbo_texture() : 0; }
    int    width()  const { return renderer_ ? renderer_->width()  : 0; }
    int    height() const { return renderer_ ? renderer_->height() : 0; }

private:
    struct Slot {
        std::string                 path;
        int                         cfg_slot = 0;
        std::unique_ptr<FileSource> src;
        DmaBufFrame                 frame;

        int width = 0, height = 0, stride = 0;

        std::shared_ptr<libcamera::Camera>               camera;
        std::unique_ptr<libcamera::CameraConfiguration>  config;
        std::unique_ptr<libcamera::FrameBufferAllocator> allocator;
        std::vector<std::unique_ptr<libcamera::Request>> requests;
        libcamera::Stream                               *stream = nullptr;
        std::unique_ptr<CaptureSession>                  session;
        bool                                             acquired = false;
        bool                                             started  = false;
    };

    bool init_camera();
    bool init_file();
    bool init_surround();
    bool init_pattern();

    bool render_camera();
    bool render_file();
    bool render_surround();
    bool render_surround_live();
    bool open_live_slot(Slot &slot, int index);
    bool start_live_cameras();
    void stop_live_cameras();
    void release_live_slots();
    bool render_pattern();

    void pace(double frame_duration_ms);
    static bool advance(FileSource &src, const std::string &path, DmaBufFrame &frame);

    const EGLState              *egl_ = nullptr;
    BevParams                    params_;
    std::unique_ptr<GpuRenderer> renderer_;
    double                       next_due_ = 0.0;
    float                        overlap_    = 0.0f;
    float                        blend_edge_ = 0.0f;

    // Camera mode. libcamera allows one CameraManager per process, so views
    // share it (see acquire_camera_manager()).
    std::shared_ptr<libcamera::CameraManager>           camera_manager_;
    std::shared_ptr<libcamera::Camera>                  camera_;
    std::unique_ptr<libcamera::CameraConfiguration>     camera_config_;
    std::unique_ptr<libcamera::FrameBufferAllocator>    buffer_allocator_;
    std::vector<std::unique_ptr<libcamera::Request>>    requests_;
    libcamera::Stream                                  *stream_ = nullptr;
    bool                                                camera_acquired_ = false;
    bool                                                camera_started_  = false;
    std::mutex                                          session_mutex_;
    std::unique_ptr<CaptureSession>                     session_;

    // File mode.
    std::unique_ptr<FileSource> file_src_;
    DmaBufFrame                 file_frame_;

    // Surround mode.
    std::vector<Slot>        slots_;
    std::vector<DmaBufFrame> frames_;
    bool                     pyramid_ = false;
    bool                     live_    = false;

    std::vector<CaptureSession *>                 live_sessions_;
    std::vector<const libcamera::FrameBuffer *>   live_bufs_;
    std::vector<libcamera::Request *>             live_reqs_;

    // Pattern mode. The motion is a function of elapsed time, not of the frame
    // counter: tying it to frames makes every hitch and every change of frame
    // rate a change of speed, which is what judder is.
    std::vector<uint8_t> pattern_;
    uint64_t             pattern_frame_ = 0;
    double               pattern_start_ms_ = 0.0;
};
