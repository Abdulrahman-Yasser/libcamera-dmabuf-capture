#pragma once

#include "ipm.h" // mat3

#include <string>

// On-disk defaults for the --src multi-camera BEV path: per-slot starting
// pose (mirrors main.cpp's SlotParams) plus an optional measured ChArUco
// board->image homography for that slot, and the global blend/scale
// tunables. Lets camera-rig geometry and calibration survive across runs
// (and rebuilds) without editing source, and be overwritten from live-tuned
// values at runtime via bev_config_save().
struct BevSlotConfig {
    double cam_x = 0.0, cam_y = 0.0, cam_h = 1.2, pitch = -30.0, yaw = 0.0;
    // Canvas coverage-wedge center, degrees (world atan2(Y,X) bearing: front
    // (+Y) = 90, right (+X) = 0, back (-Y) = 270, left (-X) = 180 -- same
    // convention as main.cpp's facing_deg_from_yaw()). Deliberately separate
    // from `yaw`: yaw is fine live-tunable content reprojection (a few
    // degrees, e.g. "this front camera is nudged slightly left"), while
    // facing_deg is which broad canvas sector the camera is assigned to for
    // blending -- two cameras that are both basically front-facing should
    // both get facing_deg=90 even if their yaws differ. Parsed from either a
    // keyword (front/back/left/right) or a raw degree number -- see
    // bev_config_load().
    double facing_deg = 90.0; // default: front
    bool   has_hb2i = false;
    mat3   hb2i; // board-metre -> image-pixel; valid only if has_hb2i

    // Live-tuned (X/x, G/g, Y/y) offset from cam_x/cam_y/yaw above, for a
    // has_hb2i slot -- see main.cpp's measured_H() comment. cam_x/cam_y/yaw
    // stay the fixed pose the ChArUco shot was taken at (never overwritten by
    // 'W' save) so the delta these three store is exactly the correction
    // main.cpp needs to reproduce; if 'W' instead overwrote cam_x/cam_y/yaw
    // directly, the delta would recompute to 0 on the next load and silently
    // discard whatever offset had been tuned in, even though the raw numbers
    // still landed correctly in the file.
    double cam_x_delta = 0.0, cam_y_delta = 0.0, yaw_delta = 0.0;

    // Which lens this slot uses, looked up by naming convention as
    // "<lens_dir>/<lens_model>-lens.ini" (see lens_calib_load() and
    // main.cpp's lens_calib_path()). Empty = no lens configured, the
    // default/harmless case (no distortion correction applied). This is
    // the only lens-related field persisted by bev_config_load()/_save() --
    // fx/fy/../k1../lens_calib_w/h below are re-read from that file every
    // startup and every runtime hot-swap ('L' key), so the lens file stays
    // the single source of truth and can be recalibrated without
    // bev_config.ini silently going stale.
    std::string lens_model;

    // Populated by a successful lens_calib_load() lookup; meaningless unless
    // has_distortion is true. fx/fy/cx/cy are in pixel units at
    // lens_calib_w x lens_calib_h (the resolution they were measured at --
    // main.cpp rescales+normalizes them for whatever resolution the actual
    // video turns out to be, same idea as measured_H()'s CAL_W/CAL_H
    // rescale for hb2i). k1/k2/k3 (radial) and p1/p2 (tangential) follow the
    // standard OpenCV distortion-model convention, matching
    // scripts/calibrate_intrinsics.py's own dist-coefficient ordering.
    double fx = 0.0, fy = 0.0, cx = 0.0, cy = 0.0;
    double k1 = 0.0, k2 = 0.0, k3 = 0.0, p1 = 0.0, p2 = 0.0;
    int    lens_calib_w = 0, lens_calib_h = 0;
    bool   has_distortion = false;
};

struct BevConfig {
    static constexpr int kMaxSlots = 4;
    // Ground-sampling density: canvas width in px / px_per_m = real-world
    // meters spanned by the BEV output. Needs rechecking any time the
    // Hb2i matrices or CAL_W/CAL_H (main.cpp) change -- the old 4000, tuned
    // for a previous calibration, made each camera sample only a sliver of
    // its own frame (camA u=[0.71,1.0], camB u=[0.0,0.41]) stretched across
    // most of the canvas once the new Hb2i/CAL_W/CAL_H took over (the
    // "both videos on the right" symptom). 2200 was confirmed (coverage-map
    // scan of the current kHb2i_A/kHb2i_B at 1640x1232) as the smallest
    // value giving 100% canvas coverage with no black gaps, while each
    // camera still uses roughly half its own frame (camA u=[0.51,1.0],
    // camB u=[0.0,0.61]) instead of a degenerate sliver -- rescale (canvas
    // size scales with your source video's resolution) if that changes.
    double        px_per_m    = 2200.0;
    // Angular half-width (degrees) of each camera's canvas coverage wedge,
    // centered on that slot's facing_deg (see BevSlotConfig::facing_deg).
    // Outside every camera's wedge, nothing is drawn there regardless of
    // whether some camera's homography would otherwise validly map that
    // point -- so this needs to be wide enough that the wedges' union
    // covers the whole canvas, or you get permanently black gaps between
    // cameras that aren't ~180/N degrees apart.
    //
    // The weight taper (kFS_MULTI/kFS_PYR_WARP: raw = 1 - |d|/overlap_deg)
    // reaches zero exactly at overlap_deg away, as the "half-width" name
    // promises -- but it's *linear* the whole way there, so it's already
    // near-invisible well before that edge. 175 was chosen to read as "the
    // whole canvas, let each camera's own homography validity be the real
    // limiter" but a linear taper to zero at 175 doesn't deliver that in
    // practice -- confirmed on a real 2-camera (both facing_deg=90, toed-in)
    // rig: roughly the near/bottom half of the canvas (where their true
    // overlap is actually widest) rendered solid black. 350 (double) pushes
    // the same taper's near-zero point out past 180 in both directions, so
    // it only meaningfully bites within a few degrees of directly opposite
    // facing_deg -- narrow it back down only if you deliberately want hard
    // sector boundaries between many closely-spaced cameras.
    float         overlap_deg = 350.0f;
    float         blend_edge  = 0.45f;
    BevSlotConfig slots[kMaxSlots];

    // Car icon overlay (see GpuRenderer::init_car_icon()/draw_car_icon()) --
    // world-meter position offset from the BEV canvas center (the vehicle's
    // own world origin, BEV_ALGORITHM.md §3) plus its real width/length in
    // meters. car_x/car_y stay 0 for a rig whose calibrated center already
    // matches the canvas center; nonzero nudges the icon to correct for a
    // mismatch instead of touching the geometry itself. --car-x/--car-y/
    // --car-width/--car-length on the command line override these.
    double car_x      = 0.0;
    double car_y      = 0.0;
    double car_width  = 1.8;
    double car_length = 4.5;
};

// Built-in fallback: the original hand-tuned front/right/back/left starting
// directions, plus the two ChArUco-measured homographies for slots 0/1.
BevConfig bev_config_defaults();

// Loads path into out, starting from bev_config_defaults() and overwriting
// only the fields the file specifies. A missing file is not an error --
// out is left as bev_config_defaults() and this still returns true.
// Returns false only on a malformed file (bad number, wrong hb2i count,
// unknown key), with a message identifying the offending line on stderr.
bool bev_config_load(const std::string &path, BevConfig &out);

// Overwrites path with cfg, in the same format bev_config_load() reads.
bool bev_config_save(const std::string &path, const BevConfig &cfg);

// Parses a flat key=value lens/intrinsics file (fx, fy, cx, cy, calib_w,
// calib_h required; k1, k2, k3, p1, p2 optional, default 0.0 -- a
// --zero-tangent-dist or non-rational-model calibration legitimately has
// some of these at zero). On success, sets slot's fx/fy/cx/cy/k1../
// lens_calib_w/h and has_distortion=true; slot is left untouched otherwise.
// Returns false, with no message printed, if path doesn't exist (the
// expected/common state before a lens is calibrated -- the caller decides
// what if anything to print) or with a path:lineno stderr message (same
// style as bev_config_load()'s) if the file exists but is malformed
// (missing required key, bad number) -- that case is a real bug worth
// surfacing, unlike a simply-absent file.
bool lens_calib_load(const std::string &path, BevSlotConfig &slot);
