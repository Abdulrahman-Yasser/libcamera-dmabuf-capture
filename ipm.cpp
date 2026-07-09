#include "ipm.h"

#include <cmath>
#include <cstdio>

// ── mat3 / mat4 ───────────────────────────────────────────────────────────────

mat3 mat3::operator*(const mat3 &o) const
{
    mat3 out;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += at(r, k) * o.at(k, c);
            out.at(r, c) = v;
        }
    return out;
}

vec3 mat3::operator*(const vec3 &v) const
{
    return {
        at(0,0)*v.x + at(0,1)*v.y + at(0,2)*v.z,
        at(1,0)*v.x + at(1,1)*v.y + at(1,2)*v.z,
        at(2,0)*v.x + at(2,1)*v.y + at(2,2)*v.z,
    };
}

mat4 mat4::from_R_t(const mat3 &R, const vec3 &t)
{
    mat4 out = mat4::identity();
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c)
            out.at(r, c) = R.at(r, c);
    out.at(0, 3) = t.x;
    out.at(1, 3) = t.y;
    out.at(2, 3) = t.z;
    return out; // row 3 stays [0,0,0,1] from identity()
}

mat4 mat4::operator*(const mat4 &o) const
{
    mat4 out;
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += at(r, k) * o.at(k, c);
            out.at(r, c) = v;
        }
    return out;
}

// ── Intrinsics ────────────────────────────────────────────────────────────────

mat3 build_intrinsics(int width, int height)
{
    double fx = 2714.0 * (double(width) / 3280.0);
    double fy = fx;
    double cx = width  / 2.0;
    double cy = height / 2.0;

    mat3 K;
    K.at(0,0) = fx;  K.at(0,1) = 0.0; K.at(0,2) = cx;
    K.at(1,0) = 0.0; K.at(1,1) = fy;  K.at(1,2) = cy;
    K.at(2,0) = 0.0; K.at(2,1) = 0.0; K.at(2,2) = 1.0;
    return K;
}

// ── Rotation ──────────────────────────────────────────────────────────────────
//
// World frame: X right, Y forward, Z up (ground = Z=0).
// Camera frame (OpenCV convention): X right, Y down, Z forward (into scene).
//
// Built as yaw (about world Z) then pitch (about the yawed right axis), by
// tracking where the camera's right/forward/up axes end up in world
// coordinates, then reading off R_world->cam as the transpose of
// [right | up | forward] (valid since it's a pure rotation matrix).
static mat3 build_rotation(double pitch_deg, double yaw_deg)
{
    const double deg2rad = M_PI / 180.0;
    double yaw   = yaw_deg   * deg2rad;
    double pitch = pitch_deg * deg2rad;

    // yaw=pitch=0 camera: forward=+Y, right=+X, up=+Z (world-aligned).
    // Yaw about world Z rotates {right, forward} within the horizontal plane.
    double cy = std::cos(yaw), sy = std::sin(yaw);
    vec3 right1 = { cy, sy, 0.0 };
    vec3 fwd1   = { -sy, cy, 0.0 };
    vec3 up0    = { 0.0, 0.0, 1.0 };

    // Pitch about the yawed right axis mixes {forward, up}.
    // pitch < 0 tilts forward toward -up (down), matching "negative = looking down".
    double cp = std::cos(pitch), sp = std::sin(pitch);
    vec3 fwd2 = { fwd1.x*cp + up0.x*sp, fwd1.y*cp + up0.y*sp, fwd1.z*cp + up0.z*sp };
    vec3 up2  = { -fwd1.x*sp + up0.x*cp, -fwd1.y*sp + up0.y*cp, -fwd1.z*sp + up0.z*cp };
    vec3 right2 = right1;

    // Camera axes expressed in world coordinates.
    vec3 xcam = right2;
    vec3 ycam = { -up2.x, -up2.y, -up2.z }; // camera "down" = -world-up
    vec3 zcam = fwd2;

    // R_cam->world columns are {xcam, ycam, zcam}; R_world->cam is its
    // transpose, i.e. rows {xcam, ycam, zcam}.
    mat3 R;
    R.at(0,0)=xcam.x; R.at(0,1)=xcam.y; R.at(0,2)=xcam.z;
    R.at(1,0)=ycam.x; R.at(1,1)=ycam.y; R.at(1,2)=ycam.z;
    R.at(2,0)=zcam.x; R.at(2,1)=zcam.y; R.at(2,2)=zcam.z;
    return R;
}

// ── Ground -> image homography ────────────────────────────────────────────────

mat3 ground_to_image_H(double cam_x, double cam_y, double h,
                       double pitch_deg, double yaw_deg,
                       const mat3 &K,
                       int img_w, int img_h,
                       double px_per_m, int bev_w, int bev_h)
{
    mat3 R = build_rotation(pitch_deg, yaw_deg);
    vec3 C = { cam_x, cam_y, h };
    vec3 RC = R * C;
    vec3 t  = { -RC.x, -RC.y, -RC.z }; // t = -R*C, standard extrinsic translation

    mat4 E = mat4::from_R_t(R, t); // [R|t; 0 0 0 1]

    // P = K * [R|t]  (3x4): rows 0..2, cols 0..3 of K * E.
    double P[3][4];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) {
            double v = 0;
            for (int k = 0; k < 3; ++k) v += K.at(r, k) * E.at(k, c);
            P[r][c] = v;
        }

    // M (4x3): BEV output pixel (px, py, 1) -> ground point (X, Y, 0, 1), meters.
    double r_ = px_per_m;
    double M[4][3] = {
        { 1.0/r_,  0.0,     -(bev_w/2.0)/r_ },
        { 0.0,    -1.0/r_,   (bev_h/2.0)/r_ },
        { 0.0,     0.0,      0.0            },
        { 0.0,     0.0,      1.0            },
    };

    // H = P * M  (3x3): direct BEV-pixel -> camera-image-pixel mapping.
    // (Not inverted — see ipm.h. M*p already yields the ground point that p
    // represents, and P already maps that ground point to the camera image,
    // so P*M composed IS the BEV->image map the shader needs for its
    // per-fragment backward sampling.)
    mat3 H;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) {
            double v = 0;
            for (int k = 0; k < 4; ++k) v += P[r][k] * M[k][c];
            H.at(r, c) = v;
        }

    // P was built from pixel-space K, so H.xy come out in camera pixel units.
    // Scale rows 0/1 so the shader's perspective-divided (u,v) land in [0,1]
    // directly, matching the validity check and texture() sampling it does.
    for (int c = 0; c < 3; ++c) {
        H.at(0, c) /= img_w;
        H.at(1, c) /= img_h;
    }

    return H;
}

void ipm_debug_check(const mat3 &H, int bev_w, int bev_h, const char *label)
{
    vec3 p = { bev_w / 2.0, bev_h / 2.0, 1.0 };
    vec3 s = H * p;
    double u = s.z != 0.0 ? s.x / s.z : 0.0;
    double v = s.z != 0.0 ? s.y / s.z : 0.0;
    bool inside = s.z > 0.0 && u >= 0.0 && u <= 1.0 && v >= 0.0 && v <= 1.0;
    std::printf("[ipm] %-6s: BEV center -> image uv = (%.3f, %.3f), z=%.3f  %s\n",
                label, u, v, s.z, inside ? "(inside image)" : "(OUTSIDE image!)");
}

#ifdef IPM_STANDALONE_TEST
// Pure-CPU smoke test, no GL/libcamera dependency:
//   g++ -std=c++17 -DIPM_STANDALONE_TEST ipm.cpp -o /tmp/ipm_test -lm && /tmp/ipm_test
int main()
{
    int w = 1640, h = 1232;
    mat3 K = build_intrinsics(w, h);

    double baseline = 0.40, cam_y = 0.0, cam_h = 1.2, pitch = -30.0, yaw = 0.0, ppm = 100.0;
    int bev_w = w, bev_h = h;

    mat3 HL = ground_to_image_H(-baseline/2, cam_y, cam_h, pitch, yaw, K, w, h, ppm, bev_w, bev_h);
    mat3 HR = ground_to_image_H( baseline/2, cam_y, cam_h, pitch, yaw, K, w, h, ppm, bev_w, bev_h);

    ipm_debug_check(HL, bev_w, bev_h, "left");
    ipm_debug_check(HR, bev_w, bev_h, "right");

    // Sweep a column of BEV pixels straight up the vertical centerline and
    // print where each lands in the left camera image, to eyeball that
    // "far" (top of BEV canvas) maps to a sensible v and doesn't invert.
    std::printf("\n[ipm] left camera, vertical centerline sweep (px=%d):\n", bev_w/2);
    for (int py_top_origin = 0; py_top_origin <= bev_h; py_top_origin += bev_h/8) {
        vec3 p = { bev_w/2.0, (double)py_top_origin, 1.0 };
        vec3 s = HL * p;
        double u = s.z != 0 ? s.x/s.z : 0, v = s.z != 0 ? s.y/s.z : 0;
        bool inside = s.z > 0 && u>=0 && u<=1 && v>=0 && v<=1;
        std::printf("  py(top-origin)=%4d  -> uv=(%.3f, %.3f) z=%.3f %s\n",
                    py_top_origin, u, v, s.z, inside ? "" : "(outside)");
    }
    return 0;
}
#endif
