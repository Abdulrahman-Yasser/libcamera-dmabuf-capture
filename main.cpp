#include "egl_context.h"
#include "capture_session.h"
#include "gpu_renderer.h"
#include "file_source.h"
#include "perf_timer.h"

#include <libcamera/libcamera.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <memory>
#include <vector>

#include <cfloat>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

using namespace libcamera;

static volatile sig_atomic_t g_running = 1;

// Read process RSS (Resident Set Size) from /proc/self/status — cheap, no syscall overhead.
static long read_rss_kb()
{
    FILE *f = std::fopen("/proc/self/status", "r");
    if (!f) return -1;
    char line[128];
    long kb = -1;
    while (std::fgets(line, sizeof(line), f))
        if (std::sscanf(line, "VmRSS: %ld kB", &kb) == 1) break;
    std::fclose(f);
    return kb;
}

// Put terminal in raw mode so keypresses register without pressing Enter.
static struct termios g_old_tio;
static void enable_raw_stdin()
{
    tcgetattr(STDIN_FILENO, &g_old_tio);
    struct termios raw = g_old_tio;
    raw.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);
}
static void restore_stdin()
{
    tcsetattr(STDIN_FILENO, TCSANOW, &g_old_tio);
}

// ── Dual file mode ────────────────────────────────────────────────────────────

static int run_dual_file_mode(const char *lpath, const char *rpath, EGLState &egl)
{
    FileSource lsrc, rsrc;
    if (!lsrc.open(lpath))  return 1;
    if (!rsrc.open(rpath))  return 1;

    DmaBufFrame lf = lsrc.nextFrame();
    DmaBufFrame rf = rsrc.nextFrame();
    if (!lf.data) { std::cerr << "[dual] failed to get first left frame\n";  return 1; }
    if (!rf.data) { std::cerr << "[dual] failed to get first right frame\n"; return 1; }

    GpuRenderer renderer;
    if (!renderer.init_dual(egl, lf.width, lf.height, lf.stride)) return 1;

    enable_raw_stdin();
    float overlap    = 0.86f;
    float blend_edge = 0.45f;
    std::printf("[stitch] overlap=%.2f edge=%.2f  "
                "+/-=overlap  [/]=sharpness  s=snapshot  Ctrl+C=stop\n",
                overlap, blend_edge);

    uint64_t total_frames = 0;

    while (g_running && lf.data && rf.data) {
        renderer.render_frame(lf, rf);
        ++total_frames;

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) == 1) {
            if (key == 's' || key == 'S') {
                static int snap_idx = 0;
                char snap_path[64];
                std::snprintf(snap_path, sizeof(snap_path), "/tmp/snapshot_%03d.png", snap_idx++);
                renderer.save_snapshot(snap_path);
            } else if (key == '+' || key == '=') {
                overlap = std::min(overlap + 0.01f, 0.95f);
                renderer.set_stitch_overlap(overlap);
                std::printf("[stitch] overlap=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == '-') {
                overlap = std::max(overlap - 0.01f, 0.05f);
                renderer.set_stitch_overlap(overlap);
                std::printf("[stitch] overlap=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == ']') {
                blend_edge = std::min(blend_edge + 0.01f, 0.49f);
                renderer.set_blend_edge(blend_edge);
                std::printf("[stitch] overlap=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == '[') {
                blend_edge = std::max(blend_edge - 0.01f, 0.0f);
                renderer.set_blend_edge(blend_edge);
                std::printf("[stitch] overlap=%.2f edge=%.2f\n", overlap, blend_edge);
            }
        }

        lf = lsrc.nextFrame();
        rf = rsrc.nextFrame();
    }

    std::printf("[loop] dual mode stopped after %llu frames\n",
                (unsigned long long)total_frames);
    restore_stdin();
    renderer.cleanup();
    return 0;
}

// ── Single file mode ──────────────────────────────────────────────────────────

static int run_file_mode(const char *path, EGLState &egl)
{
    FileSource fsrc;
    if (!fsrc.open(path)) return 1;

    // Pull first frame to discover dimensions before initialising the renderer.
    DmaBufFrame first = fsrc.nextFrame();
    if (!first.data) {
        std::cerr << "[file] failed to get first frame\n";
        return 1;
    }

    GpuRenderer renderer;
    if (!renderer.init(egl, first.width, first.height, first.stride)) return 1;

    enable_raw_stdin();
    std::cout << "[loop] file mode — press 's' to save snapshot, Ctrl+C to stop\n";

    DmaBufFrame frame = first;
    uint64_t total_frames = 0;

    while (g_running && frame.data) {
        renderer.render_frame(frame);
        ++total_frames;

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) == 1 && (key == 's' || key == 'S')) {
            static int snap_idx = 0;
            char snap_path[64];
            std::snprintf(snap_path, sizeof(snap_path), "/tmp/snapshot_%03d.png", snap_idx++);
            renderer.save_snapshot(snap_path);
        }

        frame = fsrc.nextFrame();
    }

    std::printf("[loop] file mode stopped after %llu frames\n",
                (unsigned long long)total_frames);
    restore_stdin();
    renderer.cleanup();
    return 0;
}

// ── Camera mode ───────────────────────────────────────────────────────────────

int main(int argc, char *argv[])
{
    std::signal(SIGINT,  [](int){ g_running = 0; });
    std::signal(SIGTERM, [](int){ g_running = 0; });

    // ── Parse arguments ───────────────────────────────────────────────────
    const char *file_path  = nullptr;
    const char *file_left  = nullptr;
    const char *file_right = nullptr;
    for (int i = 1; i < argc - 1; ++i) {
        std::string a = argv[i];
        if (a == "--file")        file_path  = argv[i + 1];
        else if (a == "--file-left")  file_left  = argv[i + 1];
        else if (a == "--file-right") file_right = argv[i + 1];
    }

    /* ---- EGL/GBM headless context ---- */
    EGLState egl;
    if (!setup_egl(egl)) {
        teardown_egl(egl); return 1;
    }
    if (!check_extensions(egl)) {
        std::cerr << "[egl] required extensions missing\n";
        teardown_egl(egl); return 1;
    }
    std::cout << "[egl] context ready\n\n";

    if (file_left && file_right) {
        int rc = run_dual_file_mode(file_left, file_right, egl);
        teardown_egl(egl);
        return rc;
    }
    if (file_path) {
        int rc = run_file_mode(file_path, egl);
        teardown_egl(egl);
        return rc;
    }

    /* ---- Camera init ---- */
    auto cm = std::make_unique<CameraManager>();
    if (cm->start()) {
        std::cerr << "[capture] CameraManager::start failed\n";
        teardown_egl(egl); return 1;
    }
    if (cm->cameras().empty()) {
        std::cerr << "[capture] no cameras detected\n";
        cm->stop(); teardown_egl(egl); return 1;
    }

    std::shared_ptr<Camera> camera = cm->cameras()[0];
    std::cout << "[capture] camera: " << camera->id() << "\n";

    if (camera->acquire()) {
        cm->stop(); teardown_egl(egl); return 1;
    }

    auto config = camera->generateConfiguration({StreamRole::Viewfinder});
    if (!config) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    StreamConfiguration &sc = config->at(0);
    sc.size        = {1640, 1232};
    sc.pixelFormat = formats::NV12;

    if (config->validate() == CameraConfiguration::Invalid) {
        std::cerr << "[capture] no supported format\n";
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }
    if (config->validate() == CameraConfiguration::Adjusted)
        std::cout << "[capture] config adjusted: " << sc.toString() << "\n";

    if (camera->configure(config.get())) {
        std::cerr << "[capture] configure failed\n";
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    /* ---- Buffer pool: one request per buffer ---- */
    Stream *stream = sc.stream();
    FrameBufferAllocator alloc(camera);
    if (alloc.allocate(stream) < 0) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    std::vector<std::unique_ptr<Request>> requests;
    for (auto &buf : alloc.buffers(stream)) {
        auto req = camera->createRequest();
        if (!req || req->addBuffer(stream, buf.get())) {
            camera->release(); cm->stop(); teardown_egl(egl); return 1;
        }
        requests.push_back(std::move(req));
    }
    std::printf("[capture] %zu buffer(s) allocated\n", requests.size());

    /* ---- GPU renderer: compile shader + FBO + EGLImage cache ---- */
    GpuRenderer renderer;
    if (!renderer.init(egl, sc, alloc.buffers(stream))) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }

    /* ---- Camera start + AE/AWB warmup ---- */
    CaptureSession session(sc, camera.get());
    camera->requestCompleted.connect(&session, &CaptureSession::requestCompleted);
    std::cout << "[capture] warming up AE/AWB ("
              << CaptureSession::WARMUP_FRAMES << " frames)...\n";

    if (camera->start()) {
        camera->release(); cm->stop(); teardown_egl(egl); return 1;
    }
    for (auto &req : requests)
        camera->queueRequest(req.get());

    session.waitWarmupDone();

    /* ---- Continuous render loop ---- */
    enable_raw_stdin();
    std::cout << "[loop] running — press 's' to save snapshot, Ctrl+C to stop\n";

    double   t_fps       = now_ms();
    double   t_prev      = t_fps;
    uint64_t fps_frames  = 0;
    uint64_t total_frames = 0;

    // Wall-clock frame time stats (camera-to-camera interval).
    double wall_sum_ms = 0, wall_min_ms = DBL_MAX, wall_max_ms = 0;
    // CPU time spent inside render_frame() — should be near 0 (just submits to GPU).
    double cpu_render_sum_ms = 0;

    while (g_running) {
        auto [buf, req] = session.nextFrame();
        if (!buf) break;

        double t_now        = now_ms();
        double wall_ms      = t_now - t_prev;
        t_prev              = t_now;

        double t_r0         = now_ms();
        renderer.render_frame(buf);
        double cpu_render_ms = now_ms() - t_r0;

        // Non-blocking keyboard check — cost is near zero when no key pressed.
        char key = 0;
        if (read(STDIN_FILENO, &key, 1) == 1 && (key == 's' || key == 'S')) {
            static int snap_idx = 0;
            char path[64];
            std::snprintf(path, sizeof(path), "/tmp/snapshot_%03d.png", snap_idx++);
            renderer.save_snapshot(path);
        }

        req->reuse(Request::ReuseBuffers);
        camera->queueRequest(req);

        ++fps_frames;
        ++total_frames;

        // Accumulate wall-clock stats (skip first frame — wall_ms is garbage).
        if (total_frames > 1) {
            wall_sum_ms     += wall_ms;
            if (wall_ms < wall_min_ms) wall_min_ms = wall_ms;
            if (wall_ms > wall_max_ms) wall_max_ms = wall_ms;
            cpu_render_sum_ms += cpu_render_ms;
        }

        // Print FPS + CPU render time + RSS once per second.
        double t_tick = now_ms();
        if (t_tick - t_fps >= 1000.0) {
            double fps     = fps_frames * 1000.0 / (t_tick - t_fps);
            long   rss_kb  = read_rss_kb();
            double cpu_avg = total_frames > 1
                             ? cpu_render_sum_ms / (total_frames - 1) : 0.0;
            std::printf("[perf] %4.1f fps | render_cpu avg %.3f ms | RSS %ld kB"
                        " | frames %llu\n",
                        fps, cpu_avg, rss_kb,
                        (unsigned long long)total_frames);
            t_fps      = t_tick;
            fps_frames = 0;
        }
    }

    std::printf("\n[loop] stopped after %llu frames\n",
                (unsigned long long)total_frames);

    // ── Full performance report ───────────────────────────────────────────
    uint64_t n = total_frames > 1 ? total_frames - 1 : 1;
    double   avg_wall   = wall_sum_ms   / n;
    double   avg_cpu    = cpu_render_sum_ms / n;
    double   actual_fps = avg_wall > 0 ? 1000.0 / avg_wall : 0.0;

    // Theoretical memory bandwidth per frame:
    //   Read:  NV12 = W*H*1.5 bytes (Y plane + UV half-res)
    //   Write: RGBA = W*H*4   bytes (FBO color attachment)
    int W = sc.size.width, H = sc.size.height;
    double nv12_mb   = (double)W * H * 1.5 / 1048576.0;
    double rgba_mb   = (double)W * H * 4.0 / 1048576.0;
    double bw_mbps   = (nv12_mb + rgba_mb) * actual_fps;

    const auto &gs   = renderer.gpu_stats();
    double gpu_avg   = gs.frames > 0 ? gs.sum_ms / gs.frames : 0.0;
    double gpu_ceil  = gpu_avg  > 0  ? 1000.0   / gpu_avg    : 0.0;

    std::printf("\n");
    std::printf("=== PERFORMANCE REPORT ================================\n");
    std::printf("  Resolution      : %dx%d\n", W, H);
    std::printf("  Frames captured : %llu\n", (unsigned long long)total_frames);
    std::printf("\n");
    std::printf("-- Profile (wall-clock, camera-to-camera) -------------\n");
    std::printf("  avg frame time  : %6.2f ms  (%4.1f fps actual)\n", avg_wall, actual_fps);
    std::printf("  min frame time  : %6.2f ms\n", wall_min_ms < DBL_MAX ? wall_min_ms : 0.0);
    std::printf("  max frame time  : %6.2f ms  (jitter: +%.2f ms)\n",
                wall_max_ms, wall_max_ms - (wall_min_ms < DBL_MAX ? wall_min_ms : 0.0));
    std::printf("\n");
    std::printf("-- CPU load (render_frame() call overhead) ------------\n");
    std::printf("  avg CPU/frame   : %6.3f ms  (rest is GPU + camera wait)\n", avg_cpu);
    std::printf("  CPU busy ratio  : %.1f%%  of frame time\n",
                avg_wall > 0 ? avg_cpu / avg_wall * 100.0 : 0.0);
    std::printf("\n");
    if (gs.available) {
        std::printf("-- GPU utilization (GL_EXT_disjoint_timer_query) ------\n");
        std::printf("  avg GPU time    : %6.3f ms  (ceiling: %.0f fps)\n", gpu_avg, gpu_ceil);
        std::printf("  min GPU time    : %6.3f ms\n", gs.min_ms < DBL_MAX ? gs.min_ms : 0.0);
        std::printf("  max GPU time    : %6.3f ms\n", gs.max_ms);
        std::printf("  GPU utilization : %.1f%%  of frame time\n",
                    avg_wall > 0 ? gpu_avg / avg_wall * 100.0 : 0.0);
        std::printf("  frames measured : %llu\n", (unsigned long long)gs.frames);
        std::printf("\n");
        std::printf("-- GPU cache preload (first %d frames after warmup) ---\n",
                    GpuRenderer::GpuStats::WARMUP_N);
        for (int i = 0; i < gs.warmup_count; ++i)
            std::printf("  frame %2d        : %6.3f ms\n", i + 1, gs.warmup_ms[i]);
        std::printf("\n");
    }
    std::printf("-- Memory bandwidth (theoretical) ----------------------\n");
    std::printf("  NV12 read/frame : %5.2f MB  (%.0f*%.0f * 1.5)\n",
                nv12_mb, (double)W, (double)H);
    std::printf("  RGBA write/frame: %5.2f MB  (%.0f*%.0f * 4)\n",
                rgba_mb, (double)W, (double)H);
    std::printf("  Total bandwidth : %5.1f MB/s  @ %.1f fps\n", bw_mbps, actual_fps);
    std::printf("\n");
    std::printf("-- Sysmem (RSS at exit) --------------------------------\n");
    std::printf("  RSS             : %ld kB\n", read_rss_kb());
    std::printf("=======================================================\n");

    /* ---- Cleanup ---- */
    restore_stdin();
    session.stop();
    camera->stop();
    renderer.cleanup();
    alloc.free(stream);
    camera->release();
    cm->stop();
    teardown_egl(egl);

    std::cout << "[exit] clean\n";
    return 0;
}
