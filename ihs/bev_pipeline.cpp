#include "bev_pipeline.h"

#include "bev_setup.h"
#include "ipm.h"
#include "perf_timer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <thread>

using namespace libcamera;

namespace {

// One CameraManager for the process, alive while any view holds it.
std::shared_ptr<CameraManager> acquire_camera_manager(const std::string &tuning_file)
{
    static std::mutex                 mutex;
    static std::weak_ptr<CameraManager> shared;

    std::lock_guard<std::mutex> lock(mutex);
    if (auto cm = shared.lock()) {
        if (!tuning_file.empty())
            std::fprintf(stderr, "[bev/camera] tuning_file ignored: the camera manager is "
                                 "already running for another view\n");
        return cm;
    }
    // Read once, by the RPi pipeline handler, when the manager starts -- same
    // mechanism as the CLI's --tuning-file.
    if (!tuning_file.empty()) setenv("LIBCAMERA_RPI_TUNING_FILE", tuning_file.c_str(), 1);

    auto cm = std::make_unique<CameraManager>();
    if (cm->start()) {
        std::fprintf(stderr, "[bev/camera] CameraManager::start failed\n");
        return nullptr;
    }
    std::shared_ptr<CameraManager> started(cm.release(), [](CameraManager *c) {
        c->stop();
        delete c;
    });
    shared = started;
    return started;
}

} // namespace

bool BevPipeline::init(const EGLState &egl, const BevParams &params)
{
    egl_      = &egl;
    params_   = params;
    renderer_ = std::make_unique<GpuRenderer>();
    next_due_ = now_ms();

    switch (params_.mode) {
    case BevParams::Mode::Camera:   return init_camera();
    case BevParams::Mode::File:     return init_file();
    case BevParams::Mode::Surround: return init_surround();
    case BevParams::Mode::Pattern:  return init_pattern();
    }
    return false;
}

bool BevPipeline::render_next()
{
    if (!renderer_) return false;
    switch (params_.mode) {
    case BevParams::Mode::Camera:   return render_camera();
    case BevParams::Mode::File:     return render_file();
    case BevParams::Mode::Surround: return render_surround();
    case BevParams::Mode::Pattern:  return render_pattern();
    }
    return false;
}

void BevPipeline::interrupt()
{
    std::lock_guard<std::mutex> lock(session_mutex_);
    if (session_) session_->stop();
}

void BevPipeline::shutdown()
{
    // Same order main.cpp's camera path tears down in.
    interrupt();
    if (camera_started_) {
        camera_->stop();
        camera_started_ = false;
    }
    if (camera_ && session_) camera_->requestCompleted.disconnect(session_.get());
    renderer_.reset();
    requests_.clear();
    if (buffer_allocator_ && stream_) buffer_allocator_->free(stream_);
    buffer_allocator_.reset();
    stream_ = nullptr;
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_.reset();
    }
    if (camera_acquired_) {
        camera_->release();
        camera_acquired_ = false;
    }
    camera_.reset();
    camera_config_.reset();
    camera_manager_.reset();

    file_src_.reset();
    slots_.clear();
    frames_.clear();
    pattern_.clear();
}

// Same clamps as main.cpp's +/- and [/] keys.
void BevPipeline::set_overlap(float value)
{
    if (!renderer_) return;
    overlap_ = std::clamp(value, 1.0f, 360.0f);
    renderer_->set_stitch_overlap(overlap_);
}

void BevPipeline::set_blend_edge(float value)
{
    if (!renderer_) return;
    blend_edge_ = std::clamp(value, 0.0f, 0.49f);
    renderer_->set_blend_edge(blend_edge_);
}

void BevPipeline::pace(double frame_duration_ms)
{
    if (frame_duration_ms <= 0.0) return;
    next_due_ += frame_duration_ms;
    const double now = now_ms();
    if (next_due_ > now)
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(next_due_ - now));
    else
        next_due_ = now; // fell behind: re-anchor rather than sprint to catch up
}

// Next frame, looping the recording on EOS -- a view keeps playing rather than
// freezing on its last frame.
bool BevPipeline::advance(FileSource &src, const std::string &path, DmaBufFrame &frame)
{
    frame = src.nextFrame();
    if (frame.data) return true;
    std::printf("[bev/file] EOS -- looping %s\n", path.c_str());
    src.close();
    if (src.open(path)) frame = src.nextFrame();
    return frame.data != nullptr;
}

// ── Camera ───────────────────────────────────────────────────────────────────

bool BevPipeline::init_camera()
{
    camera_manager_ = acquire_camera_manager(params_.tuning_file);
    if (!camera_manager_) return false;

    const auto cameras = camera_manager_->cameras();
    if ((size_t)params_.camera_index >= cameras.size()) {
        std::fprintf(stderr, "[bev/camera] camera_index %d, but %zu camera(s) detected\n",
                     params_.camera_index, cameras.size());
        return false;
    }
    camera_ = cameras[(size_t)params_.camera_index];
    std::printf("[bev/camera] camera: %s\n", camera_->id().c_str());
    if (camera_->acquire()) {
        std::fprintf(stderr, "[bev/camera] %s is in use\n", camera_->id().c_str());
        return false;
    }
    camera_acquired_ = true;

    camera_config_ = camera_->generateConfiguration({StreamRole::Viewfinder});
    if (!camera_config_) return false;
    StreamConfiguration &sc = camera_config_->at(0);
    sc.size        = {(unsigned)(params_.width  > 0 ? params_.width  : 1640),
                      (unsigned)(params_.height > 0 ? params_.height : 1232)};
    sc.pixelFormat = formats::NV12;

    const auto status = camera_config_->validate();
    if (status == CameraConfiguration::Invalid) {
        std::fprintf(stderr, "[bev/camera] no supported format\n");
        return false;
    }
    if (status == CameraConfiguration::Adjusted)
        std::printf("[bev/camera] config adjusted: %s\n", sc.toString().c_str());
    if (camera_->configure(camera_config_.get())) {
        std::fprintf(stderr, "[bev/camera] configure failed\n");
        return false;
    }

    stream_ = sc.stream();
    buffer_allocator_ = std::make_unique<FrameBufferAllocator>(camera_);
    if (buffer_allocator_->allocate(stream_) < 0) return false;
    for (auto &buf : buffer_allocator_->buffers(stream_)) {
        auto req = camera_->createRequest();
        if (!req || req->addBuffer(stream_, buf.get())) return false;
        requests_.push_back(std::move(req));
    }

    if (!renderer_->init(*egl_, sc, buffer_allocator_->buffers(stream_))) return false;

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        session_ = std::make_unique<CaptureSession>(sc, camera_.get());
    }
    camera_->requestCompleted.connect(session_.get(), &CaptureSession::requestCompleted);
    if (camera_->start()) return false;
    camera_started_ = true;
    for (auto &req : requests_) camera_->queueRequest(req.get());

    // No waitWarmupDone(): it can't be interrupted, and nextFrame() already
    // yields nothing until AE/AWB has settled.
    std::printf("[bev/camera] streaming %s, warming up AE/AWB (%d frames)\n",
                sc.toString().c_str(), CaptureSession::WARMUP_FRAMES);
    return true;
}

bool BevPipeline::render_camera()
{
    auto [buf, req] = session_->nextFrame();
    if (!buf) return false;
    renderer_->render_frame(buf);
    req->reuse(Request::ReuseBuffers);
    camera_->queueRequest(req);
    return true;
}

// ── Single file ──────────────────────────────────────────────────────────────

bool BevPipeline::init_file()
{
    file_src_ = std::make_unique<FileSource>();
    if (!file_src_->open(params_.file)) return false;
    file_frame_ = file_src_->nextFrame();
    if (!file_frame_.data) {
        std::fprintf(stderr, "[bev/file] no first frame from %s\n", params_.file.c_str());
        return false;
    }
    const DmaBufFrame &f = file_frame_;
    if (!renderer_->init(*egl_, f.width, f.height, f.stride)) return false;

    if (params_.bev) {
        if (!renderer_->init_bev()) return false;
        renderer_->set_bev_enabled(true);
        mat3 K = build_intrinsics(f.width, f.height);
        mat3 H = ground_to_image_H(0.0, params_.cam_y, params_.cam_h, params_.pitch, params_.yaw,
                                   K, f.width, f.height, params_.px_per_m, f.width, f.height);
        float Hf[9];
        H.to_floats(Hf);
        renderer_->set_bev(Hf);
    }
    return true;
}

bool BevPipeline::render_file()
{
    if (!file_frame_.data) return false;
    renderer_->render_frame(file_frame_);
    pace(file_src_->frame_duration_ms());
    return advance(*file_src_, params_.file, file_frame_);
}

// ── N-camera surround view ───────────────────────────────────────────────────
// run_multi_file_mode()'s setup, minus the keyboard.

bool BevPipeline::init_surround()
{
    const int n = (int)params_.sources.size();
    pyramid_ = params_.blend == "pyramid";
    const int max_cams = pyramid_ ? GpuRenderer::kPyramidMaxCameras : GpuRenderer::kMaxCameras;
    if (n < 1 || n > max_cams) {
        std::fprintf(stderr, "[bev/surround] %d source(s), must be 1..%d\n", n, max_cams);
        return false;
    }

    BevConfig cfg;
    if (!bev_config_load(params_.config_path, cfg)) return false;
    for (int i = 0; i < n; ++i) {
        const int s = params_.cfg_slots[(size_t)i];
        if (s < 0 || s >= BevConfig::kMaxSlots || !cfg.slots[s].has_hb2i) {
            std::fprintf(stderr, "[bev/surround] source %d (%s) maps to config slot %d, which has "
                                 "no hb2i in %s -- calibrate it with calibrate_bev.py first\n",
                         i, params_.sources[(size_t)i].c_str(), s, params_.config_path.c_str());
            return false;
        }
    }

    slots_.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        Slot &slot    = slots_[(size_t)i];
        slot.path     = params_.sources[(size_t)i];
        slot.cfg_slot = params_.cfg_slots[(size_t)i];
        slot.src      = std::make_unique<FileSource>();
        if (!slot.src->open(slot.path)) return false;
        slot.frame = slot.src->nextFrame();
        if (!slot.frame.data) {
            std::fprintf(stderr, "[bev/surround] no first frame from %s\n", slot.path.c_str());
            return false;
        }
    }
    frames_.resize((size_t)n);

    // Canvas is slot 0's first-frame size, fixed for the run.
    const int canvas_w = slots_[0].frame.width, canvas_h = slots_[0].frame.height;
    const bool ok = pyramid_
        ? renderer_->init_multi_pyramid(*egl_, canvas_w, canvas_h, slots_[0].frame.stride, n)
        : renderer_->init_multi(*egl_, canvas_w, canvas_h, slots_[0].frame.stride, n);
    if (!ok) return false;

    if (!params_.car_icon.empty()) {
        const double w = params_.car_width_set  ? params_.car_width  : cfg.car_width;
        const double l = params_.car_length_set ? params_.car_length : cfg.car_length;
        const double x = params_.car_x_set      ? params_.car_x      : cfg.car_x;
        const double y = params_.car_y_set      ? params_.car_y      : cfg.car_y;
        if (!renderer_->init_car_icon(params_.car_icon.c_str(), (float)w, (float)l, (float)x, (float)y))
            std::fprintf(stderr, "[bev/surround] car icon '%s' failed to load -- continuing "
                                 "without it\n", params_.car_icon.c_str());
    }

    const double px_per_m = params_.px_per_m_set ? params_.px_per_m : cfg.px_per_m;
    std::string lens_dir = params_.lens_dir;
    if (lens_dir.empty()) {
        const size_t slash = params_.config_path.find_last_of('/');
        lens_dir = slash == std::string::npos ? "." : params_.config_path.substr(0, slash);
    }

    for (int i = 0; i < n; ++i) {
        const Slot &slot = slots_[(size_t)i];
        BevSlotConfig &sc = cfg.slots[slot.cfg_slot];
        const DmaBufFrame &f = slot.frame;

        // Lens distortion, normalized to this slot's frame size.
        if (!sc.lens_model.empty() &&
            !lens_calib_load(lens_calib_path(lens_dir, sc.lens_model), sc))
            std::printf("[bev/surround] no lens calibration for '%s' -- slot %d continuing "
                        "without distortion correction\n", sc.lens_model.c_str(), slot.cfg_slot + 1);
        float fxn = 0, fyn = 0, cxn = 0, cyn = 0;
        if (sc.has_distortion) {
            const float sx = (float)f.width / (float)sc.lens_calib_w;
            const float sy = (float)f.height / (float)sc.lens_calib_h;
            fxn = (float)sc.fx * sx / (float)f.width;
            fyn = (float)sc.fy * sy / (float)f.height;
            cxn = (float)sc.cx * sx / (float)f.width;
            cyn = (float)sc.cy * sy / (float)f.height;
        }
        if (pyramid_)
            renderer_->set_distortion_multi_pyramid(i, sc.has_distortion, fxn, fyn, cxn, cyn,
                (float)sc.k1, (float)sc.k2, (float)sc.k3, (float)sc.p1, (float)sc.p2);
        else
            renderer_->set_distortion_multi(i, sc.has_distortion, fxn, fyn, cxn, cyn,
                (float)sc.k1, (float)sc.k2, (float)sc.k3, (float)sc.p1, (float)sc.p2);

        // The saved deltas are exactly the live pose's offset from the
        // calibration pose that main.cpp's rebuild_slot() computes.
        mat3 H = measured_H(sc.hb2i, f.width, f.height, px_per_m, canvas_w, canvas_h,
                            sc.cam_x_delta, sc.cam_y_delta, sc.yaw_delta);
        float Hf[9];
        H.to_floats(Hf);
        if (pyramid_) renderer_->set_ipm_multi_pyramid(i, Hf, (float)sc.facing_deg);
        else          renderer_->set_ipm_multi(i, Hf, (float)sc.facing_deg);
        std::printf("[bev/surround] source %d -> config slot %d (%s)\n",
                    i, slot.cfg_slot + 1, slot.path.c_str());
    }

    set_overlap(cfg.overlap_deg);
    set_blend_edge(cfg.blend_edge);
    renderer_->set_coverage_weight(params_.blend == "coverage");
    renderer_->set_px_per_m((float)px_per_m);
    std::printf("[bev/surround] %d source(s), blend=%s, canvas %dx%d\n",
                n, params_.blend.c_str(), canvas_w, canvas_h);
    return true;
}

bool BevPipeline::render_surround()
{
    for (size_t i = 0; i < slots_.size(); ++i) frames_[i] = slots_[i].frame;
    if (pyramid_) renderer_->render_frame_multi_pyramid(frames_);
    else          renderer_->render_frame_multi(frames_);
    pace(slots_[0].src->frame_duration_ms());
    for (auto &slot : slots_)
        if (!advance(*slot.src, slot.path, slot.frame)) return false;
    return true;
}

// ── Test pattern ─────────────────────────────────────────────────────────────
// Scrolling BT.601 colour bars, CPU-generated NV12 through the same sysmem
// upload path file mode uses -- enough to prove a shell composites the view
// with no camera and no recording.

bool BevPipeline::init_pattern()
{
    const int w = (params_.width  > 0 ? params_.width  : 1280) & ~1;
    const int h = (params_.height > 0 ? params_.height : 720)  & ~1;
    pattern_.assign((size_t)w * (size_t)h * 3 / 2, 0);
    pattern_start_ms_ = now_ms();
    return renderer_->init(*egl_, w, h, w);
}

bool BevPipeline::render_pattern()
{
    static constexpr uint8_t kBars[7][3] = { // Y, U, V
        {180, 128, 128}, {162,  44, 142}, {131, 156,  44}, {112,  72,  58},
        { 84, 184, 198}, { 65, 100, 212}, { 35, 212, 114},
    };
    const int w = renderer_->width(), h = renderer_->height();

    // Both speeds are per second, and match what the old per-frame constants
    // came to at 30fps (4 px and 3 px a frame). Per frame, a hitch or a change
    // of rate became a change of speed -- the motion sped up when the frame
    // rate did -- which reads as judder however cleanly each frame is drawn.
    static constexpr double kBarsPxPerSecond = 120.0;
    static constexpr double kBandPxPerSecond = 90.0;
    const double elapsed = (now_ms() - pattern_start_ms_) / 1000.0;
    const int shift = (int)std::fmod(elapsed * kBarsPxPerSecond, (double)w);

    // One row of each plane, then copied down. Writing the UV plane a column at
    // a time instead touches a fresh cache line per pixel, which on a Pi 4 cost
    // more than the GPU work it was meant to feed and made the frame rate
    // depend on where the band happened to be.
    uint8_t *y0 = pattern_.data();
    for (int x = 0; x < w; ++x) y0[x] = kBars[((x + shift) % w) * 7 / w][0];

    uint8_t *uv0 = pattern_.data() + (size_t)w * (size_t)h;
    for (int x = 0; x < w; x += 2) {
        const int bar = ((x + shift) % w) * 7 / w;
        uv0[x]     = kBars[bar][1];
        uv0[x + 1] = kBars[bar][2];
    }
    for (int row = 1; row < h / 2; ++row)
        std::copy(uv0, uv0 + w, uv0 + (size_t)row * (size_t)w);

    for (int row = 1; row < h; ++row)
        std::copy(y0, y0 + w, y0 + (size_t)row * (size_t)w);

    // A dark band sweeping down marks top from bottom.
    const int band = (int)std::fmod(elapsed * kBandPxPerSecond, (double)h);
    for (int row = band; row < std::min(h, band + h / 16); ++row) {
        uint8_t *dst = y0 + (size_t)row * (size_t)w;
        for (int x = 0; x < w; ++x) dst[x] = (uint8_t)(dst[x] / 3);
    }

    DmaBufFrame frame;
    frame.data      = pattern_.data();
    frame.width     = w;
    frame.height    = h;
    frame.stride    = w;
    frame.y_offset  = 0;
    frame.uv_offset = w * h;
    renderer_->render_frame(frame);
    ++pattern_frame_;
    // 0 leaves it uncapped: pace() ignores a non-positive duration.
    pace(params_.pattern_fps > 0.0 ? 1000.0 / params_.pattern_fps : 0.0);
    return true;
}
