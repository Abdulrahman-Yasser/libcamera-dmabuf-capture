#include "pipeline_api.h"
#include "pipeline_runtime.h"

#include <GLES3/gl3.h>
#ifdef PIPELINE_HAVE_IHS
#include <ihs/platform_view.h>
#endif
#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <cstdlib>
#include <future>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

struct PendingPose {
    int    slot;
    double dx, dy, dyaw;
};

struct PipelineHandle {
    std::string       config_path = "bev_config.ini";
    std::string       tuning_file;

    std::thread       worker;
    std::atomic<bool> running{false};
    bool              started = false;

    PipelineState     state;
    EGLState          egl;

#ifdef PIPELINE_HAVE_IHS
    std::mutex        ihs_mtx;
    IhsPlatformView  *ihs_view      = nullptr;
    bool              ihs_suspended = false;
    bool              ring_failed   = false;
    std::vector<int>  ring_release;
    void submit_to_view();
#endif

    std::mutex               mtx;
    std::vector<PendingPose> pending;
    std::string              snapshot_path;
    bool                     exported = false;
    PipelineFrame            frame{};
    std::atomic<uint64_t>    frame_id{0};

    void run(std::promise<bool> &ready);
    void abort_setup();
};

void PipelineHandle::abort_setup()
{
    for (auto &slot : state.slots) {
        if (slot.session) slot.session->stop();
        if (slot.camera) { slot.camera->stop(); }
    }
    state.renderer.cleanup();
    for (auto &slot : state.slots) {
        if (slot.alloc && slot.stream) slot.alloc->free(slot.stream);
        if (slot.camera) slot.camera->release();
    }
    if (state.cm) state.cm->stop();
    teardown_egl(egl);
}

void PipelineHandle::run(std::promise<bool> &ready)
{
    if (!setup_egl(egl) || !check_extensions(egl)) {
        teardown_egl(egl);
        ready.set_value(false);
        return;
    }
    if (!pipeline_setup(state, egl, config_path)) {
        std::cerr << "[api] pipeline_setup failed\n";
        abort_setup();
        ready.set_value(false);
        return;
    }
    ready.set_value(true);

    auto before = [this] {
        std::vector<PendingPose> todo;
        std::string snap;
        {
            std::lock_guard<std::mutex> lk(mtx);
            todo.swap(pending);
            snap.swap(snapshot_path);
        }
        if (!snap.empty()) state.renderer.save_snapshot(snap.c_str());
        for (auto &p : todo)
            pipeline_set_slot_pose(state, p.slot, p.dx, p.dy, p.dyaw);
    };
    auto after = [this] {
        glFinish();
        if (!exported) {
            int fd, stride, fourcc;
            uint64_t mod;
            if (!state.renderer.export_fbo_dmabuf(fd, stride, fourcc, mod)) {
                running.store(false);
                return;
            }
            std::lock_guard<std::mutex> lk(mtx);
            frame.dmabuf_fd = fd;
            frame.width     = state.renderer.width();
            frame.height    = state.renderer.height();
            frame.stride    = stride;
            frame.fourcc    = fourcc;
            frame.modifier  = mod;
            exported = true;
        }
        frame_id.fetch_add(1);
#ifdef PIPELINE_HAVE_IHS
        submit_to_view();
#endif
    };

    pipeline_loop(state, running, before, after);
#ifdef PIPELINE_HAVE_IHS
    for (int rfd : ring_release)
        if (rfd >= 0) close(rfd);
    ring_release.clear();
#endif
    pipeline_teardown(state, egl);
}

#ifdef PIPELINE_HAVE_IHS
static const uint32_t kFourccAB24 = 0x34324241;
static const int      kRingSize   = 4;

static int negotiate_view(IhsPlatformView *view)
{
    IhsFormatModifier fmt{};
    fmt.fourcc   = kFourccAB24;
    fmt.modifier = 0;

    IhsPvRequirements req{};
    req.struct_size  = sizeof req;
    req.kinds        = IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
    req.formats      = &fmt;
    req.format_count = 1;
    req.sync         = IHS_PV_SYNC_IMPLICIT;
    req.z_order      = IHS_PV_Z_INLINE;

    IhsPvGrant grant{};
    grant.struct_size = sizeof grant;
    int rc = ihs_pv_negotiate(view, &req, &grant);
    if (rc != IHS_PV_OK) return rc;
    if (grant.granted_kind != IHS_PV_KIND_TEXTURE_DMABUF_IMPORT) return IHS_PV_ERR_UNSUPPORTED;
    return IHS_PV_OK;
}

static void view_set_suspended(void *user, uint8_t suspended)
{
    auto *h = static_cast<PipelineHandle *>(user);
    std::lock_guard<std::mutex> lk(h->ihs_mtx);
    h->ihs_suspended = suspended != 0;
}

static void view_renegotiate(void *user)
{
    auto *h = static_cast<PipelineHandle *>(user);
    std::lock_guard<std::mutex> lk(h->ihs_mtx);
    if (h->ihs_view) negotiate_view(h->ihs_view);
}

static void view_dispose(void *user)
{
    auto *h = static_cast<PipelineHandle *>(user);
    std::lock_guard<std::mutex> lk(h->ihs_mtx);
    h->ihs_view = nullptr;
}

static int view_factory(const IhsPvCreateInfo *, void *factory_user, IhsPlatformView *view,
                        IhsPvCallbacks *cb, void **out_user)
{
    auto *h = static_cast<PipelineHandle *>(factory_user);
    int rc = negotiate_view(view);
    if (rc != IHS_PV_OK) return rc;

    cb->struct_size    = sizeof *cb;
    cb->set_suspended  = view_set_suspended;
    cb->renegotiate    = view_renegotiate;
    cb->dispose        = view_dispose;

    std::lock_guard<std::mutex> lk(h->ihs_mtx);
    h->ihs_view      = view;
    h->ihs_suspended = false;
    *out_user        = h;
    return IHS_PV_OK;
}

void PipelineHandle::submit_to_view()
{
    std::lock_guard<std::mutex> lk(ihs_mtx);
    if (!ihs_view || ihs_suspended || ring_failed) return;

    if (!state.renderer.has_export_ring()) {
        if (!state.renderer.init_export_ring(kRingSize)) {
            ring_failed = true;
            return;
        }
        ring_release.assign(kRingSize, -1);
    }

    int slot = state.renderer.next_ring_slot();
    if (ring_release[slot] >= 0) {
        pollfd p{ring_release[slot], POLLIN, 0};
        poll(&p, 1, 100);
        close(ring_release[slot]);
        ring_release[slot] = -1;
    }

    int id, fd, stride, fourcc;
    if (!state.renderer.export_ring_frame(id, fd, stride, fourcc)) return;

    IhsFrame f{};
    f.struct_size      = sizeof f;
    f.format.fourcc    = (uint32_t)fourcc;
    f.format.modifier  = 0;
    f.width            = (uint32_t)state.renderer.width();
    f.height           = (uint32_t)state.renderer.height();
    f.plane_count      = 1;
    f.plane_fd[0]      = fd;
    f.plane_offset[0]  = 0;
    f.plane_stride[0]  = (uint32_t)stride;
    f.buffer_id        = (uint32_t)id;

    int release_fd = -1;
    ihs_pv_submit(ihs_view, &f, -1, &release_fd);
    ring_release[id] = release_fd;
}
#endif

extern "C" {

PipelineHandle *pipeline_create(const char *bev_config_path, const char *tuning_file)
{
    auto *h = new PipelineHandle;
    if (bev_config_path && *bev_config_path) h->config_path = bev_config_path;
    if (tuning_file && *tuning_file)         h->tuning_file = tuning_file;
    return h;
}

int pipeline_start(PipelineHandle *h)
{
    if (!h || h->started) return -1;

    if (!h->tuning_file.empty())
        setenv("LIBCAMERA_RPI_TUNING_FILE", h->tuning_file.c_str(), 1);

    h->running.store(true);
    std::promise<bool> ready;
    auto fut = ready.get_future();
    h->worker = std::thread([h, &ready] { h->run(ready); });

    if (!fut.get()) {
        h->worker.join();
        h->running.store(false);
        return -1;
    }
    h->started = true;
    return 0;
}

int pipeline_get_frame(PipelineHandle *h, PipelineFrame *out)
{
    if (!h || !out || !h->started || h->frame_id.load() == 0) return -1;
    std::lock_guard<std::mutex> lk(h->mtx);
    if (!h->exported) return -1;
    *out = h->frame;
    out->dmabuf_fd = dup(h->frame.dmabuf_fd);
    out->frame_id  = h->frame_id.load();
    return out->dmabuf_fd < 0 ? -1 : 0;
}

int pipeline_set_calibration(PipelineHandle *h, int slot, double dx, double dy, double dyaw)
{
    if (!h || !h->started) return -1;
    std::lock_guard<std::mutex> lk(h->mtx);
    h->pending.push_back({slot, dx, dy, dyaw});
    return 0;
}

int pipeline_register_platform_view(PipelineHandle *h, const char *view_type)
{
#ifdef PIPELINE_HAVE_IHS
    if (!h || !view_type) return -1;
    return ihs_pv_register_factory(view_type, view_factory, h) == IHS_PV_OK ? 0 : -1;
#else
    (void)h; (void)view_type;
    return -1;
#endif
}

int pipeline_save_snapshot(PipelineHandle *h, const char *path)
{
    if (!h || !h->started || !path) return -1;
    std::lock_guard<std::mutex> lk(h->mtx);
    h->snapshot_path = path;
    return 0;
}

void pipeline_stop(PipelineHandle *h)
{
    if (!h || !h->started) return;
    h->running.store(false);
    for (auto &slot : h->state.slots)
        if (slot.session) slot.session->stop();
    if (h->worker.joinable()) h->worker.join();
    h->started = false;
}

void pipeline_destroy(PipelineHandle *h)
{
    if (!h) return;
    pipeline_stop(h);
    delete h;
}

}
