#pragma once

// Per-slot BEV geometry helpers shared by main.cpp's --src mode and the
// ivi-homescreen platform view (ihs/bev_pipeline.cpp), so both build their
// homographies and lens lookups from one copy.

#include "ipm.h" // mat3

#include <string>

// Resolution the ChArUco board->image homographies (kHb2i_A/kHb2i_B in
// bev_config.cpp's bev_config_defaults()) were actually measured at --
// fixed, independent of img_w/img_h below (the runtime video's own frame
// size), which can be a different resolution entirely. Hb2i's raw output is
// in calibration-image pixels, so normalizing by anything other than the
// resolution it was measured at would silently rescale the homography.
inline constexpr double CAL_W = 3280.0, CAL_H = 2464.0;

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
// are intentionally inert for calibrated slots (see main.cpp's H/h/P/p
// keyboard handlers' warning).
mat3 measured_H(const mat3 &Hb2i, int img_w, int img_h,
                double px_per_m, int bev_w, int bev_h,
                double dx = 0.0, double dy = 0.0, double dyaw_deg = 0.0);

// Naming-convention lookup for a per-lens-model distortion file:
// "<lens_dir>/<lens_model>-lens.ini". lens_model is used verbatim -- no
// hyphenation normalization -- so "imx219" and "imx-219" are two different
// files, consistently with whatever the user actually typed/saved.
std::string lens_calib_path(const std::string &lens_dir, const std::string &lens_model);
