// The ihs_pv face of libbev_view.so: publishes the pipeline as the
// "views/bev-view" platform view against ivi-homescreen's PlatformViewRegistry
// (ihs/platform_view.h).
//
// The view is a pure producer. It renders offscreen on its own thread and EGL
// context, hands the registry a dma-buf per frame, and the shell owns
// compositing, placement and z-order.
//
// One producer serves every backend. Rendering is always GLES, because that is
// what the pipeline's shaders are; what follows the backend is who allocates
// the buffers (dmabuf_ring.h):
//
//   wayland-vulkan / drm-kms-vulkan   exportable images on the shell's Vulkan
//                                     device, imported into GL on the same GPU
//   drm-kms-egl                       scanout-capable bos on the shell's gbm
//                                     device (and DRM_PLANE when scanout=1)
//   wayland-egl                       bos on the view's own render node
//
// Lifecycle: the factory (platform thread) reads the shell's contexts and
// starts the render thread, which brings up EGL and works out which
// format/modifier pairs it can render. The factory then negotiates with those
// -- negotiation is platform-thread only -- and lets the render thread go.

#include "bev_view_api.h"

#include "bev_params.h"
#include "bev_pipeline.h"
#include "dmabuf_ring.h"
#include "egl_context.h"
#ifdef BEV_IHS_PV_VULKAN
#include "vk_dmabuf_allocator.h"
#endif

#include "ihs/platform_view.h"

#include <drm/drm_fourcc.h>
#include <gbm.h>
#include <poll.h>
#include <unistd.h>
#include <xf86drm.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <ctime>
#include <vector>

namespace {

constexpr char kViewType[] = "views/bev-view";

// Parked while the view is off-screen or occluded.
constexpr auto kSuspendedPoll = std::chrono::milliseconds(100);
// Parked after the source stopped, once its black frame is up.
constexpr auto kIdlePoll = std::chrono::milliseconds(100);
// Two vsyncs at 60 Hz. The compositor releases a slot one present after the
// flip that read it, so a release that is merely late lands within this; one
// that never comes now costs a two-frame hitch instead of the six frames 100 ms
// did, which on a Pi 4 read as visible judder at each stall.
constexpr int kFenceWaitMs = 34;

/// What one release-fence wait cost, for the counters in BevViewStats.
struct FenceWait {
    bool     signalled = true; // vacuously true when there was nothing to wait on
    bool     waited    = false;
    uint64_t ns        = 0;
};

// Wait for the compositor to be done with a slot, then close the fence.
// Bounded -- an unbounded poll parks the render thread forever on a release
// that never arrives, and nothing else would ever wake it.
FenceWait wait_and_close_fence(int fd)
{
    if (fd < 0) return {};
    FenceWait out;
    out.waited = true;
    const auto began = std::chrono::steady_clock::now();
    pollfd pfd{fd, POLLIN, 0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kFenceWaitMs);
    for (;;) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        const int rc = poll(&pfd, 1, left > 0 ? (int)left : 0);
        if (rc == 0) {
            out.signalled = false;
            static std::once_flag warned;
            std::call_once(warned, [] {
                std::fprintf(stderr, "[bev/ihs_pv] release fence did not signal within %dms; "
                                     "drawing over the slot anyway. Frames may tear -- see "
                                     "release_timeouts in BevViewStats\n", kFenceWaitMs);
            });
            break;
        }
        if (rc > 0 || errno != EINTR || left <= 0) break;
    }
    out.ns = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - began).count();
    close(fd);
    return out;
}

// The render node the view's EGL display opens: the one the shell's GPU is on,
// so buffers are imported where they were allocated.
std::string pick_render_node(const IhsEglContext *egl, const std::string &vulkan_node)
{
    if (const char *env = std::getenv("BEV_RENDER_NODE"); env && *env) return env;
    if (!vulkan_node.empty()) return vulkan_node;
    if (egl && egl->gbm_device) {
        const int fd = gbm_device_get_fd(static_cast<gbm_device *>(egl->gbm_device));
        if (fd >= 0) {
            // NULL for a display-only device (the Pi's vc4), whose GPU is a
            // separate driver -- the default node is right there.
            if (char *name = drmGetRenderDeviceNameFromFd(fd)) {
                std::string node = name;
                std::free(name);
                return node;
            }
        }
    }
    return "/dev/dri/renderD128";
}

const char *kind_name(uint32_t kind)
{
    switch (kind) {
    case IHS_PV_KIND_TEXTURE_DMABUF_IMPORT: return "texture-dmabuf-import";
    case IHS_PV_KIND_DRM_PLANE:             return "drm-plane";
    case IHS_PV_KIND_SOFTWARE_SHM:          return "software-shm";
    }
    return "none";
}

using Command = std::function<void(BevPipeline &)>;

// One live platform view. The registry owns its lifetime: the factory returns
// it as user_data and dispose deletes it, exactly once.
//
// Threading: IhsPvCallbacks arrive on the platform thread, render_loop() owns
// the render thread and the EGL context. What crosses between them is atomic
// or under a mutex.
class ViewInstance {
public:
    ViewInstance(IhsPlatformView *view, int32_t id, BevParams params, int32_t width, int32_t height)
        : view_(view), id_(id), params_(std::move(params)),
          pending_width_(width), pending_height_(height), width_(width), height_(height) {}

    ~ViewInstance() { stop(); }

    ViewInstance(const ViewInstance &) = delete;
    ViewInstance &operator=(const ViewInstance &) = delete;

    int32_t id() const { return id_; }

    bool start()
    {
        query_shell();
        running_ = true;
        thread_ = std::thread([this] { render_loop(); });

        {
            std::unique_lock<std::mutex> lock(phase_mutex_);
            phase_cv_.wait(lock, [this] { return phase_ != Phase::SettingUp; });
            if (phase_ == Phase::SetupFailed) {
                lock.unlock();
                stop();
                return false;
            }
        }
        const bool ok = negotiate();
        {
            std::lock_guard<std::mutex> lock(phase_mutex_);
            phase_ = ok ? Phase::Go : Phase::Abort;
        }
        phase_cv_.notify_all();
        if (!ok) stop();
        return ok;
    }

    void stop()
    {
        running_ = false;
        {
            std::lock_guard<std::mutex> lock(phase_mutex_);
            if (phase_ == Phase::Ready) phase_ = Phase::Abort;
        }
        phase_cv_.notify_all();
        pipeline_.interrupt();
        if (thread_.joinable()) thread_.join();
    }

    // -- IhsPvCallbacks bodies, platform thread ------------------------------

    // Physical pixels, already DPR-scaled.
    void on_resize(double width, double height)
    {
        pending_width_  = (int32_t)width;
        pending_height_ = (int32_t)height;
    }

    // Not teardown: the view keeps its grant; the render thread just parks.
    void on_suspended(bool suspended) { suspended_ = suspended; }

    // The grant was revoked (mode change, plane pressure, lease withdrawn).
    void on_renegotiate()
    {
        if (negotiate()) {
            regrant_ = true;
            return;
        }
        std::fprintf(stderr, "[bev/ihs_pv] view %d: renegotiation failed; view inert\n", id_);
        running_ = false;
        pipeline_.interrupt();
    }

    // -- bev_view_* controls, any thread -------------------------------------

    void post(Command command)
    {
        std::lock_guard<std::mutex> lock(commands_mutex_);
        commands_.push_back(std::move(command));
    }

    void stats(BevViewStats &out)
    {
        std::lock_guard<std::mutex> lock(stats_mutex_);
        out = stats_;
    }

private:
    enum class Phase { SettingUp, SetupFailed, Ready, Go, Abort };

    // What the shell offers, read before the render thread exists.
    void query_shell()
    {
        IhsPvCapabilities caps{};
        caps.struct_size = sizeof(caps);
        if (ihs_pv_query_capabilities(&caps) == IHS_PV_OK) {
            backend_ = caps.backend_key ? caps.backend_key : "?";
            // Registry-owned and valid only until the next query: copy it.
            if (caps.formats && caps.format_count)
                shell_formats_.assign(caps.formats, caps.formats + caps.format_count);
        }

        IhsVulkanContext vk{};
        vk.struct_size = sizeof(vk);
        const bool has_vulkan = ihs_pv_vulkan_context(&vk) == IHS_PV_OK && vk.device;
        IhsEglContext egl{};
        egl.struct_size = sizeof(egl);
        const bool has_egl = ihs_pv_egl_context(&egl) == IHS_PV_OK && egl.egl_display;

        std::string vulkan_node;
#ifdef BEV_IHS_PV_VULKAN
        if (has_vulkan) {
            auto alloc = std::make_unique<VkDmabufAllocator>();
            if (alloc->init(static_cast<VkInstance>(vk.instance),
                            static_cast<VkPhysicalDevice>(vk.physical_device),
                            static_cast<VkDevice>(vk.device), vk.get_instance_proc_addr,
                            vk.device_extensions, vk.device_extension_count)) {
                vulkan_node = alloc->render_node();
                vk_alloc_ = std::move(alloc);
            } else {
                std::fprintf(stderr, "[bev/ihs_pv] view %d: can't export from the shell's Vulkan "
                                     "device; allocating from the render node\n", id_);
            }
        }
        if (vk_alloc_) allocator_ = RingAllocator::Vulkan;
        else
#else
        if (has_vulkan)
            std::fprintf(stderr, "[bev/ihs_pv] view %d: Vulkan backend, but this build has no "
                                 "Vulkan allocator (ENABLE_IHS_PV_VULKAN=OFF); allocating from "
                                 "the render node\n", id_);
#endif
        if (has_egl && egl.gbm_device) {
            allocator_ = RingAllocator::ShellGbm;
            shell_gbm_ = static_cast<gbm_device *>(egl.gbm_device);
        } else {
            allocator_ = RingAllocator::RenderNode;
        }
        render_node_ = pick_render_node(has_egl ? &egl : nullptr, vulkan_node);
    }

    // Render thread: EGL, the ring, and the formats this view can offer.
    bool setup_gl()
    {
        if (!setup_egl(egl_, render_node_.c_str())) return false;
        if (!check_extensions(egl_)) {
            std::fprintf(stderr, "[bev/ihs_pv] view %d: required EGL/GL extensions missing on %s\n",
                         id_, render_node_.c_str());
            return false;
        }
        if (!ring_.init(egl_, allocator_, shell_gbm_, vk_allocator())) return false;

        // Opaque content, so XRGB first. Per format: the modifiers the
        // allocator can produce that this display can also render into,
        // restricted to what the shell said it can take.
        auto shell_offers = [this](uint32_t fourcc, uint64_t modifier) {
            if (shell_formats_.empty()) return true;
            return std::any_of(shell_formats_.begin(), shell_formats_.end(),
                               [&](const IhsFormatModifier &f) {
                                   return f.fourcc == fourcc && f.modifier == modifier;
                               });
        };
        for (uint32_t fourcc : {(uint32_t)DRM_FORMAT_XRGB8888, (uint32_t)DRM_FORMAT_ARGB8888}) {
            std::vector<uint64_t> modifiers;
#ifdef BEV_IHS_PV_VULKAN
            if (vk_alloc_) {
                const auto renderable = DmabufRing::renderable_modifiers(egl_.dpy, fourcc);
                for (uint64_t m : vk_alloc_->modifiers())
                    if (m == DRM_FORMAT_MOD_LINEAR ||
                        std::find(renderable.begin(), renderable.end(), m) != renderable.end())
                        modifiers.push_back(m);
            } else
#endif
            modifiers.push_back(DRM_FORMAT_MOD_LINEAR);

            for (uint64_t m : modifiers)
                if (shell_offers(fourcc, m)) formats_.push_back({fourcc, 0, m});
        }
        if (formats_.empty()) {
            // Nothing we can make is on the shell's list. Offer LINEAR anyway
            // and let negotiation have the final word.
            formats_.push_back({DRM_FORMAT_XRGB8888, 0, DRM_FORMAT_MOD_LINEAR});
            formats_.push_back({DRM_FORMAT_ARGB8888, 0, DRM_FORMAT_MOD_LINEAR});
        }

        // Release the context: produce() makes it current again on this thread,
        // but nothing may hold it while the factory waits on us.
        eglMakeCurrent(egl_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return true;
    }

    // Platform thread.
    bool negotiate()
    {
        IhsPvCapabilities caps{};
        caps.struct_size = sizeof(caps);
        int rc = ihs_pv_query_capabilities(&caps);
        if (rc != IHS_PV_OK) {
            std::fprintf(stderr, "[bev/ihs_pv] view %d: query_capabilities failed: %d\n", id_, rc);
            return false;
        }

        // A KMS plane only for buffers that can be scanned out, i.e. gbm bos
        // from the shell's own device.
        const bool plane = params_.scanout && allocator_ == RingAllocator::ShellGbm &&
                           (caps.kinds & IHS_PV_KIND_DRM_PLANE);
        if (!plane && !(caps.kinds & IHS_PV_KIND_TEXTURE_DMABUF_IMPORT)) {
            std::fprintf(stderr, "[bev/ihs_pv] view %d: backend '%s' offers no dma-buf path "
                                 "(kinds=0x%x), and this view has no software fallback\n",
                         id_, caps.backend_key ? caps.backend_key : "?", caps.kinds);
            return false;
        }

        IhsPvRequirements req{};
        req.struct_size  = sizeof(req);
        req.kinds        = plane ? IHS_PV_KIND_DRM_PLANE : IHS_PV_KIND_TEXTURE_DMABUF_IMPORT;
        req.formats      = formats_.data();
        req.format_count = formats_.size();
        req.needs_alpha  = 0;
        req.sync         = IHS_PV_SYNC_EXPLICIT_PREFERRED;
        req.z_order      = plane ? IHS_PV_Z_BELOW_FLUTTER : IHS_PV_Z_INLINE;

        IhsPvGrant grant{};
        grant.struct_size = sizeof(grant);
        rc = ihs_pv_negotiate(view_, &req, &grant);
        if (rc != IHS_PV_OK) {
            std::fprintf(stderr, "[bev/ihs_pv] view %d: negotiate failed: %d\n", id_, rc);
            return false;
        }
        if (grant.granted_kind != IHS_PV_KIND_TEXTURE_DMABUF_IMPORT &&
            grant.granted_kind != IHS_PV_KIND_DRM_PLANE) {
            std::fprintf(stderr, "[bev/ihs_pv] view %d: granted %s, which this producer can't fill\n",
                         id_, kind_name(grant.granted_kind));
            return false;
        }
        if (grant.format.fourcc == 0) grant.format = formats_.front();

        {
            std::lock_guard<std::mutex> lock(grant_mutex_);
            grant_ = grant;
        }
        std::printf("[bev/ihs_pv] view %d on '%s': %s fourcc=0x%08x mod=0x%llx sync=%u, "
                    "buffers via %s, GL on %s\n",
                    id_, caps.backend_key ? caps.backend_key : "?", kind_name(grant.granted_kind),
                    grant.format.fourcc, (unsigned long long)grant.format.modifier, grant.sync,
                    ring_allocator_name(allocator_), render_node_.c_str());
        return true;
    }

    void render_loop()
    {
        const bool ready = setup_gl();
        bool go = false;
        {
            std::unique_lock<std::mutex> lock(phase_mutex_);
            phase_ = ready ? Phase::Ready : Phase::SetupFailed;
            phase_cv_.notify_all();
            if (ready) {
                phase_cv_.wait(lock, [this] { return phase_ == Phase::Go || phase_ == Phase::Abort; });
                go = phase_ == Phase::Go;
            }
        }
        if (go && eglMakeCurrent(egl_.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_.ctx))
            produce();

        pipeline_.shutdown();
        drain_release_fences();
        ring_.release();
        teardown_egl(egl_);
    }

    void produce()
    {
        bool pipeline_ok = pipeline_.init(egl_, params_);
        if (!pipeline_ok)
            std::fprintf(stderr, "[bev/ihs_pv] view %d: pipeline did not start; showing black\n", id_);
        if (!allocate_ring()) return;

        // Set after a black frame goes up for a stopped source, so the thread
        // parks instead of re-submitting it; cleared by anything that changes
        // the buffers.
        bool idle = false;
        auto window_start = std::chrono::steady_clock::now();
        uint64_t window_frames = 0;
        const auto produce_start = std::chrono::steady_clock::now();
        uint64_t loop_frames = 0;
        unsigned timeout_lines = 0;

        while (running_) {
            if (regrant_.exchange(false)) {
                if (!allocate_ring()) break;
                idle = false;
            }
            if (suspended_) {
                std::this_thread::sleep_for(kSuspendedPoll);
                continue;
            }
            const int32_t w = pending_width_, h = pending_height_;
            if (w > 0 && h > 0 && (w != width_ || h != height_)) {
                width_  = w;
                height_ = h;
                if (!allocate_ring()) break;
                idle = false;
            }
            run_commands();

            if (pipeline_ok) {
                if (pipeline_.render_next()) {
                    std::lock_guard<std::mutex> lock(stats_mutex_);
                    ++stats_.frames_rendered;
                } else {
                    if (!running_) break;
                    std::fprintf(stderr, "[bev/ihs_pv] view %d: source stopped producing frames\n", id_);
                    pipeline_ok = false;
                    idle = false;
                }
            }
            if (!pipeline_ok && idle) {
                std::this_thread::sleep_for(kIdlePoll);
                continue;
            }

            // Only draw over a slot once the compositor has let go of it.
            const FenceWait fence = wait_and_close_fence(release_fences_[slot_]);
            release_fences_[slot_] = -1;
            if (fence.waited) {
                std::lock_guard<std::mutex> lock(stats_mutex_);
                ++stats_.release_waits;
                if (!fence.signalled) ++stats_.release_timeouts;
                stats_.release_wait_ms += (double)fence.ns / 1.0e6;
            }
            // Which slot and when, for each timeout: a count alone can't tell a
            // one-off event from steady load, or say what the compositor was
            // doing at the time. Wall-clock time lines up with the shell's log.
            if (fence.waited && !fence.signalled && timeout_lines < 32) {
                ++timeout_lines;
                const double since = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - produce_start).count();
                timespec ts{};
                clock_gettime(CLOCK_REALTIME, &ts);
                tm local{};
                localtime_r(&ts.tv_sec, &local);
                std::printf("[bev/ihs_pv] view %d: release timeout slot %u buffer %u "
                            "frame %llu at +%.3fs (%02d:%02d:%02d.%03ld)\n",
                            id_, slot_, ring_.buffer(slot_).buffer_id,
                            (unsigned long long)loop_frames, since, local.tm_hour,
                            local.tm_min, local.tm_sec, ts.tv_nsec / 1000000L);
            }
            ++loop_frames;

            if (pipeline_ok)
                ring_.compose(slot_, pipeline_.output_texture(), pipeline_.width(),
                              pipeline_.height(), params_.fit);
            else
                ring_.clear(slot_);
            const bool submitted = submit(slot_, ring_.finish());
            idle = !pipeline_ok;
            slot_ = (slot_ + 1) % ring_.size();

            const auto now = std::chrono::steady_clock::now();
            if (submitted) ++window_frames;
            const double window_s = std::chrono::duration<double>(now - window_start).count();
            std::lock_guard<std::mutex> lock(stats_mutex_);
            if (submitted) ++stats_.frames_submitted;
            if (window_s >= 1.0) {
                stats_.fps    = window_frames / window_s;
                window_start  = now;
                window_frames = 0;
            }
            stats_.running       = pipeline_ok ? 1 : 0;
            stats_.source_width  = pipeline_.width();
            stats_.source_height = pipeline_.height();
            stats_.overlap       = pipeline_.overlap();
            stats_.blend_edge    = pipeline_.blend_edge();
        }
        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.running = 0;
        stats_.fps     = 0.0;
    }

    // (Re)build the ring for the current grant and size. Render thread.
    bool allocate_ring()
    {
        IhsPvGrant grant;
        {
            std::lock_guard<std::mutex> lock(grant_mutex_);
            grant = grant_;
        }
        // Every buffer is about to go: retire the whole ring's fences first.
        drain_release_fences();
        const uint32_t w = (uint32_t)std::max(width_, 1), h = (uint32_t)std::max(height_, 1);
        if (!ring_.allocate((uint32_t)params_.ring_size, w, h, grant.format.fourcc,
                            grant.format.modifier)) {
            std::fprintf(stderr, "[bev/ihs_pv] view %d: can't allocate %ux%u buffers for the "
                                 "grant; view inert\n", id_, w, h);
            return false;
        }
        release_fences_.assign(ring_.size(), -1);
        slot_ = 0;

        std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.view_width   = (int32_t)w;
        stats_.view_height  = (int32_t)h;
        stats_.granted_kind = grant.granted_kind;
        stats_.fourcc       = grant.format.fourcc;
        stats_.modifier     = ring_.buffer(0).modifier;
        stats_.allocator    = (int32_t)allocator_;
        return true;
    }

    bool submit(uint32_t slot, int acquire_fence_fd)
    {
        const RingBuffer &b = ring_.buffer(slot);

        IhsFrame frame{};
        frame.struct_size = sizeof(frame);
        frame.format      = {b.fourcc, 0, b.modifier};
        frame.color_space = IHS_COLOR_SPACE_DEFAULT; // RGB: no CSC
        frame.color_range = IHS_COLOR_RANGE_DEFAULT;
        frame.width       = b.width;
        frame.height      = b.height;
        frame.plane_count = 1;
        for (int &fd : frame.plane_fd) fd = -1;
        // The registry consumes what it's handed; the ring keeps its own fd.
        frame.plane_fd[0]     = dup(b.fd);
        frame.plane_offset[0] = b.offset;
        frame.plane_stride[0] = b.stride;
        frame.hdr             = nullptr;
        frame.buffer_id       = b.buffer_id;

        // Always ask for a release fence, whatever sync was negotiated: the
        // DRM_PLANE path hands one back even under implicit sync.
        int release_fence_fd = -1;
        const int rc = ihs_pv_submit(view_, &frame, acquire_fence_fd, &release_fence_fd);
        if (rc != IHS_PV_OK) {
            // Ownership only transfers on success.
            if (frame.plane_fd[0] >= 0) close(frame.plane_fd[0]);
            static std::once_flag warned;
            std::call_once(warned, [&] {
                std::fprintf(stderr, "[bev/ihs_pv] view %d: submit failed: %d\n", id_, rc);
            });
            return false;
        }
        // Both halves of explicit sync, once: "fence waits 0" in the stats is
        // ambiguous on its own -- it reads the same whether the producer never
        // made an acquire fence or the compositor never handed a release one
        // back. Saying which is missing turns that into a one-line diagnosis.
        static std::once_flag first_submit;
        std::call_once(first_submit, [&] {
            std::printf("[bev/ihs_pv] first submit: acquire fence %s, release fence %s\n",
                        acquire_fence_fd >= 0 ? "yes" : "no (implicit)",
                        release_fence_fd >= 0 ? "yes" : "no");
        });
        release_fences_[slot] = release_fence_fd;
        return true;
    }

    void run_commands()
    {
        std::deque<Command> pending;
        {
            std::lock_guard<std::mutex> lock(commands_mutex_);
            pending.swap(commands_);
        }
        if (!pipeline_.renderer()) return;
        for (auto &command : pending) command(pipeline_);
    }

    void drain_release_fences()
    {
        for (int &fd : release_fences_) {
            wait_and_close_fence(fd);
            fd = -1;
        }
    }

    IhsPlatformView *view_;
    const int32_t    id_;
    const BevParams  params_;

    // Shell side, fixed before the render thread starts.
    std::string                    backend_;
    std::vector<IhsFormatModifier> shell_formats_;
    RingAllocator                  allocator_ = RingAllocator::RenderNode;
    gbm_device                    *shell_gbm_ = nullptr;
    std::string                    render_node_;
#ifdef BEV_IHS_PV_VULKAN
    std::unique_ptr<VkDmabufAllocator> vk_alloc_;
    const VkDmabufAllocator *vk_allocator() const { return vk_alloc_.get(); }
#else
    const VkDmabufAllocator *vk_allocator() const { return nullptr; }
#endif

    // Written by the render thread during setup, read-only once negotiating.
    std::vector<IhsFormatModifier> formats_;

    std::mutex              phase_mutex_;
    std::condition_variable phase_cv_;
    Phase                   phase_ = Phase::SettingUp;

    std::thread       thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> suspended_{false};
    std::atomic<bool> regrant_{false};

    std::atomic<int32_t> pending_width_;
    std::atomic<int32_t> pending_height_;
    int32_t              width_;
    int32_t              height_;

    std::mutex grant_mutex_;
    IhsPvGrant grant_{};

    // Render thread only.
    EGLState         egl_;
    DmabufRing       ring_;
    BevPipeline      pipeline_;
    std::vector<int> release_fences_; // per slot; -1 = nothing outstanding
    uint32_t         slot_ = 0;

    std::mutex          commands_mutex_;
    std::deque<Command> commands_;

    std::mutex   stats_mutex_;
    BevViewStats stats_{};
};

std::mutex                          g_views_mutex;
std::map<int64_t, ViewInstance *>   g_views;

// Runs @fn against the live view @id under the registry lock, so dispose can't
// free it underneath. Returns false for an unknown id.
template <typename Fn>
bool with_view(int64_t id, Fn &&fn)
{
    std::lock_guard<std::mutex> lock(g_views_mutex);
    const auto it = g_views.find(id);
    if (it == g_views.end()) return false;
    fn(*it->second);
    return true;
}

// -- IhsPvCallbacks trampolines -----------------------------------------------

void on_resize(void *user_data, double width, double height)
{
    static_cast<ViewInstance *>(user_data)->on_resize(width, height);
}

void on_suspended(void *user_data, uint8_t suspended)
{
    static_cast<ViewInstance *>(user_data)->on_suspended(suspended != 0);
}

void on_renegotiate(void *user_data)
{
    static_cast<ViewInstance *>(user_data)->on_renegotiate();
}

void on_dispose(void *user_data)
{
    auto *view = static_cast<ViewInstance *>(user_data);
    {
        std::lock_guard<std::mutex> lock(g_views_mutex);
        g_views.erase(view->id());
    }
    // Joins the render thread here: the registry releases the grant as soon
    // as this returns, and a producer still submitting would outlive its view.
    delete view;
}

int bev_factory(const IhsPvCreateInfo *info, void * /*factory_user_data*/,
                IhsPlatformView *view, IhsPvCallbacks *out_callbacks, void **out_user_data)
{
    if (!info || !view || !out_callbacks || !out_user_data) return IHS_PV_ERR_INVALID;

    const char *assets = ihs_pv_assets_path ? ihs_pv_assets_path() : nullptr;
    BevParams params;
    std::string error;
    if (!bev_params_parse(info->params, info->params_size, assets ? assets : "", params, error)) {
        std::fprintf(stderr, "[bev/ihs_pv] view %d: bad creation params: %s\n", info->id,
                     error.c_str());
        return IHS_PV_ERR_INVALID;
    }

    // The create message carries a placeholder size; the real one arrives
    // with the first resize.
    const int32_t width  = info->width  > 0 ? (int32_t)info->width  : 1;
    const int32_t height = info->height > 0 ? (int32_t)info->height : 1;

    auto instance = std::make_unique<ViewInstance>(view, info->id, std::move(params), width, height);
    if (!instance->start()) return IHS_PV_ERR_UNSUPPORTED;

    out_callbacks->resize        = on_resize;
    out_callbacks->set_suspended = on_suspended;
    out_callbacks->renegotiate   = on_renegotiate;
    out_callbacks->dispose       = on_dispose;
    // on_touch / accept_gesture / reject_gesture stay NULL: Flutter widgets
    // over the view take input, and controls arrive through bev_view_*.
    {
        std::lock_guard<std::mutex> lock(g_views_mutex);
        g_views[info->id] = instance.get();
    }
    *out_user_data = instance.release();
    return IHS_PV_OK;
}

} // namespace

extern "C" BEV_EXPORT void bev_register_ihs_pv(void)
{
    // A shell on a board runs with stdout redirected to a log file, where it is
    // block-buffered: every line below -- the grant, the ring, the renderer's
    // own startup lines -- would otherwise surface only when the process exits,
    // which is exactly when a diagnostic is no longer useful. Only the stderr
    // warnings got through, so a log read mid-run looked like a view that had
    // never started.
    static std::once_flag line_buffered;
    std::call_once(line_buffered, [] { setvbuf(stdout, nullptr, _IOLBF, 0); });

    const int rc = ihs_pv_register_factory(kViewType, bev_factory, nullptr);
    if (rc != IHS_PV_OK)
        std::fprintf(stderr, "[bev/ihs_pv] register_factory('%s') failed: %d\n", kViewType, rc);
}

extern "C" BEV_EXPORT void bev_unregister_ihs_pv(void)
{
    ihs_pv_unregister_factory(kViewType);
}

extern "C" BEV_EXPORT int32_t bev_view_stats(int64_t view_id, BevViewStats *out)
{
    if (!out) return 0;
    return with_view(view_id, [&](ViewInstance &v) { v.stats(*out); }) ? 1 : 0;
}

extern "C" BEV_EXPORT void bev_view_set_overlap(int64_t view_id, double value)
{
    with_view(view_id, [&](ViewInstance &v) {
        v.post([value](BevPipeline &p) { p.set_overlap((float)value); });
    });
}

extern "C" BEV_EXPORT void bev_view_set_blend_edge(int64_t view_id, double value)
{
    with_view(view_id, [&](ViewInstance &v) {
        v.post([value](BevPipeline &p) { p.set_blend_edge((float)value); });
    });
}

extern "C" BEV_EXPORT void bev_view_set_free_yaw(int64_t view_id, int32_t enable)
{
    with_view(view_id, [&](ViewInstance &v) {
        v.post([enable](BevPipeline &p) { p.renderer()->set_free_yaw(enable != 0); });
    });
}

extern "C" BEV_EXPORT void bev_view_set_car_center(int64_t view_id, double x_m, double y_m)
{
    with_view(view_id, [&](ViewInstance &v) {
        v.post([x_m, y_m](BevPipeline &p) { p.renderer()->set_car_center((float)x_m, (float)y_m); });
    });
}

extern "C" BEV_EXPORT int32_t bev_view_snapshot(int64_t view_id, const char *path)
{
    if (!path || !*path) return 0;
    std::string target = path;
    return with_view(view_id, [&](ViewInstance &v) {
        v.post([target](BevPipeline &p) { p.renderer()->save_snapshot(target.c_str()); });
    }) ? 1 : 0;
}
