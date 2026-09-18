#pragma once

// A platform view's creation params: UTF-8 "key=value" lines, one per line,
// '#' comments and blank lines ignored. The CLI flags map onto them one to one
// (--forward-left -> forward_left=...), so a working command line is a working
// view. `src` may repeat. Paths prefixed "asset:" resolve against the running
// bundle's flutter_assets directory.
//
//   mode        camera | file | surround | pattern   (default: surround when a
//               source is given, file when `file` is, else camera)
//   fit         contain | cover | fill               (default contain)
//   ring_size   2..6 dma-bufs per view               (default 4)
//   scanout     1 = take a KMS plane when the shell offers one (EGL DRM only)
//
//   camera:   camera_index, width, height, tuning_file
//   file:     file, bev, cam_h, pitch, yaw, cam_y, px_per_m
//   surround: src (repeatable) | forward_left, forward_right, backward_left,
//             backward_right; config, lens_dir, blend (feather | pyramid |
//             coverage), px_per_m, car_icon, car_width, car_length, car_x, car_y
//   pattern:  width, height, pattern_fps -- a synthetic moving test image, for
//             bringing a shell up without a camera or a recording. 0 fps runs
//             it flat out, which is how you measure what the view can actually
//             carry on a board.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct BevParams {
    enum class Mode { Camera, File, Surround, Pattern };
    enum class Fit  { Contain, Cover, Fill };

    Mode mode      = Mode::Camera;
    Fit  fit       = Fit::Contain;
    // Four, not three: a slot is reused only once the compositor releases it,
    // and on a Pi 4 (drm-kms-egl, both the texture-import and the KMS-plane
    // grant) three slots left the producer waiting out the release timeout on
    // ~7% of frames -- 42 timeouts per 600 frames, and 110 per 350 on the
    // plane path, where blocking dropped it from 30 to 14 fps. Each timeout is
    // also a slot redrawn while the display may still be reading it, which is
    // visible tearing. A fourth slot took both to 3 (the first frames, before
    // anything has been released).
    int  ring_size = 4;
    bool scanout   = false;

    // Camera mode (and pattern mode's size).
    int         camera_index = 0;
    int         width = 0, height = 0; // 0 = the mode's default
    std::string tuning_file;

    // Pattern mode's frame rate. The other modes pace to their source (the
    // sensor, or the recording's rate); the pattern has none, so it paces
    // itself. 0 = uncapped.
    double pattern_fps = 30.0;

    // File mode; the IPM pose is shared with --bev.
    std::string file;
    bool        bev   = false;
    double      cam_h = 1.2, pitch = -30.0, yaw = 0.0, cam_y = 0.0;

    // Shared by file and surround modes. Surround mode falls back to the
    // config file's px_per_m unless this was given.
    double px_per_m     = 100.0;
    bool   px_per_m_set = false;

    // Surround mode. cfg_slots[i] is the bev_config.ini slot sources[i] reads
    // its pose/hb2i from -- identity for `src`, fixed 0..3 for the named keys.
    std::vector<std::string> sources;
    std::vector<int>         cfg_slots;
    std::string              config_path = "bev_config.ini";
    std::string              lens_dir;   // empty = config_path's directory
    std::string              blend       = "feather";
    std::string              car_icon;
    double car_width  = 1.8, car_length = 4.5, car_x = 0.0, car_y = 0.0;
    bool   car_width_set = false, car_length_set = false;
    bool   car_x_set     = false, car_y_set      = false;
};

// Parses @size bytes of creation params into @out. Returns false with a
// description in @error for anything that cannot be used (an unknown mode, a
// malformed number, --src mixed with the named sources); unknown keys are
// reported on stderr and skipped. @assets_dir may be empty.
bool bev_params_parse(const uint8_t *data, size_t size, const std::string &assets_dir,
                      BevParams &out, std::string &error);
