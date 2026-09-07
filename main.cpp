#include "egl_context.h"
#include "capture_session.h"
#include "gpu_renderer.h"
#include "file_source.h"
#include "perf_timer.h"
#include "ipm.h"
#include "bev_config.h"
#ifdef HAVE_WAYLAND_PREVIEW
#include "wayland_window.h"
#endif

#include <libcamera/libcamera.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>

#include <cfloat>

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

using namespace libcamera;

// Drop-in for ground_to_image_H() when the pose is measured, not tuned.
// Reuses the same BEV-pixel -> ground mapping M; board centre (0.27,0.36 m)
// is offset to the canvas centre so it renders centred.
//
// dx/dy/dyaw_deg let a calibrated (has_hb2i) slot still be live-tuned: they
// are this slot's cam_x/cam_y/yaw *delta* away from the pose the ChArUco
// shot was taken at (0 = untouched, reproduces the original fixed-Hb2i
// behavior exactly). A ground-plane position+heading offset is the only
// thing that can be validly composed on top of an opaque, already-
// calibrated Hb2i -- cam_h/pitch are baked into Hb2i itself (the camera's
// actual height/tilt when the board was shot) and can't be recovered or
// re-applied without decomposing Hb2i back into K/R/t, so those two knobs
// are intentionally inert for calibrated slots (see the H/h/P/p keyboard
// handlers' warning below).
// Resolution the ChArUco board->image homographies (kHb2i_A/kHb2i_B in
// bev_config.cpp's bev_config_defaults()) were actually measured at --
// fixed, independent of img_w/img_h below (the runtime video's own frame
// size), which can be a different resolution entirely. Hb2i's raw output is
// in calibration-image pixels, so normalizing by anything other than the
// resolution it was measured at would silently rescale the homography.
static constexpr double CAL_W = 3280.0, CAL_H = 2464.0;

static mat3 measured_H(const mat3 &Hb2i, int img_w, int img_h,
                       double px_per_m, int bev_w, int bev_h,
                       double dx = 0.0, double dy = 0.0, double dyaw_deg = 0.0) {
    (void)img_w; (void)img_h; // kept for call-site compatibility; see CAL_W/CAL_H above
    const double r = px_per_m;
    const double Xc = 0.27, Yc = 0.36;
    mat3 S;
    S.at(0,0)= 1.0/r; S.at(0,1)= 0.0;   S.at(0,2)= -(bev_w/2.0)/r + Xc;
    S.at(1,0)= 0.0;   S.at(1,1)=-1.0/r; S.at(1,2)=  (bev_h/2.0)/r + Yc;
    S.at(2,0)= 0.0;   S.at(2,1)= 0.0;   S.at(2,2)= 1.0;

    // Rigid transform in board-metre space: rotate by dyaw about the board
    // origin, then translate by (dx,dy). Identity when untouched, so an
    // un-tuned slot renders exactly as before this fix.
    const double rad = dyaw_deg * M_PI / 180.0;
    const double ca = std::cos(rad), sa = std::sin(rad);
    mat3 T;
    T.at(0,0)= ca; T.at(0,1)=-sa; T.at(0,2)= dx;
    T.at(1,0)= sa; T.at(1,1)= ca; T.at(1,2)= dy;
    T.at(2,0)= 0;  T.at(2,1)=  0; T.at(2,2)= 1;

    mat3 H = Hb2i * T * S;
    for (int col = 0; col < 3; ++col) { H.at(0,col) /= CAL_W; H.at(1,col) /= CAL_H; }
    return H;
}

// Naming-convention lookup for a per-lens-model distortion file:
// "<lens_dir>/<lens_model>-lens.ini". lens_model is used verbatim -- no
// hyphenation normalization -- so "imx219" and "imx-219" are two different
// files, consistently with whatever the user actually typed/saved.
static std::string lens_calib_path(const std::string &lens_dir, const std::string &lens_model)
{
    if (lens_dir.empty() || lens_dir.back() == '/') return lens_dir + lens_model + "-lens.ini";
    return lens_dir + "/" + lens_model + "-lens.ini";
}

// Points at the active WaylandPreviewWindow, or null when --preview wasn't
// requested / this build has no preview support. Kept as a plain type alias
// (rather than #ifdef-ing every function signature that takes one) so
// run_file_mode()/run_dual_file_mode() have one signature regardless of
// HAVE_WAYLAND_PREVIEW.
#ifdef HAVE_WAYLAND_PREVIEW
using PreviewWindowPtr = WaylandPreviewWindow *;
#else
using PreviewWindowPtr = void *;
#endif

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

// Sleeps just enough to keep file-mode playback paced to the video's real
// frame rate when --preview is active. file_source.cpp's appsink runs with
// sync=FALSE, so nextFrame() otherwise returns frames as fast as the decoder
// produces them, which looks fast-forwarded in a live preview window. No-op
// when the framerate is unknown. Falls behind gracefully: if we're already
// past `next_due`, re-anchor to now rather than trying to catch up.
static void pace_to_framerate(double &next_due, double frame_duration_ms)
{
    if (frame_duration_ms <= 0.0) return;
    next_due += frame_duration_ms;
    double now = now_ms();
    if (next_due > now)
        std::this_thread::sleep_for(std::chrono::duration<double, std::milli>(next_due - now));
    else
        next_due = now;
}

// ── Dual file mode ────────────────────────────────────────────────────────────

// Inverse-perspective-mapping parameters (see ipm.h). Both cameras share
// everything except cam_x, which is ∓baseline/2.
struct IpmParams {
    double baseline = 0.40; // meters between the two cameras along X
    double cam_h    = 1.2;  // camera height above the ground, meters
    double pitch    = -30.0; // degrees; negative = looking down
    double yaw      = 0.0;   // degrees, rotation about vertical axis
    double cam_y    = 0.0;   // camera forward offset from world origin, meters
    double px_per_m = 100.0; // BEV ground-sampling density
};

static int run_dual_file_mode(const char *lpath, const char *rpath,
                              const EGLState &egl, PreviewWindowPtr preview,
                              const IpmParams &ipm_in)
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

    // Mutable working copy — runtime keys below adjust this and rebuild.
    IpmParams ipm = ipm_in;

    // Rebuilds H_left/H_right from the current `ipm` values and re-uploads
    // them. CPU cost is negligible (a handful of 3x3/4x4 multiplies) but
    // still only runs on demand — never per rendered frame.
    auto rebuild_ipm = [&](const char *changed) {
        mat3 K  = build_intrinsics(lf.width, lf.height);
        mat3 HL = ground_to_image_H(-ipm.baseline / 2, ipm.cam_y, ipm.cam_h,
                                    ipm.pitch, ipm.yaw, K,
                                    lf.width, lf.height, ipm.px_per_m,
                                    lf.width, lf.height);
        mat3 HR = ground_to_image_H( ipm.baseline / 2, ipm.cam_y, ipm.cam_h,
                                    ipm.pitch, ipm.yaw, K,
                                    lf.width, lf.height, ipm.px_per_m,
                                    lf.width, lf.height);
        float HLf[9], HRf[9];
        HL.to_floats(HLf);
        HR.to_floats(HRf);
        renderer.set_ipm(HLf, HRf);
        renderer.set_px_per_m((float)ipm.px_per_m);
        std::printf("[ipm] %-10s baseline=%.2fm h=%.2fm pitch=%.1fdeg yaw=%.1fdeg "
                    "cam_y=%.2fm px_per_m=%.1f\n",
                    changed, ipm.baseline, ipm.cam_h, ipm.pitch, ipm.yaw,
                    ipm.cam_y, ipm.px_per_m);
    };

    rebuild_ipm("init");

    enable_raw_stdin();
    float overlap    = 0.40f; // meters, blend half-width around baseline midline
    float blend_edge = 0.45f;
    std::printf("[stitch] overlap(m)=%.2f edge=%.2f\n", overlap, blend_edge);
    std::printf(
        "[keys] s=snapshot  +/-=overlap  [/]=sharpness  c=sanity-check\n"
        "       H/h=height+-  P/p=pitch+-  Y/y=yaw+-  B/b=baseline+-  "
        "G/g=cam_y+-  M/m=px_per_m+-  Ctrl+C=stop\n");

    uint64_t total_frames = 0;
    double   next_due     = now_ms();

    // Temporary workaround for the agl-compositor background-surface exit
    // crash (see wayland_window.cpp's top comment): loop each file forever
    // on EOS instead of ending the run, so this process — and therefore its
    // agl_shell background surface — never exits while --preview is active.
    auto loop_on_eos = [](FileSource &src, const char *path, DmaBufFrame &frame) {
        frame = src.nextFrame();
        if (!frame.data) {
            std::printf("[dual] EOS — looping %s\n", path);
            src.close();
            if (src.open(path)) frame = src.nextFrame();
        }
    };

    while (g_running && lf.data && rf.data) {
        renderer.render_frame(lf, rf);
        ++total_frames;

#ifdef HAVE_WAYLAND_PREVIEW
        if (preview) {
            preview->pump_events();
            preview->present(renderer.fbo_texture(), renderer.width(), renderer.height());
            if (!preview->running()) g_running = 0;
            pace_to_framerate(next_due, lsrc.frame_duration_ms());
        }
#endif

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) == 1) {
            if (key == 's' || key == 'S') {
                static int snap_idx = 0;
                char snap_path[64];
                std::snprintf(snap_path, sizeof(snap_path), "/tmp/snapshot_%03d.png", snap_idx++);
                renderer.save_snapshot(snap_path);
            } else if (key == '+' || key == '=') {
                overlap = std::min(overlap + 0.05f, 3.0f);
                renderer.set_stitch_overlap(overlap);
                std::printf("[stitch] overlap(m)=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == '-') {
                overlap = std::max(overlap - 0.05f, 0.05f);
                renderer.set_stitch_overlap(overlap);
                std::printf("[stitch] overlap(m)=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == ']') {
                blend_edge = std::min(blend_edge + 0.01f, 0.49f);
                renderer.set_blend_edge(blend_edge);
                std::printf("[stitch] overlap(m)=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == '[') {
                blend_edge = std::max(blend_edge - 0.01f, 0.0f);
                renderer.set_blend_edge(blend_edge);
                std::printf("[stitch] overlap(m)=%.2f edge=%.2f\n", overlap, blend_edge);
            } else if (key == 'c') {
                mat3 K  = build_intrinsics(lf.width, lf.height);
                mat3 HL = ground_to_image_H(-ipm.baseline / 2, ipm.cam_y, ipm.cam_h,
                                            ipm.pitch, ipm.yaw, K,
                                            lf.width, lf.height, ipm.px_per_m,
                                            lf.width, lf.height);
                mat3 HR = ground_to_image_H( ipm.baseline / 2, ipm.cam_y, ipm.cam_h,
                                            ipm.pitch, ipm.yaw, K,
                                            lf.width, lf.height, ipm.px_per_m,
                                            lf.width, lf.height);
                ipm_debug_check(HL, lf.width, lf.height, "left");
                ipm_debug_check(HR, lf.width, lf.height, "right");
            } else if (key == 'H') {
                ipm.cam_h = std::min(ipm.cam_h + 0.05, 3.0);
                rebuild_ipm("height+");
            } else if (key == 'h') {
                ipm.cam_h = std::max(ipm.cam_h - 0.05, 0.2);
                rebuild_ipm("height-");
            } else if (key == 'P') {
                ipm.pitch = std::min(ipm.pitch + 1.0, -1.0);
                rebuild_ipm("pitch+");
            } else if (key == 'p') {
                ipm.pitch = std::max(ipm.pitch - 1.0, -89.0);
                rebuild_ipm("pitch-");
            } else if (key == 'Y') {
                ipm.yaw = std::min(ipm.yaw + 1.0, 90.0);
                rebuild_ipm("yaw+");
            } else if (key == 'y') {
                ipm.yaw = std::max(ipm.yaw - 1.0, -90.0);
                rebuild_ipm("yaw-");
            } else if (key == 'B') {
                ipm.baseline = std::min(ipm.baseline + 0.02, 2.0);
                rebuild_ipm("baseline+");
            } else if (key == 'b') {
                ipm.baseline = std::max(ipm.baseline - 0.02, 0.05);
                rebuild_ipm("baseline-");
            } else if (key == 'G') {
                ipm.cam_y = std::min(ipm.cam_y + 0.05, 5.0);
                rebuild_ipm("cam_y+");
            } else if (key == 'g') {
                ipm.cam_y = std::max(ipm.cam_y - 0.05, -5.0);
                rebuild_ipm("cam_y-");
            } else if (key == 'M') {
                ipm.px_per_m = std::min(ipm.px_per_m + 5.0, 500.0);
                rebuild_ipm("px_per_m+");
            } else if (key == 'm') {
                ipm.px_per_m = std::max(ipm.px_per_m - 5.0, 10.0);
                rebuild_ipm("px_per_m-");
            }
        }

        loop_on_eos(lsrc, lpath, lf);
        loop_on_eos(rsrc, rpath, rf);
    }

    std::printf("[loop] dual mode stopped after %llu frames\n",
                (unsigned long long)total_frames);
    restore_stdin();
    renderer.cleanup();
    return 0;
}

// ── Single file mode ──────────────────────────────────────────────────────────

static int run_file_mode(const char *path, const EGLState &egl, PreviewWindowPtr preview,
                         const IpmParams &ipm_in, bool bev_requested)
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

    // Forward BEV (single camera, no blend) — first step toward the planned
    // front/back/left/right 4-camera BEV blend; reuses the exact same
    // ground_to_image_H() math as the dual-camera stitch, just cam_x=0 (no
    // baseline) since there's only one camera here.
    bool bev_on = false;
    IpmParams ipm = ipm_in;
    if (bev_requested) {
        if (!renderer.init_bev()) return 1;
        renderer.set_bev_enabled(true);
        bev_on = true;
    }

    auto rebuild_bev = [&](const char *changed) {
        mat3 K = build_intrinsics(first.width, first.height);
        mat3 H = ground_to_image_H(0.0, ipm.cam_y, ipm.cam_h, ipm.pitch, ipm.yaw, K,
                                   first.width, first.height, ipm.px_per_m,
                                   first.width, first.height);
        float Hf[9];
        H.to_floats(Hf);
        renderer.set_bev(Hf);
        std::printf("[bev] %-10s h=%.2fm pitch=%.1fdeg yaw=%.1fdeg cam_y=%.2fm "
                    "px_per_m=%.1f\n",
                    changed, ipm.cam_h, ipm.pitch, ipm.yaw, ipm.cam_y, ipm.px_per_m);
    };
    if (bev_on) rebuild_bev("init");

    enable_raw_stdin();
    if (bev_on)
        std::printf("[keys] s=snapshot  c=sanity-check  H/h=height+-  P/p=pitch+-  "
                    "Y/y=yaw+-  G/g=cam_y+-  M/m=px_per_m+-  Ctrl+C=stop\n");
    else
        std::cout << "[loop] file mode — press 's' to save snapshot, Ctrl+C to stop\n";

    DmaBufFrame frame = first;
    uint64_t total_frames = 0;
    double   next_due     = now_ms();

    // Loop on EOS instead of ending the run — same as run_dual_file_mode
    // (see wayland_window.cpp's top comment re: the agl-compositor
    // background-surface exit crash) and, independent of that, generally
    // more useful for a live preview: keep playing rather than freezing on
    // the last frame once the clip runs out.
    auto loop_on_eos = [&]() {
        frame = fsrc.nextFrame();
        if (!frame.data) {
            std::printf("[file] EOS — looping %s\n", path);
            fsrc.close();
            if (fsrc.open(path)) frame = fsrc.nextFrame();
        }
    };

    while (g_running && frame.data) {
        renderer.render_frame(frame);
        ++total_frames;

#ifdef HAVE_WAYLAND_PREVIEW
        if (preview) {
            preview->pump_events();
            preview->present(renderer.fbo_texture(), renderer.width(), renderer.height());
            if (!preview->running()) g_running = 0;
            pace_to_framerate(next_due, fsrc.frame_duration_ms());
        }
#endif

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) == 1) {
            if (key == 's' || key == 'S') {
                static int snap_idx = 0;
                char snap_path[64];
                std::snprintf(snap_path, sizeof(snap_path), "/tmp/snapshot_%03d.png", snap_idx++);
                renderer.save_snapshot(snap_path);
            } else if (bev_on && key == 'c') {
                mat3 K = build_intrinsics(first.width, first.height);
                mat3 H = ground_to_image_H(0.0, ipm.cam_y, ipm.cam_h, ipm.pitch, ipm.yaw, K,
                                           first.width, first.height, ipm.px_per_m,
                                           first.width, first.height);
                ipm_debug_check(H, first.width, first.height, "forward");
            } else if (bev_on && key == 'H') {
                ipm.cam_h = std::min(ipm.cam_h + 0.05, 3.0);
                rebuild_bev("height+");
            } else if (bev_on && key == 'h') {
                ipm.cam_h = std::max(ipm.cam_h - 0.05, 0.2);
                rebuild_bev("height-");
            } else if (bev_on && key == 'P') {
                ipm.pitch = std::min(ipm.pitch + 1.0, -1.0);
                rebuild_bev("pitch+");
            } else if (bev_on && key == 'p') {
                ipm.pitch = std::max(ipm.pitch - 1.0, -89.0);
                rebuild_bev("pitch-");
            } else if (bev_on && key == 'Y') {
                ipm.yaw = std::min(ipm.yaw + 1.0, 90.0);
                rebuild_bev("yaw+");
            } else if (bev_on && key == 'y') {
                ipm.yaw = std::max(ipm.yaw - 1.0, -90.0);
                rebuild_bev("yaw-");
            } else if (bev_on && key == 'G') {
                ipm.cam_y = std::min(ipm.cam_y + 0.05, 5.0);
                rebuild_bev("cam_y+");
            } else if (bev_on && key == 'g') {
                ipm.cam_y = std::max(ipm.cam_y - 0.05, -5.0);
                rebuild_bev("cam_y-");
            } else if (bev_on && key == 'M') {
                ipm.px_per_m = std::min(ipm.px_per_m + 5.0, 500.0);
                rebuild_bev("px_per_m+");
            } else if (bev_on && key == 'm') {
                ipm.px_per_m = std::max(ipm.px_per_m - 5.0, 10.0);
                rebuild_bev("px_per_m-");
            }
        }

        loop_on_eos();
    }

    std::printf("[loop] file mode stopped after %llu frames\n",
                (unsigned long long)total_frames);
    restore_stdin();
    renderer.cleanup();
    return 0;
}

// ── N-camera surround-view mode ─────────────────────────────────────────────────

// Per-slot camera pose — fully independent, unlike IpmParams's shared
// height/pitch/yaw (dual mode) or hardcoded cam_x=0 (single-camera BEV
// mode). px_per_m and the BEV canvas size stay global/shared (one output
// canvas), so they're not in here.
struct SlotParams {
    double cam_x = 0.0, cam_y = 0.0, cam_h = 1.2, pitch = -30.0, yaw = 0.0;
};

struct Slot {
    std::string                 path;
    SlotParams                  params;
    std::unique_ptr<FileSource> src;
    DmaBufFrame                 frame;
};

// Which N-camera blend backend to use -- Feather is kFS_MULTI's existing
// single-pass angular-weighted blend, Pyramid is the multi-band Laplacian
// pyramid blend (GpuRenderer::init_multi_pyramid() et al.), a separate,
// comparable alternative selected via --blend pyramid. Both share the same
// slot setup/keyboard loop below; only init/per-slot-setter/render differ.
// Coverage reuses Feather's exact pipeline (kFS_MULTI/prog_multi_/init_multi)
// -- it only swaps that shader's per-fragment weight formula (see
// GpuRenderer::set_coverage_weight()) from the synthetic facing/overlap
// angular heuristic to a calibration-derived one (distance from each
// camera's own homography-valid image bounds), so it needs none of
// Pyramid's separate init/render/set_ipm dispatch.
enum class BlendMode { Feather, Pyramid, Coverage };

static int run_multi_file_mode(const std::vector<std::string> &paths,
                               const std::vector<int> &cfg_slots,
                               const EGLState &egl, PreviewWindowPtr preview,
                               double cli_px_per_m, bool px_per_m_from_cli,
                               BlendMode blend_mode, const std::string &config_path,
                               const std::string &car_icon_path,
                               double cli_car_width, bool car_width_from_cli,
                               double cli_car_length, bool car_length_from_cli,
                               double cli_car_x, bool car_x_from_cli,
                               double cli_car_y, bool car_y_from_cli,
                               const std::string &lens_dir)
{
    int n = (int)paths.size();
    int max_cams = (blend_mode == BlendMode::Pyramid)
                   ? GpuRenderer::kPyramidMaxCameras : GpuRenderer::kMaxCameras;
    if (n < 1 || n > max_cams) {
        std::cerr << "[multi] " << n << " sources requested, must be 1.."
                  << max_cams
                  << (blend_mode == BlendMode::Pyramid ? " (pyramid blend mode's lower cap)\n" : "\n");
        return 1;
    }

    BevConfig cfg;
    if (!bev_config_load(config_path, cfg)) return 1;

    // cfg_slots[i] is which bev_config.ini slot paths[i] reads pose/hb2i
    // from -- identity (0,1,2,...) for plain --src, but a fixed 0/1/2/3
    // mapping for --forward-*/--backward-*, so this has to be bounds- and
    // hb2i-checked per source rather than assuming paths[i] <-> cfg.slots[i].
    for (int i = 0; i < n; ++i) {
        if (cfg_slots[i] < 0 || cfg_slots[i] >= BevConfig::kMaxSlots) {
            std::cerr << "[multi] source " << i << " (" << paths[i] << ") maps to config "
                         "slot " << cfg_slots[i] << ", out of range 0.."
                      << (BevConfig::kMaxSlots - 1) << "\n";
            return 1;
        }
        if (!cfg.slots[cfg_slots[i]].has_hb2i) {
            std::cerr << "[multi] source " << i << " (" << paths[i] << ") maps to config "
                         "slot " << cfg_slots[i] << ", which has no hb2i configured in "
                      << config_path << " -- calibrate it with calibrate_bev.py "
                         "--slot-a/--slot-b first\n";
            return 1;
        }
    }

    std::vector<Slot> slots(n);
    for (int i = 0; i < n; ++i) {
        slots[i].path = paths[i];
        slots[i].src  = std::make_unique<FileSource>();
        if (!slots[i].src->open(slots[i].path)) {
            std::cerr << "[multi] failed to open source " << i << ": " << slots[i].path << "\n";
            return 1;
        }
        slots[i].frame = slots[i].src->nextFrame();
        if (!slots[i].frame.data) {
            std::cerr << "[multi] failed to get first frame for source " << i << "\n";
            return 1;
        }
        // Starting live pose is baseline + whatever delta was saved last time
        // (see BevSlotConfig::cam_x_delta) -- not just the bare baseline, or
        // a previously-tuned-and-saved offset would silently vanish on load.
        const BevSlotConfig &sc = cfg.slots[cfg_slots[i]];
        slots[i].params = SlotParams{ sc.cam_x + sc.cam_x_delta, sc.cam_y + sc.cam_y_delta,
                                       sc.cam_h, sc.pitch, sc.yaw + sc.yaw_delta };
    }

    // Shared BEV canvas size — slot 0's first-frame dimensions, fixed for
    // the whole run regardless of any individual slot's native resolution.
    // Each slot's homography is rebuilt from THAT slot's own frame
    // width/height every time (see rebuild_slot below); upload_nv12()
    // already re-specifies texture size on every call, so a live path-swap
    // to a differently-sized video needs no texture-reallocation logic.
    int canvas_w      = slots[0].frame.width;
    int canvas_h      = slots[0].frame.height;
    int canvas_stride = slots[0].frame.stride;

    GpuRenderer renderer;
    bool init_ok = (blend_mode == BlendMode::Pyramid)
        ? renderer.init_multi_pyramid(egl, canvas_w, canvas_h, canvas_stride, n)
        : renderer.init_multi(egl, canvas_w, canvas_h, canvas_stride, n);
    if (!init_ok) return 1;

    // CLI --car-width/--car-length/--car-x/--car-y win if given; otherwise
    // fall back to the config file's car_width/car_length/car_x/car_y (see
    // bev_config.h) -- same resolution pattern as px_per_m just below.
    double car_width_m  = car_width_from_cli  ? cli_car_width  : cfg.car_width;
    double car_length_m = car_length_from_cli ? cli_car_length : cfg.car_length;
    double car_x_m       = car_x_from_cli      ? cli_car_x      : cfg.car_x;
    double car_y_m       = car_y_from_cli      ? cli_car_y      : cfg.car_y;
    if (!car_icon_path.empty() &&
        !renderer.init_car_icon(car_icon_path.c_str(), (float)car_width_m, (float)car_length_m,
                                (float)car_x_m, (float)car_y_m)) {
        std::cerr << "[multi] car icon '" << car_icon_path
                  << "' failed to load -- continuing without it\n";
    }

    // CLI --px-per-m wins if given; otherwise fall back to the config file's
    // default (bev_config_defaults() if the file didn't specify one either).
    double px_per_m = px_per_m_from_cli ? cli_px_per_m : cfg.px_per_m;

    // Rebuilds one slot's homography from its current params + current
    // frame's own width/height, and re-uploads it. Cheap — only called on
    // a keypress (or a path-swap commit) for the one changed slot, never
    // per rendered frame.
    auto rebuild_slot = [&](int i, const char *changed) {
        const DmaBufFrame &f = slots[i].frame;
        const SlotParams  &p = slots[i].params;
        const BevSlotConfig &sc = cfg.slots[cfg_slots[i]];
        // Deltas from this slot's as-loaded (calibration-time) pose -- see
        // measured_H()'s comment for why only cam_x/cam_y/yaw can be
        // live-tuned this way. Zero until the user actually touches
        // X/x, G/g, or Y/y, so an untouched slot's projection is bit-for-bit
        // the original fixed-Hb2i result.
        double dx   = p.cam_x - sc.cam_x;
        double dy   = p.cam_y - sc.cam_y;
        double dyaw = p.yaw   - sc.yaw;
        mat3 H = measured_H(sc.hb2i, f.width, f.height, px_per_m, canvas_w, canvas_h,
                            dx, dy, dyaw);
        float Hf[9];
        H.to_floats(Hf);
        // The blend coverage wedge (which canvas sector this camera is
        // allowed to contribute to) is a fact of the physical mounting --
        // its own facing_deg config field, not derived from yaw at all, so
        // two cameras that are both basically front-facing (just nudged a
        // few degrees left/right via yaw) both correctly claim the front
        // sector instead of splitting 90 degrees apart. Live yaw-tuning
        // reprojects content via dyaw above without moving this boundary.
        float facing_deg = (float)sc.facing_deg;
        if (blend_mode == BlendMode::Pyramid)
            renderer.set_ipm_multi_pyramid(i, Hf, facing_deg);
        else
            renderer.set_ipm_multi(i, Hf, facing_deg);
        std::printf("[multi] slot %d %-10s cam_x=%.2fm cam_y=%.2fm h=%.2fm "
                    "pitch=%.1fdeg yaw=%.1fdeg (facing %.0fdeg, fixed)\n",
                    cfg_slots[i] + 1, changed, p.cam_x, p.cam_y, p.cam_h, p.pitch, p.yaw,
                    facing_deg);
    };
    auto rebuild_all = [&](const char *changed) {
        for (int i = 0; i < n; ++i) rebuild_slot(i, changed);
    };

    // Lens distortion has nothing to do with pose (fx/fy/../k1.. don't
    // depend on cam_x/y/h/pitch/yaw at all), so it's pushed separately from
    // rebuild_slot/rebuild_all -- rescales fx/fy/cx/cy from the lens file's
    // own calib_w/h to this slot's actual runtime frame size, pre-normalizes
    // into [0,1] uv terms, and uploads. has_distortion=false pushes the
    // all-zero/off state, matching init_multi()/init_multi_pyramid()'s own
    // default.
    auto push_distortion = [&](int i, const BevSlotConfig &sc) {
        float fxn = 0, fyn = 0, cxn = 0, cyn = 0;
        if (sc.has_distortion) {
            const DmaBufFrame &f = slots[i].frame;
            float sx = (float)f.width  / (float)sc.lens_calib_w;
            float sy = (float)f.height / (float)sc.lens_calib_h;
            fxn = (float)sc.fx * sx / (float)f.width;
            fyn = (float)sc.fy * sy / (float)f.height;
            cxn = (float)sc.cx * sx / (float)f.width;
            cyn = (float)sc.cy * sy / (float)f.height;
        }
        if (blend_mode == BlendMode::Pyramid)
            renderer.set_distortion_multi_pyramid(i, sc.has_distortion, fxn, fyn, cxn, cyn,
                (float)sc.k1, (float)sc.k2, (float)sc.k3, (float)sc.p1, (float)sc.p2);
        else
            renderer.set_distortion_multi(i, sc.has_distortion, fxn, fyn, cxn, cyn,
                (float)sc.k1, (float)sc.k2, (float)sc.k3, (float)sc.p1, (float)sc.p2);
    };

    // Looks up "<lens_dir>/<model>-lens.ini", and on success writes its
    // numbers straight into cfg.slots[cfg_slots[i]] (not a shadow copy) --
    // unlike pose, lens_model has no "delta from calibration baseline"
    // concept, so 'W' round-trips it for free via save_cfg = cfg, same as
    // has_hb2i/hb2i already do for fields 'W' never explicitly touches.
    // model=="" is the explicit/default "no lens" state -- quiet, not an
    // error. A model that doesn't resolve to a real file is NOT an error
    // either (graceful "continue without distortion correction", never
    // aborts the run) -- only a found-but-malformed file prints a sharper
    // diagnostic, from lens_calib_load() itself.
    auto load_lens_for_slot = [&](int i, const std::string &model) -> bool {
        BevSlotConfig &sc = cfg.slots[cfg_slots[i]];
        if (model.empty()) {
            sc.lens_model = "";
            sc.has_distortion = false;
            push_distortion(i, sc);
            return true;
        }
        std::string path = lens_calib_path(lens_dir, model);
        BevSlotConfig trial;
        if (!lens_calib_load(path, trial)) {
            std::printf("[multi] no lens calibration found for '%s' (%s) -- "
                        "slot %d continuing without distortion correction\n",
                        model.c_str(), path.c_str(), cfg_slots[i] + 1);
            return false;
        }
        sc.lens_model = model;
        sc.fx = trial.fx; sc.fy = trial.fy; sc.cx = trial.cx; sc.cy = trial.cy;
        sc.k1 = trial.k1; sc.k2 = trial.k2; sc.k3 = trial.k3;
        sc.p1 = trial.p1; sc.p2 = trial.p2;
        sc.lens_calib_w = trial.lens_calib_w;
        sc.lens_calib_h = trial.lens_calib_h;
        sc.has_distortion = true;
        push_distortion(i, sc);
        std::printf("[multi] slot %d lens '%s' -> %s (distortion correction ON)\n",
                    cfg_slots[i] + 1, model.c_str(), path.c_str());
        return true;
    };

    for (int i = 0; i < n; ++i)
        load_lens_for_slot(i, cfg.slots[cfg_slots[i]].lens_model);

    rebuild_all("init");

    enable_raw_stdin();
    int         active_slot  = 0;
    float       overlap_deg  = cfg.overlap_deg; // degrees, angular half-width (see BEV_ALGORITHM.md)
    float       blend_edge   = cfg.blend_edge;
    bool        in_path_edit = false;
    bool        in_lens_edit = false;
    bool        free_yaw     = false; // 'F' toggle -- see GpuRenderer::set_free_yaw()
    std::string path_buf;
    std::string lens_buf;
    renderer.set_stitch_overlap(overlap_deg);
    renderer.set_blend_edge(blend_edge);
    renderer.set_coverage_weight(blend_mode == BlendMode::Coverage);
    renderer.set_px_per_m((float)px_per_m);

    std::printf("[multi] %d source(s) loaded, blend=%s\n", n,
                blend_mode == BlendMode::Pyramid ? "pyramid" : "feather");
    for (int i = 0; i < n; ++i)
        std::printf("[multi]   source %d -> config slot %d (%s)\n",
                    i, cfg_slots[i] + 1, paths[i].c_str());
    std::printf(
        "[keys] s=snapshot  +/-=overlap(deg)  [/]=sharpness  c=sanity-check  "
        "F=toggle free-yaw\n"
        "       1..%d=select slot  E=edit path  L=edit lens_model\n"
        "       H/h=height+-  P/p=pitch+-  Y/y=yaw+-  G/g=cam_y+-  X/x=cam_x+-  "
        "M/m=px_per_m+- (global)\n"
        "       B/b=car_x+-  N/n=car_y+- (global, car icon)  "
        "W=save as default  Ctrl+C=stop\n", n);

    uint64_t total_frames = 0;
    double   next_due     = now_ms();

    auto loop_on_eos = [](Slot &slot) {
        slot.frame = slot.src->nextFrame();
        if (!slot.frame.data) {
            std::printf("[multi] EOS — looping %s\n", slot.path.c_str());
            slot.src->close();
            if (slot.src->open(slot.path)) slot.frame = slot.src->nextFrame();
        }
    };
    auto frames_valid = [&]() {
        for (auto &s : slots) if (!s.frame.data) return false;
        return true;
    };

    std::vector<DmaBufFrame> frames(n);

    while (g_running && frames_valid()) {
        for (int i = 0; i < n; ++i) frames[i] = slots[i].frame;
        if (blend_mode == BlendMode::Pyramid) renderer.render_frame_multi_pyramid(frames);
        else                                  renderer.render_frame_multi(frames);
        ++total_frames;

#ifdef HAVE_WAYLAND_PREVIEW
        if (preview) {
            preview->pump_events();
            preview->present(renderer.fbo_texture(), renderer.width(), renderer.height());
            if (!preview->running()) g_running = 0;
            pace_to_framerate(next_due, slots[0].src->frame_duration_ms());
        }
#endif

        char key = 0;
        if (read(STDIN_FILENO, &key, 1) == 1) {
            if (in_path_edit) {
                // Text-entry mode: ECHO is off in raw mode (enable_raw_stdin()),
                // so every printable byte is echoed back manually.
                if (key == '\r' || key == '\n') {
                    // Open-before-close: a bad path must never kill a
                    // working slot.
                    auto new_src = std::make_unique<FileSource>();
                    if (new_src->open(path_buf)) {
                        slots[active_slot].src->close();
                        slots[active_slot].src   = std::move(new_src);
                        slots[active_slot].path  = path_buf;
                        slots[active_slot].frame = slots[active_slot].src->nextFrame();
                        rebuild_slot(active_slot, "path");
                        // A differently-sized video changes this slot's own
                        // frame width/height, which the normalized fx/fy/cx/cy
                        // distortion uniforms were rescaled for -- re-push so
                        // they don't go stale at the wrong scale.
                        load_lens_for_slot(active_slot, cfg.slots[cfg_slots[active_slot]].lens_model);
                        std::printf("\n[multi] slot %d path -> %s\n",
                                    active_slot + 1, path_buf.c_str());
                    } else {
                        std::printf("\n[multi] failed to open '%s' — keeping previous source\n",
                                    path_buf.c_str());
                    }
                    in_path_edit = false;
                } else if (key == 0x1b) {
                    std::printf("\n[multi] path edit cancelled\n");
                    in_path_edit = false;
                } else if (key == 0x7f || key == 0x08) {
                    if (!path_buf.empty()) {
                        path_buf.pop_back();
                        std::fputs("\b \b", stdout);
                        std::fflush(stdout);
                    }
                } else if (key >= 0x20 && key < 0x7f) {
                    path_buf += key;
                    std::fputc(key, stdout);
                    std::fflush(stdout);
                }
            } else if (in_lens_edit) {
                // Exact structural mirror of in_path_edit above (same
                // commit/Esc/backspace/echo shape), but the commit action
                // delegates to load_lens_for_slot() rather than duplicating
                // lookup logic -- that function already does its own
                // build-before-swap safety (a scratch BevSlotConfig, only
                // written into cfg.slots[...] on success).
                if (key == '\r' || key == '\n') {
                    std::printf("\n");
                    load_lens_for_slot(active_slot, lens_buf);
                    in_lens_edit = false;
                } else if (key == 0x1b) {
                    std::printf("\n[multi] lens edit cancelled\n");
                    in_lens_edit = false;
                } else if (key == 0x7f || key == 0x08) {
                    if (!lens_buf.empty()) {
                        lens_buf.pop_back();
                        std::fputs("\b \b", stdout);
                        std::fflush(stdout);
                    }
                } else if (key >= 0x20 && key < 0x7f) {
                    lens_buf += key;
                    std::fputc(key, stdout);
                    std::fflush(stdout);
                }
            } else if (key == 's' || key == 'S') {
                static int snap_idx = 0;
                char snap_path[64];
                std::snprintf(snap_path, sizeof(snap_path), "/tmp/snapshot_%03d.png", snap_idx++);
                renderer.save_snapshot(snap_path);
            } else if (key == '+' || key == '=') {
                overlap_deg = std::min(overlap_deg + 5.0f, 360.0f);
                renderer.set_stitch_overlap(overlap_deg);
                std::printf("[multi] overlap(deg)=%.0f edge=%.2f\n", overlap_deg, blend_edge);
            } else if (key == '-') {
                overlap_deg = std::max(overlap_deg - 5.0f, 1.0f);
                renderer.set_stitch_overlap(overlap_deg);
                std::printf("[multi] overlap(deg)=%.0f edge=%.2f\n", overlap_deg, blend_edge);
            } else if (key == ']') {
                blend_edge = std::min(blend_edge + 0.01f, 0.49f);
                renderer.set_blend_edge(blend_edge);
                std::printf("[multi] overlap(deg)=%.0f edge=%.2f\n", overlap_deg, blend_edge);
            } else if (key == '[') {
                blend_edge = std::max(blend_edge - 0.01f, 0.0f);
                renderer.set_blend_edge(blend_edge);
                std::printf("[multi] overlap(deg)=%.0f edge=%.2f\n", overlap_deg, blend_edge);
            } else if (key == 'c') {
                for (int i = 0; i < n; ++i) {
                    const DmaBufFrame &f = slots[i].frame;
                    const SlotParams &p = slots[i].params;
                    const BevSlotConfig &sc = cfg.slots[cfg_slots[i]];
                    mat3 H = measured_H(sc.hb2i, f.width, f.height, px_per_m, canvas_w, canvas_h,
                                        p.cam_x - sc.cam_x,
                                        p.cam_y - sc.cam_y,
                                        p.yaw   - sc.yaw);
                    char label[16];
                    std::snprintf(label, sizeof(label), "slot%d", cfg_slots[i] + 1);
                    ipm_debug_check(H, canvas_w, canvas_h, label);
                }
            } else if (key == 'F') {
                free_yaw = !free_yaw;
                renderer.set_free_yaw(free_yaw);
                std::printf("[multi] free-yaw %s -- %s\n", free_yaw ? "ON" : "off",
                            free_yaw ? "wedge cutoff disabled, content visible "
                                       "anywhere its own homography is valid"
                                     : "back to facing-sector-limited blending");
            } else if (key == 'W') {
                BevConfig save_cfg   = cfg; // keeps has_hb2i/hb2i + any slot not currently loaded untouched
                save_cfg.px_per_m    = px_per_m;
                save_cfg.overlap_deg = overlap_deg;
                save_cfg.blend_edge  = blend_edge;
                save_cfg.car_x       = car_x_m;
                save_cfg.car_y       = car_y_m;
                save_cfg.car_width   = car_width_m;
                save_cfg.car_length  = car_length_m;
                for (int i = 0; i < n; ++i) {
                    const SlotParams &p = slots[i].params;
                    BevSlotConfig    &s = save_cfg.slots[cfg_slots[i]];
                    // cam_x/cam_y/yaw stay the fixed calibration-time baseline
                    // (read before being overwritten below) -- only the delta
                    // from it is saved, or a tuned offset would silently
                    // reset to 0 on the next load (see cam_x_delta's comment).
                    s.cam_x_delta = p.cam_x - s.cam_x;
                    s.cam_y_delta = p.cam_y - s.cam_y;
                    s.yaw_delta   = p.yaw   - s.yaw;
                    s.cam_h = p.cam_h;
                    s.pitch = p.pitch;
                }
                if (bev_config_save(config_path, save_cfg)) {
                    cfg = save_cfg;
                    std::printf("[multi] saved current values as defaults -> %s\n",
                                config_path.c_str());
                }
            } else if (key == 'M') {
                // Cap was 500 -- nowhere near enough for a hand-held-board
                // ChArUco calibration, whose valid ground span is much
                // smaller than a car-scale rig's, so it needed a canvas
                // representing thousands of px/m to not be mostly black
                // (see the coverage-map investigation this fixed). Bigger
                // step too, so tuning this doesn't take hundreds of presses.
                px_per_m = std::min(px_per_m + 50.0, 10000.0);
                renderer.set_px_per_m((float)px_per_m);
                rebuild_all("px_per_m+");
            } else if (key == 'm') {
                px_per_m = std::max(px_per_m - 50.0, 10.0);
                renderer.set_px_per_m((float)px_per_m);
                rebuild_all("px_per_m-");
            } else if (key == 'E' || key == 'e') {
                in_path_edit = true;
                path_buf.clear();
                std::printf("\n[multi] editing slot %d path (current: %s) — "
                            "type new path, Enter=commit, Esc=cancel:\n> ",
                            cfg_slots[active_slot] + 1, slots[active_slot].path.c_str());
                std::fflush(stdout);
            } else if (key == 'L' || key == 'l') {
                in_lens_edit = true;
                lens_buf.clear();
                const std::string &cur = cfg.slots[cfg_slots[active_slot]].lens_model;
                std::printf("\n[multi] editing slot %d lens_model (current: %s) — "
                            "type new model, Enter=commit, Esc=cancel, empty+Enter=clear:\n> ",
                            cfg_slots[active_slot] + 1, cur.empty() ? "(none)" : cur.c_str());
                std::fflush(stdout);
            } else if (key >= '1' && key <= '9') {
                int idx = key - '1';
                if (idx < n) {
                    active_slot = idx;
                    std::printf("[multi] active slot -> %d (%s)\n",
                                cfg_slots[active_slot] + 1, slots[active_slot].path.c_str());
                } else {
                    std::printf("[multi] slot %d doesn't exist (only %d loaded)\n", idx + 1, n);
                }
            } else if (key == 'H') {
                slots[active_slot].params.cam_h = std::min(slots[active_slot].params.cam_h + 0.05, 3.0);
                rebuild_slot(active_slot, "height+");
                if (cfg.slots[cfg_slots[active_slot]].has_hb2i)
                    std::printf("[multi] note: cam_h is baked into slot %d's calibrated "
                                "homography and has no visual effect here\n", cfg_slots[active_slot] + 1);
            } else if (key == 'h') {
                slots[active_slot].params.cam_h = std::max(slots[active_slot].params.cam_h - 0.05, 0.2);
                rebuild_slot(active_slot, "height-");
                if (cfg.slots[cfg_slots[active_slot]].has_hb2i)
                    std::printf("[multi] note: cam_h is baked into slot %d's calibrated "
                                "homography and has no visual effect here\n", cfg_slots[active_slot] + 1);
            } else if (key == 'P') {
                slots[active_slot].params.pitch = std::min(slots[active_slot].params.pitch + 1.0, -1.0);
                rebuild_slot(active_slot, "pitch+");
                if (cfg.slots[cfg_slots[active_slot]].has_hb2i)
                    std::printf("[multi] note: pitch is baked into slot %d's calibrated "
                                "homography and has no visual effect here\n", cfg_slots[active_slot] + 1);
            } else if (key == 'p') {
                slots[active_slot].params.pitch = std::max(slots[active_slot].params.pitch - 1.0, -89.0);
                rebuild_slot(active_slot, "pitch-");
                if (cfg.slots[cfg_slots[active_slot]].has_hb2i)
                    std::printf("[multi] note: pitch is baked into slot %d's calibrated "
                                "homography and has no visual effect here\n", cfg_slots[active_slot] + 1);
            } else if (key == 'Y') {
                slots[active_slot].params.yaw = std::fmod(slots[active_slot].params.yaw + 1.0, 360.0);
                rebuild_slot(active_slot, "yaw+");
            } else if (key == 'y') {
                slots[active_slot].params.yaw = std::fmod(slots[active_slot].params.yaw - 1.0 + 360.0, 360.0);
                rebuild_slot(active_slot, "yaw-");
            } else if (key == 'G') {
                slots[active_slot].params.cam_y = std::min(slots[active_slot].params.cam_y + 0.05, 5.0);
                rebuild_slot(active_slot, "cam_y+");
            } else if (key == 'g') {
                slots[active_slot].params.cam_y = std::max(slots[active_slot].params.cam_y - 0.05, -5.0);
                rebuild_slot(active_slot, "cam_y-");
            } else if (key == 'X') {
                slots[active_slot].params.cam_x = std::min(slots[active_slot].params.cam_x + 0.05, 5.0);
                rebuild_slot(active_slot, "cam_x+");
            } else if (key == 'x') {
                slots[active_slot].params.cam_x = std::max(slots[active_slot].params.cam_x - 0.05, -30.0);
                rebuild_slot(active_slot, "cam_x-");
            } else if (key == 'B') {
                car_x_m = std::min(car_x_m + 0.05, 5.0);
                renderer.set_car_center((float)car_x_m, (float)car_y_m);
                std::printf("[multi] car_x=%.2fm car_y=%.2fm\n", car_x_m, car_y_m);
            } else if (key == 'b') {
                car_x_m = std::max(car_x_m - 0.05, -5.0);
                renderer.set_car_center((float)car_x_m, (float)car_y_m);
                std::printf("[multi] car_x=%.2fm car_y=%.2fm\n", car_x_m, car_y_m);
            } else if (key == 'N') {
                car_y_m = std::min(car_y_m + 0.05, 5.0);
                renderer.set_car_center((float)car_x_m, (float)car_y_m);
                std::printf("[multi] car_x=%.2fm car_y=%.2fm\n", car_x_m, car_y_m);
            } else if (key == 'n') {
                car_y_m = std::max(car_y_m - 0.05, -5.0);
                renderer.set_car_center((float)car_x_m, (float)car_y_m);
                std::printf("[multi] car_x=%.2fm car_y=%.2fm\n", car_x_m, car_y_m);
            }
        }

        for (int i = 0; i < n; ++i) loop_on_eos(slots[i]);
    }

    std::printf("[loop] multi mode stopped after %llu frames\n",
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
    std::vector<std::string> sources;  // --src, repeatable; N-camera surround mode
    // Named alternative to --src for the 2-pair (front+back) surround rig --
    // each flag pins its source to a FIXED bev_config.ini slot (0/1/2/3)
    // instead of a --src's position in argv, so e.g. a backward-only run
    // still reads slot2/slot3's calibrated hb2i rather than slot0/slot1's.
    const char *fwd_left  = nullptr;
    const char *fwd_right = nullptr;
    const char *bwd_left  = nullptr;
    const char *bwd_right = nullptr;
    IpmParams   ipm;
    bool        preview_requested  = false;
    bool        bev_requested      = false;
    // --src mode's per-slot pose/homography defaults live in a config file
    // (see bev_config.h) instead of hardcoded arrays; --px-per-m still wins
    // over the file's default when passed, hence tracking whether it was.
    std::string config_path        = "bev_config.ini";
    bool        px_per_m_from_cli  = false;
    // Live camera mode only (the libcamera::CameraManager path at the bottom
    // of main(), not --src/--forward-*/--file's pre-recorded-video playback,
    // which never touches libcamera/the ISP at all) -- same mechanism
    // rpicam-apps's own --tuning-file uses under the hood: libcamera's RPi
    // pipeline handler reads this env var (once, at CameraManager startup)
    // to pick a non-default IPA tuning file instead of doing its normal
    // sensor-name-based lookup. Must be set before the CameraManager exists.
    std::string tuning_file;
    // Per-lens-model distortion files (see bev_config.h's lens_calib_load())
    // are looked up as "<lens_dir>/<lens_model>-lens.ini". Left empty here
    // and resolved to config_path's own directory once argv parsing is done
    // (see below) -- travels with wherever --config already points, no new
    // fixed-subdirectory convention to remember on top of it.
    std::string lens_dir;
    // --blend pyramid selects the multi-band Laplacian pyramid blend
    // (GpuRenderer::init_multi_pyramid() et al.) instead of the default
    // single-pass angular-weighted "feather" blend (kFS_MULTI) -- --src
    // mode only, a separate/comparable alternative, not a replacement.
    BlendMode   blend_mode = BlendMode::Feather;
    // Static top-down car icon composited over the BEV canvas's permanently
    // camera-blind center -- opt-in (no default path/CWD guessing, unlike
    // config_path's bev_config_defaults() fallback: there's no equivalent
    // built-in default icon to fall back to), and only matters if --car-icon
    // is given. Width/length/center default to the config file's car_width/
    // car_length/car_x/car_y (see bev_config.h) same as --px-per-m does for
    // px_per_m -- the CLI flags below only override when actually passed.
    std::string car_icon_path;
    double      cli_car_width  = 1.8;
    double      cli_car_length = 4.5;
    double      cli_car_x      = 0.0;
    double      cli_car_y      = 0.0;
    bool        car_width_from_cli  = false;
    bool        car_length_from_cli = false;
    bool        car_x_from_cli      = false;
    bool        car_y_from_cli      = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--file"            && i + 1 < argc) file_path  = argv[++i];
        else if (a == "--file-left"  && i + 1 < argc) file_left  = argv[++i];
        else if (a == "--file-right" && i + 1 < argc) file_right = argv[++i];
        else if (a == "--src"        && i + 1 < argc) sources.push_back(argv[++i]);
        else if (a == "--forward-left"   && i + 1 < argc) fwd_left   = argv[++i];
        else if (a == "--forward-right"  && i + 1 < argc) fwd_right  = argv[++i];
        else if (a == "--backward-left"  && i + 1 < argc) bwd_left   = argv[++i];
        else if (a == "--backward-right" && i + 1 < argc) bwd_right  = argv[++i];
        else if (a == "--preview") preview_requested = true;
        else if (a == "--bev") bev_requested = true;
        else if (a == "--config"     && i + 1 < argc) config_path = argv[++i];
        else if (a == "--lens-dir"   && i + 1 < argc) lens_dir    = argv[++i];
        else if (a == "--tuning-file" && i + 1 < argc) tuning_file = argv[++i];
        else if (a == "--blend"      && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "pyramid") blend_mode = BlendMode::Pyramid;
            else if (mode == "feather") blend_mode = BlendMode::Feather;
            else if (mode == "coverage") blend_mode = BlendMode::Coverage;
            else { std::cerr << "[multi] unknown --blend mode '" << mode << "' (use feather|pyramid|coverage)\n"; return 1; }
        }
        else if (a == "--baseline"   && i + 1 < argc) ipm.baseline = std::atof(argv[++i]);
        else if (a == "--h"          && i + 1 < argc) ipm.cam_h    = std::atof(argv[++i]);
        else if (a == "--pitch"      && i + 1 < argc) ipm.pitch    = std::atof(argv[++i]);
        else if (a == "--yaw"        && i + 1 < argc) ipm.yaw      = std::atof(argv[++i]);
        else if (a == "--cam-y"      && i + 1 < argc) ipm.cam_y    = std::atof(argv[++i]);
        else if (a == "--px-per-m"   && i + 1 < argc) {
            ipm.px_per_m      = std::atof(argv[++i]);
            px_per_m_from_cli = true;
        }
        else if (a == "--car-icon"   && i + 1 < argc) car_icon_path = argv[++i];
        else if (a == "--car-width"  && i + 1 < argc) {
            cli_car_width      = std::atof(argv[++i]);
            car_width_from_cli = true;
        }
        else if (a == "--car-length" && i + 1 < argc) {
            cli_car_length      = std::atof(argv[++i]);
            car_length_from_cli = true;
        }
        else if (a == "--car-x"      && i + 1 < argc) {
            cli_car_x      = std::atof(argv[++i]);
            car_x_from_cli = true;
        }
        else if (a == "--car-y"      && i + 1 < argc) {
            cli_car_y      = std::atof(argv[++i]);
            car_y_from_cli = true;
        }
    }

    // Default --lens-dir to config_path's own directory (falls back to "."
    // if config_path has no '/') -- only when --lens-dir wasn't passed.
    if (lens_dir.empty()) {
        size_t slash = config_path.find_last_of('/');
        lens_dir = (slash == std::string::npos) ? "." : config_path.substr(0, slash);
    }

    // cfg_slots[i] is the bev_config.ini slot that sources[i] reads its
    // pose/hb2i from. Plain --src keeps the old identity mapping (position
    // in argv == config slot); --forward-*/--backward-* pin fixed slots
    // (0/1/2/3) instead so a partial (e.g. backward-only) run still lands
    // on the right slots rather than sliding down to 0/1.
    bool any_named = fwd_left || fwd_right || bwd_left || bwd_right;
    if (any_named && !sources.empty()) {
        std::cerr << "[multi] --forward-*/--backward-* can't be combined with --src\n";
        return 1;
    }
    std::vector<int> cfg_slots;
    if (any_named) {
        if ((fwd_left != nullptr) != (fwd_right != nullptr)) {
            std::cerr << "[multi] --forward-left and --forward-right must be given together\n";
            return 1;
        }
        if ((bwd_left != nullptr) != (bwd_right != nullptr)) {
            std::cerr << "[multi] --backward-left and --backward-right must be given together\n";
            return 1;
        }
        if (fwd_left)  { sources.push_back(fwd_left);  cfg_slots.push_back(0); }
        if (fwd_right) { sources.push_back(fwd_right); cfg_slots.push_back(1); }
        if (bwd_left)  { sources.push_back(bwd_left);  cfg_slots.push_back(2); }
        if (bwd_right) { sources.push_back(bwd_right); cfg_slots.push_back(3); }
    } else {
        cfg_slots.resize(sources.size());
        for (size_t i = 0; i < sources.size(); ++i) cfg_slots[i] = (int)i;
    }
    {
        int max_cams = (blend_mode == BlendMode::Pyramid)
                       ? GpuRenderer::kPyramidMaxCameras : GpuRenderer::kMaxCameras;
        if ((int)sources.size() > max_cams) {
            std::cerr << "[multi] " << sources.size() << " source(s) requested, "
                         "max is " << max_cams << "\n";
            return 1;
        }
    }

    // Declared before everything else so it's the LAST thing torn down at
    // function exit (reverse construction order) — GpuRenderer's GL objects
    // (destroyed via renderer.cleanup()/~GpuRenderer, further down in each
    // code path) must go away while this still owns a current EGL context.
#ifdef HAVE_WAYLAND_PREVIEW
    std::unique_ptr<WaylandPreviewWindow> preview_window;
    if (preview_requested) {
        preview_window = std::make_unique<WaylandPreviewWindow>();
        if (!preview_window->init(1280, 720)) {
            std::cerr << "[preview] failed to initialize Wayland preview window\n";
            return 1;
        }
    }
    PreviewWindowPtr preview_ptr = preview_window.get();
#else
    if (preview_requested)
        std::cerr << "[preview] --preview requested but this build has no "
                     "Wayland preview support (ENABLE_WAYLAND_PREVIEW was off "
                     "or its dependencies were missing at configure time)\n";
    PreviewWindowPtr preview_ptr = nullptr;
#endif
    const bool using_preview = preview_ptr != nullptr;

    /* ---- EGL context: windowed (preview) or headless GBM (default) ---- */
    EGLState egl;  // stays zero-initialized when using_preview — every
                   // teardown_egl(egl) call below is then a harmless no-op,
                   // since preview_window owns and tears down the real one.
    if (!using_preview) {
        if (!setup_egl(egl)) {
            teardown_egl(egl); return 1;
        }
    }
#ifdef HAVE_WAYLAND_PREVIEW
    const EGLState &active_egl = using_preview ? preview_window->egl_state() : egl;
#else
    const EGLState &active_egl = egl;
#endif
    if (!check_extensions(active_egl)) {
        std::cerr << "[egl] required extensions missing\n";
        teardown_egl(egl); return 1;
    }
    std::cout << "[egl] context ready\n\n";

    if (!sources.empty()) {
        int rc = run_multi_file_mode(sources, cfg_slots, active_egl, preview_ptr, ipm.px_per_m,
                                     px_per_m_from_cli, blend_mode, config_path,
                                     car_icon_path,
                                     cli_car_width, car_width_from_cli,
                                     cli_car_length, car_length_from_cli,
                                     cli_car_x, car_x_from_cli,
                                     cli_car_y, car_y_from_cli,
                                     lens_dir);
        teardown_egl(egl);
        return rc;
    }
    if (file_left && file_right) {
        int rc = run_dual_file_mode(file_left, file_right, active_egl, preview_ptr, ipm);
        teardown_egl(egl);
        return rc;
    }
    if (file_path) {
        int rc = run_file_mode(file_path, active_egl, preview_ptr, ipm, bev_requested);
        teardown_egl(egl);
        return rc;
    }

    /* ---- Camera init ---- */
    // Same mechanism rpicam-still/rpicam-vid's --tuning-file uses: libcamera's
    // RPi pipeline handler reads this env var once, when it starts up, to
    // load a specific IPA tuning JSON instead of its normal sensor-name-based
    // default lookup. Must be set before CameraManager exists -- setting it
    // any later has no effect, the tuning file is only read at startup.
    if (!tuning_file.empty())
        setenv("LIBCAMERA_RPI_TUNING_FILE", tuning_file.c_str(), 1);

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
    if (!renderer.init(active_egl, sc, alloc.buffers(stream))) {
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

#ifdef HAVE_WAYLAND_PREVIEW
        // No extra pacing needed here — the camera already paces delivery.
        if (preview_ptr) {
            preview_ptr->pump_events();
            preview_ptr->present(renderer.fbo_texture(), renderer.width(), renderer.height());
            if (!preview_ptr->running()) g_running = 0;
        }
#endif

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
