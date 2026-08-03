#pragma once

// Inverse Perspective Mapping (IPM) math for the dual-camera bird's-eye-view
// (BEV) stitch. Pure CPU, no OpenCV — runs once at startup / on parameter
// change, never per-frame. See ipm.cpp for the derivation notes.

struct vec3 { double x = 0, y = 0, z = 0; };

// Column-major 3x3 matrix — layout matches GLSL mat3 / glUniformMatrix3fv
// with transpose=GL_FALSE, so to_floats() can be uploaded directly.
struct mat3 {
    double m[9] = {1,0,0, 0,1,0, 0,0,1};

    double &at(int row, int col)       { return m[col * 3 + row]; }
    double  at(int row, int col) const { return m[col * 3 + row]; }

    static mat3 identity() { return mat3{}; }

    mat3 operator*(const mat3 &o) const;
    vec3 operator*(const vec3 &v) const;

    void to_floats(float out[9]) const {
        for (int i = 0; i < 9; ++i) out[i] = (float)m[i];
    }
};

// Column-major 4x4 matrix — holds the rigid extrinsic transform [R|t; 0 0 0 1].
struct mat4 {
    double m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};

    double &at(int row, int col)       { return m[col * 4 + row]; }
    double  at(int row, int col) const { return m[col * 4 + row]; }

    static mat4 identity() { return mat4{}; }
    static mat4 from_R_t(const mat3 &R, const vec3 &t);

    mat4 operator*(const mat4 &o) const;
};

// IMX219 sensor intrinsics, scaled from the reference 3280 px calibration
// width. cx/cy are the principal point in pixels (width/2, height/2).
mat3 build_intrinsics(int width, int height);

// Builds the direct BEV-output-pixel -> camera-image-pixel homography for one
// camera: given p = (px, py, 1) on the BEV canvas, s = H * p, and the camera
// pixel to sample is (s.x/s.z, s.y/s.z), already normalized to [0,1].
//
// cam_x, cam_y, h : camera position in the world/ground frame, meters.
//                   World frame: X right, Y forward, Z up, ground = Z=0.
// pitch_deg       : tilt from horizontal; negative = looking down.
// yaw_deg         : rotation about the vertical (Z) axis.
// K               : intrinsics from build_intrinsics(img_w, img_h).
// img_w, img_h    : camera image size in pixels (passed explicitly — not
//                   recovered from K — since the caller already has it from
//                   the decoded frame).
// px_per_m        : BEV ground-sampling density (pixels per meter).
// bev_w, bev_h    : BEV output canvas size, pixels.
mat3 ground_to_image_H(double cam_x, double cam_y, double h,
                       double pitch_deg, double yaw_deg,
                       const mat3 &K,
                       int img_w, int img_h,
                       double px_per_m, int bev_w, int bev_h);

// Startup sanity check: projects the BEV canvas center through H and prints
// where it lands in the camera image. Run once per camera at startup so a
// sign/axis mistake shows up immediately as "OUTSIDE image" instead of a
// silent black/garbage render.
void ipm_debug_check(const mat3 &H, int bev_w, int bev_h, const char *label);
