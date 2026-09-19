#include "pipeline_api.h"
#include "pipeline_runtime.h"

#include <GLES3/gl3.h>
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
    };

    pipeline_loop(state, running, before, after);
    pipeline_teardown(state, egl);
}

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
