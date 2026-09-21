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
    for (CaptureSession *s : live_sessions_) s->stop();
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
    stop_live_cameras();
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
    release_live_slots();
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

    // A frame-duration limit, when one was asked for. Both ends of the range are
    // the same value: libcamera reads it as "every frame lasts this long", which
    // caps the exposure AE may choose and so pins the rate. Left empty, the IPA
    // is free to stretch the frame for light, and the rate follows the room.
    ControlList controls(camera_->controls());
    if (params_.camera_fps > 0.0) {
        const int64_t us = (int64_t)(1'000'000.0 / params_.camera_fps + 0.5);
        controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({us, us}));
        std::printf("[bev/camera] frame duration pinned to %ld us (%.3g fps)\n",
                    (long)us, params_.camera_fps);
    }
    if (camera_->start(&controls)) return false;
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

    bool any_live = false, all_live = true;
    for (const std::string &s : params_.sources) {
        const bool is_live = s.rfind("camera:", 0) == 0;
        any_live = any_live || is_live;
        all_live = all_live && is_live;
    }
    if (any_live && !all_live) {
        std::fprintf(stderr, "[bev/surround] camera sources cannot be mixed with files\n");
        return false;
    }
    if (any_live && pyramid_) {
        std::fprintf(stderr, "[bev/surround] blend=pyramid has no live camera path\n");
        return false;
    }
    live_ = any_live;
    if (live_) {
        camera_manager_ = acquire_camera_manager(params_.tuning_file);
        if (!camera_manager_) return false;
    }

    slots_.resize((size_t)n);
    for (int i = 0; i < n; ++i) {
        Slot &slot    = slots_[(size_t)i];
        slot.path     = params_.sources[(size_t)i];
        slot.cfg_slot = params_.cfg_slots[(size_t)i];
        if (live_) {
            if (!open_live_slot(slot, std::atoi(slot.path.c_str() + 7))) return false;
            continue;
        }
        slot.src      = std::make_unique<FileSource>();
        if (!slot.src->open(slot.path)) return false;
        slot.frame = slot.src->nextFrame();
        if (!slot.frame.data) {
            std::fprintf(stderr, "[bev/surround] no first frame from %s\n", slot.path.c_str());
            return false;
        }
        slot.width  = slot.frame.width;
        slot.height = slot.frame.height;
        slot.stride = slot.frame.stride;
    }
    if (!live_) frames_.resize((size_t)n);

    const int canvas_w = slots_[0].width, canvas_h = slots_[0].height;
    if (live_) {
        for (const Slot &slot : slots_) {
            if (slot.width != canvas_w || slot.height != canvas_h) {
                std::fprintf(stderr, "[bev/surround] all cameras must use the same size\n");
                return false;
            }
        }
    }
    bool ok;
    if (live_)        ok = renderer_->init_multi_live(*egl_, canvas_w, canvas_h, slots_[0].stride, n);
    else if (pyramid_) ok = renderer_->init_multi_pyramid(*egl_, canvas_w, canvas_h, slots_[0].stride, n);
    else              ok = renderer_->init_multi(*egl_, canvas_w, canvas_h, slots_[0].stride, n);
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

        // Lens distortion, normalized to this slot's frame size.
        if (!sc.lens_model.empty() &&
            !lens_calib_load(lens_calib_path(lens_dir, sc.lens_model), sc))
            std::printf("[bev/surround] no lens calibration for '%s' -- slot %d continuing "
                        "without distortion correction\n", sc.lens_model.c_str(), slot.cfg_slot + 1);
        float fxn = 0, fyn = 0, cxn = 0, cyn = 0;
        if (sc.has_distortion) {
            const float sx = (float)slot.width / (float)sc.lens_calib_w;
            const float sy = (float)slot.height / (float)sc.lens_calib_h;
            fxn = (float)sc.fx * sx / (float)slot.width;
            fyn = (float)sc.fy * sy / (float)slot.height;
            cxn = (float)sc.cx * sx / (float)slot.width;
            cyn = (float)sc.cy * sy / (float)slot.height;
        }
        if (pyramid_)
            renderer_->set_distortion_multi_pyramid(i, sc.has_distortion, fxn, fyn, cxn, cyn,
                (float)sc.k1, (float)sc.k2, (float)sc.k3, (float)sc.p1, (float)sc.p2);
        else
            renderer_->set_distortion_multi(i, sc.has_distortion, fxn, fyn, cxn, cyn,
                (float)sc.k1, (float)sc.k2, (float)sc.k3, (float)sc.p1, (float)sc.p2);

        // The saved deltas are exactly the live pose's offset from the
        // calibration pose that main.cpp's rebuild_slot() computes.
        mat3 H = measured_H(sc.hb2i, slot.width, slot.height, px_per_m, canvas_w, canvas_h,
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
    if (live_ && !start_live_cameras()) return false;
    return true;
}

bool BevPipeline::render_surround()
{
    if (live_) return render_surround_live();
    for (size_t i = 0; i < slots_.size(); ++i) frames_[i] = slots_[i].frame;
    if (pyramid_) renderer_->render_frame_multi_pyramid(frames_);
    else          renderer_->render_frame_multi(frames_);
    pace(slots_[0].src->frame_duration_ms());
    for (auto &slot : slots_)
        if (!advance(*slot.src, slot.path, slot.frame)) return false;
    return true;
}

bool BevPipeline::open_live_slot(Slot &slot, int index)
{
    const auto cameras = camera_manager_->cameras();
    if (index < 0 || (size_t)index >= cameras.size()) {
        std::fprintf(stderr, "[bev/surround] camera %d requested, but %zu camera(s) detected\n",
                     index, cameras.size());
        return false;
    }
    slot.camera = cameras[(size_t)index];
    std::printf("[bev/surround] camera %d: %s\n", index, slot.camera->id().c_str());
    if (slot.camera->acquire()) {
        std::fprintf(stderr, "[bev/surround] %s is in use\n", slot.camera->id().c_str());
        return false;
    }
    slot.acquired = true;

    slot.config = slot.camera->generateConfiguration({StreamRole::Viewfinder});
    if (!slot.config) return false;
    StreamConfiguration &sc = slot.config->at(0);
    sc.size        = {(unsigned)(params_.width  > 0 ? params_.width  : 1640),
                      (unsigned)(params_.height > 0 ? params_.height : 1232)};
    sc.pixelFormat = formats::NV12;

    const auto status = slot.config->validate();
    if (status == CameraConfiguration::Invalid) {
        std::fprintf(stderr, "[bev/surround] camera %d: no supported format\n", index);
        return false;
    }
    if (status == CameraConfiguration::Adjusted)
        std::printf("[bev/surround] camera %d: config adjusted: %s\n", index, sc.toString().c_str());
    if (slot.camera->configure(slot.config.get())) {
        std::fprintf(stderr, "[bev/surround] camera %d: configure failed\n", index);
        return false;
    }

    slot.stream    = sc.stream();
    slot.allocator = std::make_unique<FrameBufferAllocator>(slot.camera);
    if (slot.allocator->allocate(slot.stream) < 0) return false;
    for (auto &buf : slot.allocator->buffers(slot.stream)) {
        auto req = slot.camera->createRequest();
        if (!req || req->addBuffer(slot.stream, buf.get())) return false;
        slot.requests.push_back(std::move(req));
    }

    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        slot.session = std::make_unique<CaptureSession>(sc, slot.camera.get());
        live_sessions_.push_back(slot.session.get());
    }
    slot.camera->requestCompleted.connect(slot.session.get(), &CaptureSession::requestCompleted);

    slot.width  = (int)sc.size.width;
    slot.height = (int)sc.size.height;
    slot.stride = (int)sc.stride;
    return true;
}

bool BevPipeline::start_live_cameras()
{
    for (Slot &slot : slots_) {
        ControlList controls(slot.camera->controls());
        if (params_.camera_fps > 0.0) {
            const int64_t us = (int64_t)(1'000'000.0 / params_.camera_fps + 0.5);
            controls.set(controls::FrameDurationLimits, Span<const int64_t, 2>({us, us}));
        }
        if (slot.camera->start(&controls)) {
            std::fprintf(stderr, "[bev/surround] camera start failed\n");
            return false;
        }
        slot.started = true;
        for (auto &req : slot.requests) slot.camera->queueRequest(req.get());
    }
    return true;
}

bool BevPipeline::render_surround_live()
{
    live_bufs_.clear();
    live_reqs_.clear();
    for (Slot &slot : slots_) {
        auto [buf, req] = slot.session->nextFrame();
        if (!buf) return false;
        live_bufs_.push_back(buf);
        live_reqs_.push_back(req);
    }
    renderer_->render_frame_multi_live(live_bufs_);
    for (size_t i = 0; i < slots_.size(); ++i) {
        live_reqs_[i]->reuse(Request::ReuseBuffers);
        slots_[i].camera->queueRequest(live_reqs_[i]);
    }
    return true;
}

void BevPipeline::stop_live_cameras()
{
    for (Slot &slot : slots_) {
        if (slot.started) {
            slot.camera->stop();
            slot.started = false;
        }
        if (slot.camera && slot.session)
            slot.camera->requestCompleted.disconnect(slot.session.get());
    }
}

void BevPipeline::release_live_slots()
{
    {
        std::lock_guard<std::mutex> lock(session_mutex_);
        live_sessions_.clear();
    }
    for (Slot &slot : slots_) {
        slot.requests.clear();
        if (slot.allocator && slot.stream) slot.allocator->free(slot.stream);
        slot.allocator.reset();
        slot.stream = nullptr;
        slot.session.reset();
        if (slot.acquired) {
            slot.camera->release();
            slot.acquired = false;
        }
        slot.camera.reset();
        slot.config.reset();
    }
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
