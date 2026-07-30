#include "gpu_renderer.h"
#include "perf_timer.h"

#include <drm/drm_fourcc.h>
#include <png.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

using namespace libcamera;

// ── Shader source ─────────────────────────────────────────────────────────────

// Fragment shader for single system-memory NV12 frame.
static const char kFS_2D[] = R"glsl(
#version 300 es
precision mediump float;
uniform sampler2D uTexY;
uniform sampler2D uTexUV;
in  vec2 vTexCoord;
out vec4 fragColor;
void main() {
    vec2  uv = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    float Y  = texture(uTexY,  uv).r;
    vec2  UV = texture(uTexUV, uv).rg - 0.5;
    float R  = clamp(Y + 1.402  * UV.y,                0.0, 1.0);
    float G  = clamp(Y - 0.344  * UV.x - 0.714 * UV.y, 0.0, 1.0);
    float B  = clamp(Y + 1.772  * UV.x,                0.0, 1.0);
    fragColor = vec4(R, G, B, 1.0);
}
)glsl";

// IPM bird's-eye-view stitch shader.
// Per-fragment backward warp: each fragment IS a point on the flat-ground
// BEV output plane. uH_L/uH_R (built by ground_to_image_H(), see ipm.h) map
// that BEV pixel directly to each camera's normalized image coordinate.
// Smoothstep blend where both cameras cover the same ground point.
static const char kFS_DUAL[] = R"glsl(
#version 300 es
precision mediump float;
uniform sampler2D uTexY0;
uniform sampler2D uTexUV0;
uniform sampler2D uTexY1;
uniform sampler2D uTexUV1;

// BEV-pixel -> camera-image homography, column-major, pre-normalized to
// [0,1] (see ground_to_image_H() in ipm.cpp). uv = (H*p).xy / (H*p).z.
uniform mat3 uH_L;
uniform mat3 uH_R;

// BEV canvas size in pixels (matches the FBO/render size; set once at
// init_dual, not runtime-tunable).
uniform float uBevWidth;
uniform float uBevHeight;

// BEV ground-sampling density, pixels per meter. This is already baked into
// uH_L/uH_R; kept as a uniform only so the blend math below (which needs to
// recover world-space X from gl_FragCoord) can stay in sync without a
// separate constant.
uniform float uPxPerM;

// Blend half-width, in meters, of the seam transition centered on the
// baseline midline (world X=0) — the two cameras are mounted symmetrically
// about it, so that's the natural blend center.
uniform float uOverlap;
// Controls sharpness of the crossover within the blend zone.
// 0.0 = full gradual blend, 0.49 = nearly instant cut.
uniform float uBlendEdge;

out vec4 fragColor;

vec3 nv12_to_rgb(sampler2D sY, sampler2D sUV, vec2 uv) {
    float Y  = texture(sY,  uv).r;
    vec2  UV = texture(sUV, uv).rg - 0.5;
    return vec3(
        clamp(Y + 1.402  * UV.y,                0.0, 1.0),
        clamp(Y - 0.344  * UV.x - 0.714 * UV.y, 0.0, 1.0),
        clamp(Y + 1.772  * UV.x,                0.0, 1.0));
}

void main() {
    // This fragment IS a point on the BEV output plane. gl_FragCoord.y is
    // GL's bottom-left-origin window coordinate, but ground_to_image_H()'s
    // M matrix was built assuming top-origin "py" (py=0 = far/top row, like
    // a normal image) — flip it here to match.
    vec3 p = vec3(gl_FragCoord.x, uBevHeight - gl_FragCoord.y, 1.0);

    // Project into each camera.
    vec3 sL = uH_L * p;
    vec3 sR = uH_R * p;
    vec2 uvL = sL.xy / sL.z;      // perspective divide
    vec2 uvR = sR.xy / sR.z;

    // Validity: inside [0,1] AND in front of the camera (z acts as depth).
    bool okL = sL.z > 0.0 && all(greaterThanEqual(uvL, vec2(0.0)))
                          && all(lessThanEqual(uvL, vec2(1.0)));
    bool okR = sR.z > 0.0 && all(greaterThanEqual(uvR, vec2(0.0)))
                          && all(lessThanEqual(uvR, vec2(1.0)));

    // No flip here, unlike kFS/kFS_2D's screen-position sampling: uvL/uvR
    // come from the homography (ground_to_image_H()'s standard image-space
    // math, v increasing downward), which already lands on uv.y=0 = image
    // top -- the same row glTexImage2D's first uploaded row placed at GL's
    // V=0. kFS_2D's "1.0-v" flip corrects a *different* mismatch (screen-
    // space NDC vTexCoord vs. texture row order); it doesn't apply here.
    vec3 cL = nv12_to_rgb(uTexY0, uTexUV0, uvL);
    vec3 cR = nv12_to_rgb(uTexY1, uTexUV1, uvR);

    vec3 rgb;
    if (okL && okR) {
        // Overlap: blend across the ground-plane X coordinate of this
        // fragment, since the cameras are mounted symmetrically about X=0.
        float worldX = (gl_FragCoord.x - uBevWidth * 0.5) / uPxPerM;
        float t = clamp((worldX + uOverlap) / (2.0 * uOverlap), 0.0, 1.0);
        float s = smoothstep(uBlendEdge, 1.0 - uBlendEdge, t);
        rgb = mix(cL, cR, s);
    } else if (okL) {
        rgb = cL;
    } else if (okR) {
        rgb = cR;
    } else {
        rgb = vec3(0.0); // no camera covers this ground point
    }

    fragColor = vec4(rgb, 1.0);
}
)glsl";

// Single-camera forward BEV shader — the same backward-warp idea as
// kFS_DUAL's per-camera projection above, just one camera and no blend.
// First step toward the eventual front/back/left/right 4-camera BEV blend;
// kept separate from kFS_DUAL rather than adding a "camera count" branch to
// it, so composing the 4-camera version later means adding more of these,
// not restructuring this one.
static const char kFS_BEV[] = R"glsl(
#version 300 es
precision mediump float;
uniform sampler2D uTexY;
uniform sampler2D uTexUV;

// BEV-pixel -> camera-image homography, column-major, pre-normalized to
// [0,1] (see ground_to_image_H() in ipm.cpp). uv = (H*p).xy / (H*p).z.
uniform mat3 uH;

// BEV canvas size in pixels (matches the FBO/render size; set once at
// init_bev, not runtime-tunable) — needed for the same gl_FragCoord.y flip
// kFS_DUAL uses (ground_to_image_H()'s M matrix assumes top-origin "py",
// GLES's gl_FragCoord.y is bottom-origin).
uniform float uBevWidth;
uniform float uBevHeight;

out vec4 fragColor;

void main() {
    vec3 p = vec3(gl_FragCoord.x, uBevHeight - gl_FragCoord.y, 1.0);
    vec3 s = uH * p;
    vec2 uv = s.xy / s.z;   // perspective divide

    // Validity: inside [0,1] AND in front of the camera (z acts as depth).
    bool ok = s.z > 0.0 && all(greaterThanEqual(uv, vec2(0.0)))
                        && all(lessThanEqual(uv, vec2(1.0)));
    if (!ok) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0); // no ground coverage here
        return;
    }

    // No flip here, unlike kFS/kFS_2D's screen-position sampling: `uv` comes
    // from ground_to_image_H(), which already uses standard image-space
    // math (cy = height/2, v increasing downward) -- the same row-order
    // convention the decoded NV12 buffer (and therefore this texture's v)
    // already has: glTexImage2D's first uploaded row (the image top) lands
    // at GL's V=0, and the homography's uv.y=0 means image top too, so they
    // already agree. kFS_2D's "1.0-v" flip corrects a *different* mismatch
    // (screen-space NDC vTexCoord vs. texture row order); it doesn't apply
    // to a homography-derived uv, which already matches the texture directly.
    float Y  = texture(uTexY,  uv).r;
    vec2  UV = texture(uTexUV, uv).rg - 0.5;
    float R  = clamp(Y + 1.402  * UV.y,                0.0, 1.0);
    float G  = clamp(Y - 0.344  * UV.x - 0.714 * UV.y, 0.0, 1.0);
    float B  = clamp(Y + 1.772  * UV.x,                0.0, 1.0);
    fragColor = vec4(R, G, B, 1.0);
}
)glsl";

// N-camera surround-view BEV shader (--src mode). Same per-fragment
// backward-warp idea as kFS_DUAL/kFS_BEV above, generalized two ways:
//
//   1. Up to MAX_CAMERAS independently-posed cameras instead of exactly 2 —
//      each camera gets its own homography uH[i] (built the same way as
//      uH_L/uH_R, one ground_to_image_H() call per camera) and its own
//      "facing bearing" uFacingDeg[i], derived from that camera's yaw (see
//      BEV_ALGORITHM.md's "yaw -> facing" formula). uNumCameras is a
//      *runtime* uniform used only as a continue-guard inside a loop whose
//      *bound* is the compile-time MAX_CAMERAS constant — GLSL ES 3.00 does
//      not reliably support genuinely dynamic (data-dependent) indexing of a
//      sampler array, but a for-loop with a constant bound is the
//      spec-legal "constant-index-expression" form, so uTexY[i]/uTexUV[i]/
//      uH[i] stay portable even though i is a variable.
//   2. kFS_DUAL's single-axis (world X=0) 2-camera blend doesn't generalize
//      to cameras arranged around a vehicle (multiple corner overlaps, not
//      one shared seam) — replaced with a weighted angular blend: each
//      valid camera contributes color weighted by how close this fragment's
//      bearing (relative to the BEV canvas center) is to that camera's own
//      facing bearing, normalized by the sum of weights. This handles any
//      number of simultaneously-overlapping cameras (e.g. 3+ near a corner)
//      without pairwise seam special-casing. uOverlap is reinterpreted as an
//      angular half-width in *degrees* (was meters in kFS_DUAL); uBlendEdge
//      keeps its exact existing meaning/formula.
static const char kFS_MULTI[] = R"glsl(
#version 300 es
precision mediump float;

#define MAX_CAMERAS 8

uniform sampler2D uTexY[MAX_CAMERAS];
uniform sampler2D uTexUV[MAX_CAMERAS];
// BEV-pixel -> camera-image homographies, one per camera (see
// ground_to_image_H() in ipm.cpp), column-major, pre-normalized to [0,1].
uniform mat3  uH[MAX_CAMERAS];
// Each camera's facing bearing, degrees, world atan2(Y,X) convention
// (derived from that camera's yaw — see BEV_ALGORITHM.md).
uniform float uFacingDeg[MAX_CAMERAS];
uniform int   uNumCameras;

// BEV canvas size in pixels (matches the FBO/render size; set once at
// init_multi, not runtime-tunable).
uniform float uBevWidth;
uniform float uBevHeight;
// BEV ground-sampling density, pixels per meter — shared/global (one output
// canvas), already baked into uH[]; kept as a uniform so the blend math
// below (which recovers world-space X/Y from gl_FragCoord) stays in sync.
uniform float uPxPerM;

// Angular half-width, degrees, of the blend zone centered on each camera's
// facing bearing. Needs to be at least half the largest gap between
// adjacent camera facings or a black wedge appears between them.
uniform float uOverlap;
// Controls sharpness of the crossover within the blend zone — identical
// meaning/formula to kFS_DUAL's uBlendEdge.
uniform float uBlendEdge;
// Testing/exploration escape hatch: when > 0.5, every camera's blend weight
// is forced to 1 (still gated by the real per-pixel "ok" validity check
// below) instead of being shaped by facing/uOverlap/uBlendEdge at all -- so
// a camera's content is visible everywhere its own homography validly maps
// it, with no angular sector restricting where that's allowed to show up.
// Physically dishonest for a real rig (two forward cameras can't actually
// see behind themselves), but useful for freely sweeping yaw during testing
// without fighting the coverage wedge. Off (0) by default.
uniform float uFreeYaw;

out vec4 fragColor;

const float kPi = 3.14159265358979;

// Mesa's GLSL ES 3.00 compiler (confirmed on the real V3D target) rejects
// ANY variable index into a sampler array — including uTexY[i] inside a
// texture() call where i is a compile-time-bounded loop variable. It does
// not perform loop-unrolling before that check, so the "canonical constant-
// bound for-loop" exception the spec allows isn't honored here in practice.
// The portable fix: unroll the loop in the GLSL *source text* itself via
// this macro, so every sampler-array subscript is a literal integer — no
// variable indexing anywhere, satisfying even the strictest compiler.
#define PROCESS_CAMERA(IDX)                                                  \
    if (IDX < uNumCameras) {                                                 \
        vec3 s  = uH[IDX] * p;                                              \
        vec2 uv = s.xy / s.z;                                               \
        bool ok = s.z > 0.0 && all(greaterThanEqual(uv, vec2(0.0)))         \
                            && all(lessThanEqual(uv, vec2(1.0)));           \
        if (ok) {                                                           \
            float w;                                                        \
            if (uFreeYaw > 0.5) {                                           \
                w = 1.0;                                                    \
            } else {                                                        \
                /* Angular distance from this fragment's bearing to camera */\
                /* IDX's facing, wrapped to (-pi, pi]. */                   \
                float d   = mod(thetaFrag - radians(uFacingDeg[IDX]) + kPi, \
                                2.0 * kPi) - kPi;                           \
                float raw = clamp(1.0 - abs(d) / radians(uOverlap), 0.0, 1.0);\
                w = smoothstep(uBlendEdge, 1.0 - uBlendEdge, raw);          \
            }                                                               \
            if (w > 0.0) {                                                  \
                /* NV12->RGB. No v-flip: uv is homography-derived (see    */ \
                /* kFS_BEV's comment) and already matches the texture's   */ \
                /* row order directly.                                   */ \
                float Y  = texture(uTexY[IDX],  uv).r;                     \
                vec2  UV = texture(uTexUV[IDX], uv).rg - 0.5;               \
                vec3  c  = vec3(                                            \
                    clamp(Y + 1.402  * UV.y,                0.0, 1.0),     \
                    clamp(Y - 0.344  * UV.x - 0.714 * UV.y, 0.0, 1.0),     \
                    clamp(Y + 1.772  * UV.x,                0.0, 1.0));    \
                colorSum  += c * w;                                         \
                weightSum += w;                                            \
            }                                                               \
        }                                                                    \
    }

void main() {
    // This fragment IS a point on the BEV output plane (same top-origin
    // flip kFS_DUAL/kFS_BEV use for the homography lookup).
    vec3 p = vec3(gl_FragCoord.x, uBevHeight - gl_FragCoord.y, 1.0);

    // This fragment's bearing relative to the BEV canvas center — used only
    // for blend weighting, not for the per-camera homography lookup above.
    float worldX    = (gl_FragCoord.x - uBevWidth  * 0.5) / uPxPerM;
    float worldY    = (gl_FragCoord.y - uBevHeight * 0.5) / uPxPerM;
    float thetaFrag = atan(worldY, worldX);

    vec3  colorSum  = vec3(0.0);
    float weightSum = 0.0;

    // Manually unrolled (see PROCESS_CAMERA comment above) — one literal
    // invocation per MAX_CAMERAS slot (8); extras beyond uNumCameras are
    // skipped at runtime by the "IDX < uNumCameras" check, but the array
    // index itself stays a compile-time literal in every one. If
    // MAX_CAMERAS / GpuRenderer::kMaxCameras ever changes, this list of
    // literal invocations must be added to or trimmed to match — the GLSL
    // preprocessor can't generate them from MAX_CAMERAS automatically.
    PROCESS_CAMERA(0)
    PROCESS_CAMERA(1)
    PROCESS_CAMERA(2)
    PROCESS_CAMERA(3)
    PROCESS_CAMERA(4)
    PROCESS_CAMERA(5)
    PROCESS_CAMERA(6)
    PROCESS_CAMERA(7)

    fragColor = vec4(weightSum > 0.0 ? colorSum / weightSum : vec3(0.0), 1.0);
}
)glsl";

// ── Multi-band (Laplacian pyramid) blend, N-camera surround-view ───────────────
//
// Second, separate blend option for --src mode, alongside kFS_MULTI's
// single-pass angular-weighted "feathering" blend above. Blends different
// spatial-frequency bands with different-width blend zones instead of one
// fixed width, so it can hide brightness/color seam mismatch feathering
// cannot. Genuinely multi-pass: warp -> Gaussian pyramid -> Laplacian
// pyramid -> per-level cross-camera blend -> reconstruct, five shaders below
// used in that order by GpuRenderer::render_frame_multi_pyramid().
//
// Weight is packed into each pyramid texture's alpha channel alongside
// color (RGBA16F, needed since Laplacian levels are difference images with
// negative values, which GL_RGBA8 cannot store) -- so the Gaussian/Laplacian
// pyramid decomposition of the per-camera blend-weight mask happens
// automatically in lockstep with the color's, exactly matching Burt-Adelson
// mask-pyramid treatment, with no separate mask texture or pass.

// Pass 1/5: per-camera homography warp + angular blend weight, one draw per
// camera (NOT one draw for all N like kFS_MULTI -- see GpuRenderer's
// u_H_pyr_loc_ comment). Because of that, this shader takes a single uH/
// uFacingDeg pair, not an array, so it needs none of kFS_MULTI's manual
// unrolling -- ordinary sampler2D, no sampler array anywhere.
static const char kFS_PYR_WARP[] = R"glsl(
#version 300 es
precision mediump float;

uniform sampler2D uTexY;
uniform sampler2D uTexUV;

// BEV-pixel -> camera-image homography for THIS camera (see
// ground_to_image_H() in ipm.cpp), same convention as kFS_MULTI/kFS_BEV.
uniform mat3  uH;
uniform float uFacingDeg;

uniform float uBevWidth;
uniform float uBevHeight;
uniform float uPxPerM;
uniform float uOverlap;     // angular half-width, degrees (kFS_MULTI's meaning)
uniform float uBlendEdge;
uniform float uFreeYaw;     // same testing escape hatch as kFS_MULTI's uFreeYaw

out vec4 fragColor;

const float kPi = 3.14159265358979;

void main() {
    vec3 p = vec3(gl_FragCoord.x, uBevHeight - gl_FragCoord.y, 1.0);
    vec3 s = uH * p;
    vec2 uv = s.xy / s.z;

    bool ok = s.z > 0.0 && all(greaterThanEqual(uv, vec2(0.0)))
                        && all(lessThanEqual(uv, vec2(1.0)));
    if (!ok) {
        fragColor = vec4(0.0);   // weight 0 -> contributes nothing at any level
        return;
    }

    float w;
    if (uFreeYaw > 0.5) {
        w = 1.0;
    } else {
        float worldX    = (gl_FragCoord.x - uBevWidth  * 0.5) / uPxPerM;
        float worldY    = (gl_FragCoord.y - uBevHeight * 0.5) / uPxPerM;
        float thetaFrag = atan(worldY, worldX);

        float d   = mod(thetaFrag - radians(uFacingDeg) + kPi, 2.0 * kPi) - kPi;
        float raw = clamp(1.0 - abs(d) / radians(uOverlap), 0.0, 1.0);
        w = smoothstep(uBlendEdge, 1.0 - uBlendEdge, raw);
    }

    // No v-flip: uv is homography-derived (see kFS_BEV's comment) and
    // already matches the texture's row order directly.
    float Y  = texture(uTexY,  uv).r;
    vec2  UV = texture(uTexUV, uv).rg - 0.5;
    vec3  c  = vec3(
        clamp(Y + 1.402  * UV.y,                0.0, 1.0),
        clamp(Y - 0.344  * UV.x - 0.714 * UV.y, 0.0, 1.0),
        clamp(Y + 1.772  * UV.x,                0.0, 1.0));

    fragColor = vec4(c, w);
}
)glsl";

// Pass 2/5: build one Gaussian pyramid level from the previous one -- fixed
// 5x5 binomial-approximation blur ([1,4,6,4,1]/16 outer-producted) applied
// to color+weight together (one vec4 fetch per tap), then implicitly
// downsampled by rendering at half the source's viewport size. Single-pass,
// non-separable (10 taps would be cheaper than 25 via two passes, but that
// doubles this stage's draw-call count -- not worth it for a first cut at
// this pass budget; a real profiling target if it's ever too slow).
static const char kFS_PYR_DOWNSAMPLE[] = R"glsl(
#version 300 es
precision mediump float;

uniform sampler2D uSrc;
uniform vec2      uSrcTexelSize;   // 1/srcWidth, 1/srcHeight

in  vec2 vTexCoord;
out vec4 fragColor;

void main() {
    const float k[5] = float[5](1.0, 4.0, 6.0, 4.0, 1.0);
    vec4  sum  = vec4(0.0);
    float wsum = 0.0;
    for (int j = -2; j <= 2; ++j) {
        for (int i = -2; i <= 2; ++i) {
            float wgt  = k[i + 2] * k[j + 2];
            vec2  offs = vec2(float(i), float(j)) * uSrcTexelSize;
            sum  += texture(uSrc, vTexCoord + offs) * wgt;
            wsum += wgt;
        }
    }
    fragColor = sum / wsum;
}
)glsl";

// Pass 3/5: Laplacian (band-pass) level = this level's Gaussian minus the
// next (coarser) level's Gaussian, upsampled back to this level's
// resolution. Upsampling is just a plain texture() fetch of the smaller
// source at this pass's (larger) destination resolution -- GL_LINEAR
// bilinear filtering on the bound texture does the interpolation, no
// separate upsample shader/pass needed. That level's weight (alpha, already
// blurred/downsampled in lockstep with color by the pass above) carries
// through unchanged.
static const char kFS_PYR_LAPLACE[] = R"glsl(
#version 300 es
precision mediump float;

uniform sampler2D uGaussCur;    // this level, full res at this level
uniform sampler2D uGaussNext;   // next (coarser) level, half res -> upsampled by sampling here

in  vec2 vTexCoord;
out vec4 fragColor;

void main() {
    vec4 cur = texture(uGaussCur,  vTexCoord);
    vec3 up  = texture(uGaussNext, vTexCoord).rgb;
    fragColor = vec4(cur.rgb - up, cur.a);
}
)glsl";

// Pass 4/5: cross-camera blend at one pyramid level -- reads ALL N cameras'
// textures for that level in one draw, so (like kFS_MULTI) this is the one
// shader in the pyramid pipeline that hits Mesa/V3D's "no variable index
// into a sampler array" restriction, and needs the same manual-unroll fix.
// MAX_PYR_CAMERAS must track GpuRenderer::kPyramidMaxCameras (same
// hand-maintained-list caveat as kFS_MULTI's MAX_CAMERAS/PROCESS_CAMERA
// list). Used for every level including the coarsest: at the coarsest level
// the caller binds each camera's coarsest Gaussian (not Laplacian) texture
// to uLevelTex[i], making this exactly kFS_MULTI's normalized weighted
// average, evaluated once at low resolution -- not a coincidence.
static const char kFS_PYR_BLEND[] = R"glsl(
#version 300 es
precision mediump float;

#define MAX_PYR_CAMERAS 4

uniform sampler2D uLevelTex[MAX_PYR_CAMERAS];
uniform int       uNumCameras;

in  vec2 vTexCoord;
out vec4 fragColor;

#define PROCESS_PYR_CAMERA(IDX)                             \
    if (IDX < uNumCameras) {                                 \
        vec4 t = texture(uLevelTex[IDX], vTexCoord);         \
        colorSum  += t.rgb * t.a;                            \
        weightSum += t.a;                                    \
    }

void main() {
    vec3  colorSum  = vec3(0.0);
    float weightSum = 0.0;

    PROCESS_PYR_CAMERA(0)
    PROCESS_PYR_CAMERA(1)
    PROCESS_PYR_CAMERA(2)
    PROCESS_PYR_CAMERA(3)

    // No clamp -- blended Laplacian bands can be negative/out-of-[0,1];
    // only the final reconstructed image (kFS_PYR_RECON's sum) represents
    // actual display color.
    fragColor = vec4(weightSum > 0.0 ? colorSum / weightSum : vec3(0.0), 1.0);
}
)glsl";

// Pass 5/5: reconstruction -- this level's blended band plus the next
// coarser level's already-reconstructed result, upsampled the same
// bilinear-fetch way kFS_PYR_LAPLACE does. Run from the coarsest level
// downward; the coarsest level itself needs no reconstruction pass (its
// blended texture from kFS_PYR_BLEND already *is* that level's result) --
// see render_frame_multi_pyramid()'s loop, which starts at level K-2.
static const char kFS_PYR_RECON[] = R"glsl(
#version 300 es
precision mediump float;

uniform sampler2D uBlended;      // this level's cross-camera-blended texture
uniform sampler2D uPrevResult;   // next coarser level's reconstructed result

in  vec2 vTexCoord;
out vec4 fragColor;

void main() {
    vec3 blended = texture(uBlended,    vTexCoord).rgb;
    vec3 prev    = texture(uPrevResult, vTexCoord).rgb;
    fragColor = vec4(blended + prev, 1.0);
}
)glsl";

static const char kVS[] = R"glsl(
#version 300 es
out vec2 vTexCoord;
void main() {
    const vec2 pos[3] = vec2[3](
        vec2(-1.0, -1.0),
        vec2( 3.0, -1.0),
        vec2(-1.0,  3.0)
    );
    vTexCoord   = pos[gl_VertexID] * 0.5 + 0.5;
    gl_Position = vec4(pos[gl_VertexID], 0.0, 1.0);
}
)glsl";

static const char kFS[] = R"glsl(
#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES uTexture;
in  vec2 vTexCoord;
out vec4 fragColor;
void main() {
    /* Flip Y: DMA-BUF origin is top-left, GL UV origin is bottom-left. */
    vec2 uv  = vec2(vTexCoord.x, 1.0 - vTexCoord.y);
    vec3 rgb = texture(uTexture, uv).rgb;

    /* Red tint: boost red channel, suppress green and blue. */
    vec3 tinted = vec3(min(rgb.r * 1.5, 1.0), rgb.g * 0.5, rgb.b * 0.5);

    /* Parking guide grid:
     *  3 horizontal lines at 25%, 50%, 75% — depth cues
     *  2 vertical lines at 33%, 66%        — lane boundaries
     */
    float lw = 0.005;
    float h1 = 1.0 - step(lw, abs(uv.y - 0.25));
    float h2 = 1.0 - step(lw, abs(uv.y - 0.50));
    float h3 = 1.0 - step(lw, abs(uv.y - 0.75));
    float v1 = 1.0 - step(lw, abs(uv.x - 0.33));
    float v2 = 1.0 - step(lw, abs(uv.x - 0.66));
    float grid = max(max(max(h1, h2), max(h3, v1)), v2);

    fragColor = vec4(mix(tinted, vec3(1.0), grid), 1.0);
}
)glsl";

// ── Helpers ───────────────────────────────────────────────────────────────────

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetShaderInfoLog(s, sizeof(log), nullptr, log);
        std::cerr << "[gpu] shader compile error:\n" << log << "\n";
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint build_program(const char *vs_src, const char *fs_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER,   vs_src);
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) { glDeleteShader(vs); glDeleteShader(fs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);

    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024] = {};
        glGetProgramInfoLog(prog, sizeof(log), nullptr, log);
        std::cerr << "[gpu] program link error:\n" << log << "\n";
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

// ── Shared helpers ────────────────────────────────────────────────────────────

void GpuRenderer::collect_timer()
{
    if (!timer_pending_ || !gpu_stats_.available) return;
    GLuint64 ns = 0;
    pfn_GetQuery64(timer_query_, GL_QUERY_RESULT_EXT, &ns);
    double ms = (double)ns * 1e-6;
    auto &s = gpu_stats_;
    if (s.warmup_count < GpuStats::WARMUP_N) s.warmup_ms[s.warmup_count++] = ms;
    s.sum_ms += ms;
    if (ms < s.min_ms) s.min_ms = ms;
    if (ms > s.max_ms) s.max_ms = ms;
    ++s.frames;
    timer_pending_ = false;
}

// Upload one NV12 frame's Y and UV planes to the given GL textures.
// unit_y / unit_uv are the GL_TEXTUREn indices (0-based).
void GpuRenderer::upload_nv12(const DmaBufFrame &f,
                               GLuint tex_y, GLuint tex_uv,
                               int unit_y, int unit_uv)
{
    glPixelStorei(GL_UNPACK_ROW_LENGTH, f.stride);
    glActiveTexture(GL_TEXTURE0 + unit_y);
    glBindTexture(GL_TEXTURE_2D, tex_y);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8,
                 f.width, f.height, 0,
                 GL_RED, GL_UNSIGNED_BYTE,
                 f.data + f.y_offset);

    // UV plane: stride/2 GL_RG pixels per row (2 bytes per pixel).
    glPixelStorei(GL_UNPACK_ROW_LENGTH, f.stride / 2);
    glActiveTexture(GL_TEXTURE0 + unit_uv);
    glBindTexture(GL_TEXTURE_2D, tex_uv);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8,
                 f.width / 2, f.height / 2, 0,
                 GL_RG, GL_UNSIGNED_BYTE,
                 f.data + f.uv_offset);

    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

// Cache a pre-built EGLImage + texture for a given fd.
void GpuRenderer::cache_frame(int fd, EGLImageKHR img)
{
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    pfn_TexImage2DOES(GL_TEXTURE_EXTERNAL_OES, img);
    fd_cache_[fd] = {img, tex};
}

// ── GpuRenderer ───────────────────────────────────────────────────────────────

bool GpuRenderer::load_ext_fns()
{
    pfn_CreateImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(
        eglGetProcAddress("eglCreateImageKHR"));
    pfn_DestroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(
        eglGetProcAddress("eglDestroyImageKHR"));
    pfn_TexImage2DOES = reinterpret_cast<PFNGLEGLIMAGETARGETTEXTURE2DOESPROC>(
        eglGetProcAddress("glEGLImageTargetTexture2DOES"));
    if (!pfn_CreateImage || !pfn_DestroyImage || !pfn_TexImage2DOES) {
        std::cerr << "[gpu] failed to resolve EGL/GL extension functions\n";
        return false;
    }

    // Optional: GL_EXT_disjoint_timer_query for true GPU execution time.
    pfn_GenQueries = reinterpret_cast<PFNGLGENQUERIESEXTPROC>(
        eglGetProcAddress("glGenQueriesEXT"));
    pfn_DelQueries = reinterpret_cast<PFNGLDELETEQUERIESEXTPROC>(
        eglGetProcAddress("glDeleteQueriesEXT"));
    pfn_BeginQuery = reinterpret_cast<PFNGLBEGINQUERYEXTPROC>(
        eglGetProcAddress("glBeginQueryEXT"));
    pfn_EndQuery   = reinterpret_cast<PFNGLENDQUERYEXTPROC>(
        eglGetProcAddress("glEndQueryEXT"));
    pfn_GetQuery64 = reinterpret_cast<PFNGLGETQUERYOBJECTUI64VEXTPROC>(
        eglGetProcAddress("glGetQueryObjectui64vEXT"));

    gpu_stats_.available = pfn_GenQueries && pfn_DelQueries &&
                           pfn_BeginQuery  && pfn_EndQuery   && pfn_GetQuery64;
    if (!gpu_stats_.available)
        std::printf("[gpu] GL_EXT_disjoint_timer_query not available — GPU time not measured\n");

    return true;
}

EGLImageKHR GpuRenderer::create_egl_image(const FrameBuffer *buf)
{
    const auto &planes = buf->planes();
    const EGLint attrs[] = {
        EGL_WIDTH,                      W_,
        EGL_HEIGHT,                     H_,
        EGL_LINUX_DRM_FOURCC_EXT,       DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT,      planes[0].fd.get(),
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  (EGLint)planes[0].offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   stride_,
        EGL_DMA_BUF_PLANE1_FD_EXT,      planes[1].fd.get(),
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,  (EGLint)planes[1].offset,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,   stride_,
        EGL_NONE,
    };
    return pfn_CreateImage(egl_->dpy, EGL_NO_CONTEXT,
                           EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
}

EGLImageKHR GpuRenderer::create_egl_image(const DmaBufFrame &f)
{
    const EGLint attrs[] = {
        EGL_WIDTH,                      W_,
        EGL_HEIGHT,                     H_,
        EGL_LINUX_DRM_FOURCC_EXT,       DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT,      f.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,  f.y_offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,   stride_,
        EGL_DMA_BUF_PLANE1_FD_EXT,      f.fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,  f.uv_offset,
        EGL_DMA_BUF_PLANE1_PITCH_EXT,   stride_,
        EGL_NONE,
    };
    return pfn_CreateImage(egl_->dpy, EGL_NO_CONTEXT,
                           EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
}

// File mode init — system-memory NV12 path via sampler2D textures.
bool GpuRenderer::init(const EGLState &egl, int w, int h, int stride)
{
    egl_    = &egl;
    W_      = w;
    H_      = h;
    stride_ = stride;

    if (!load_ext_fns()) return false;

    double t0 = now_ms();
    prog_2d_ = build_program(kVS, kFS_2D);
    if (!prog_2d_) return false;
    std::printf("[gpu] shader compile+link : %.1f ms (one-time)\n", now_ms() - t0);

    glUseProgram(prog_2d_);
    glUniform1i(glGetUniformLocation(prog_2d_, "uTexY"),  0);
    glUniform1i(glGetUniformLocation(prog_2d_, "uTexUV"), 1);

    // Allocate persistent Y and UV textures (data uploaded each frame).
    glGenTextures(1, &tex_y_);
    glBindTexture(GL_TEXTURE_2D, tex_y_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &tex_uv_);
    glBindTexture(GL_TEXTURE_2D, tex_uv_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo_);
    glGenTextures(1,     &fbo_tex_);
    glBindTexture(GL_TEXTURE_2D, fbo_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W_, H_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fbo_tex_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[gpu] FBO incomplete\n";
        return false;
    }
    glViewport(0, 0, W_, H_);

    if (gpu_stats_.available)
        pfn_GenQueries(1, &timer_query_);

    std::printf("[gpu] renderer ready (file/sysmem mode): %dx%d stride=%d\n", W_, H_, stride_);
    return true;
}

bool GpuRenderer::init_dual(const EGLState &egl, int w, int h, int stride)
{
    // Build the base single-file setup (FBO, tex_y_, tex_uv_, prog_2d_).
    if (!init(egl, w, h, stride)) return false;

    double t0 = now_ms();
    prog_dual_ = build_program(kVS, kFS_DUAL);
    if (!prog_dual_) return false;
    std::printf("[gpu] dual shader compile+link : %.1f ms\n", now_ms() - t0);

    glUseProgram(prog_dual_);
    glUniform1i(glGetUniformLocation(prog_dual_, "uTexY0"),  0);
    glUniform1i(glGetUniformLocation(prog_dual_, "uTexUV0"), 1);
    glUniform1i(glGetUniformLocation(prog_dual_, "uTexY1"),  2);
    glUniform1i(glGetUniformLocation(prog_dual_, "uTexUV1"), 3);
    // uOverlap is now a blend half-width in meters (was an image-fraction
    // before IPM); 0.40 m is a reasonable starting width around the
    // baseline midline, tunable at runtime via set_stitch_overlap().
    glUniform1f(glGetUniformLocation(prog_dual_, "uOverlap"),    0.40f);
    glUniform1f(glGetUniformLocation(prog_dual_, "uBlendEdge"), 0.45f);
    // BEV canvas size — fixed for the lifetime of this renderer (same WxH
    // as the FBO), unlike uOverlap/uBlendEdge/uPxPerM which are tunable.
    glUniform1f(glGetUniformLocation(prog_dual_, "uBevWidth"),  (float)w);
    glUniform1f(glGetUniformLocation(prog_dual_, "uBevHeight"), (float)h);
    glUniform1f(glGetUniformLocation(prog_dual_, "uPxPerM"),    100.0f);

    auto make_tex = [](GLuint &t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    make_tex(tex_y2_);
    make_tex(tex_uv2_);

    std::printf("[gpu] renderer ready (dual file mode): %dx%d\n", W_, H_);
    return true;
}

bool GpuRenderer::init_bev()
{
    // Assumes init() (single-file mode) has already run — needs tex_y_/
    // tex_uv_/fbo_ and W_/H_, same as the dual path relies on init_dual()
    // calling init() first.
    double t0 = now_ms();
    prog_bev_ = build_program(kVS, kFS_BEV);
    if (!prog_bev_) return false;
    std::printf("[gpu] bev shader compile+link : %.1f ms\n", now_ms() - t0);

    glUseProgram(prog_bev_);
    glUniform1i(glGetUniformLocation(prog_bev_, "uTexY"),  0);
    glUniform1i(glGetUniformLocation(prog_bev_, "uTexUV"), 1);
    // BEV canvas size — fixed for the lifetime of this renderer (same WxH
    // as the FBO), matching kFS_DUAL's uBevWidth/uBevHeight.
    glUniform1f(glGetUniformLocation(prog_bev_, "uBevWidth"),  (float)W_);
    glUniform1f(glGetUniformLocation(prog_bev_, "uBevHeight"), (float)H_);
    bev_u_H_ = glGetUniformLocation(prog_bev_, "uH");

    std::printf("[gpu] bev (forward, single-camera) ready: %dx%d\n", W_, H_);
    return true;
}

void GpuRenderer::set_bev(const float H[9])
{
    if (!prog_bev_) return;
    glUseProgram(prog_bev_);
    glUniformMatrix3fv(bev_u_H_, 1, GL_FALSE, H);
}

bool GpuRenderer::init(const EGLState &egl,
                       const StreamConfiguration &sc,
                       const std::vector<std::unique_ptr<FrameBuffer>> &buffers)
{
    egl_    = &egl;
    W_      = (int)sc.size.width;
    H_      = (int)sc.size.height;
    stride_ = (int)sc.stride;

    if (!load_ext_fns()) return false;

    // ── Compile shader once ───────────────────────────────────────────────
    double t0 = now_ms();
    prog_ = build_program(kVS, kFS);
    if (!prog_) return false;
    std::printf("[gpu] shader compile+link : %.1f ms (one-time)\n", now_ms() - t0);

    glUseProgram(prog_);
    glUniform1i(glGetUniformLocation(prog_, "uTexture"), 0);

    // ── Create FBO once ───────────────────────────────────────────────────
    glGenFramebuffers(1, &fbo_);
    glGenTextures(1,     &fbo_tex_);
    glBindTexture(GL_TEXTURE_2D, fbo_tex_);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W_, H_, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, fbo_tex_, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        std::cerr << "[gpu] FBO incomplete\n";
        return false;
    }
    glViewport(0, 0, W_, H_);

    // ── GPU timer query object (one, single-buffered) ─────────────────────
    if (gpu_stats_.available)
        pfn_GenQueries(1, &timer_query_);

    // ── Pre-cache EGLImage + texture per buffer ───────────────────────────
    // eglCreateImageKHR is called here ONCE per buffer, not per frame.
    // The fd→EGLImage binding is stable for the lifetime of the buffer pool.
    for (const auto &buf : buffers) {
        int fd = buf->planes()[0].fd.get();

        EGLImageKHR img = create_egl_image(buf.get());
        if (img == EGL_NO_IMAGE_KHR) {
            std::cerr << "[gpu] eglCreateImageKHR failed for fd=" << fd
                      << " (0x" << std::hex << eglGetError() << std::dec << ")\n";
            return false;
        }

        cache_frame(fd, img);
        std::printf("[gpu] cached fd=%-3d → EGLImage+texture\n", fd);
    }

    std::printf("[gpu] renderer ready: %dx%d, %zu buffer(s) cached\n",
                W_, H_, fd_cache_.size());
    return true;
}

void GpuRenderer::render_frame(const FrameBuffer *buf)
{
    collect_timer();

    int fd = buf->planes()[0].fd.get();
    auto it = fd_cache_.find(fd);
    if (it == fd_cache_.end()) {
        std::cerr << "[gpu] unknown fd=" << fd << " — frame skipped\n";
        return;
    }

    // Re-establish our FBO/program/viewport explicitly rather than assuming
    // they're still bound — a caller may have rebound framebuffer 0 and a
    // different program/viewport in between calls (e.g. --preview's window
    // composite pass), same as the other render_frame() overloads already do.
    glUseProgram(prog_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, W_, H_);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, it->second.texture);

    if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }

    glFlush(); // submit to GPU; don't stall CPU
}

void GpuRenderer::render_frame(const DmaBufFrame &frame)
{
    collect_timer();

    if (frame.fd < 0) {
        // System memory path (file mode).
        upload_nv12(frame, tex_y_, tex_uv_, 0, 1);
        glUseProgram(bev_enabled_ && prog_bev_ ? prog_bev_ : prog_2d_);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
        glViewport(0, 0, W_, H_);
        if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }
        glFlush();
        return;
    }

    // DMA-BUF path (camera mode).
    auto it = fd_cache_.find(frame.fd);
    if (it == fd_cache_.end()) {
        EGLImageKHR img = create_egl_image(frame);
        if (img == EGL_NO_IMAGE_KHR) {
            std::cerr << "[gpu] eglCreateImageKHR failed for fd=" << frame.fd
                      << " (0x" << std::hex << eglGetError() << std::dec << ")\n";
            return;
        }
        cache_frame(frame.fd, img);
        std::printf("[gpu] cached fd=%-3d → EGLImage+texture (lazy)\n", frame.fd);
        it = fd_cache_.find(frame.fd);
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, it->second.texture);

    if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }
    glFlush();
}

void GpuRenderer::set_stitch_overlap(float v)
{
    if (prog_dual_) {
        glUseProgram(prog_dual_);
        glUniform1f(glGetUniformLocation(prog_dual_, "uOverlap"), v);
    }
    if (prog_multi_) {
        glUseProgram(prog_multi_);
        glUniform1f(glGetUniformLocation(prog_multi_, "uOverlap"), v);
    }
    if (prog_pyr_warp_) {
        glUseProgram(prog_pyr_warp_);
        glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uOverlap"), v);
    }
}

void GpuRenderer::set_blend_edge(float v)
{
    if (prog_dual_) {
        glUseProgram(prog_dual_);
        glUniform1f(glGetUniformLocation(prog_dual_, "uBlendEdge"), v);
    }
    if (prog_multi_) {
        glUseProgram(prog_multi_);
        glUniform1f(glGetUniformLocation(prog_multi_, "uBlendEdge"), v);
    }
    if (prog_pyr_warp_) {
        glUseProgram(prog_pyr_warp_);
        glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uBlendEdge"), v);
    }
}

void GpuRenderer::set_free_yaw(bool on)
{
    // kFS_DUAL has no facing/wedge concept at all (its blend is a plain
    // world-X seam), so there's nothing to bypass there -- multi/pyramid
    // only.
    float v = on ? 1.0f : 0.0f;
    if (prog_multi_) {
        glUseProgram(prog_multi_);
        glUniform1f(glGetUniformLocation(prog_multi_, "uFreeYaw"), v);
    }
    if (prog_pyr_warp_) {
        glUseProgram(prog_pyr_warp_);
        glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uFreeYaw"), v);
    }
}

void GpuRenderer::set_ipm(const float H_left[9], const float H_right[9])
{
    if (!prog_dual_) return;
    glUseProgram(prog_dual_);
    glUniformMatrix3fv(glGetUniformLocation(prog_dual_, "uH_L"), 1, GL_FALSE, H_left);
    glUniformMatrix3fv(glGetUniformLocation(prog_dual_, "uH_R"), 1, GL_FALSE, H_right);
}

void GpuRenderer::set_px_per_m(float v)
{
    if (prog_dual_) {
        glUseProgram(prog_dual_);
        glUniform1f(glGetUniformLocation(prog_dual_, "uPxPerM"), v);
    }
    if (prog_multi_) {
        glUseProgram(prog_multi_);
        glUniform1f(glGetUniformLocation(prog_multi_, "uPxPerM"), v);
    }
    if (prog_pyr_warp_) {
        glUseProgram(prog_pyr_warp_);
        glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uPxPerM"), v);
    }
}

void GpuRenderer::render_frame(const DmaBufFrame &left, const DmaBufFrame &right)
{
    collect_timer();

    upload_nv12(left,  tex_y_,  tex_uv_,  0, 1);
    upload_nv12(right, tex_y2_, tex_uv2_, 2, 3);

    glUseProgram(prog_dual_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, W_, H_);

    if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }
    glFlush();
}

bool GpuRenderer::init_multi(const EGLState &egl, int w, int h, int stride, int num_cameras)
{
    if (num_cameras < 1 || num_cameras > kMaxCameras) {
        std::cerr << "[gpu] init_multi: num_cameras=" << num_cameras
                  << " out of range [1," << kMaxCameras << "]\n";
        return false;
    }

    // GLES 3.0 only guarantees 16 fragment texture image units — 2 per
    // camera (Y+UV) means kMaxCameras=8 is already at that guaranteed
    // floor. Check the real driver limit too, since some hardware may not
    // even reach the guaranteed minimum in practice.
    GLint max_units = 0;
    glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &max_units);
    if (2 * num_cameras > max_units) {
        std::cerr << "[gpu] init_multi: " << num_cameras << " cameras need "
                  << (2 * num_cameras) << " texture units, but this driver "
                     "only supports " << max_units << "\n";
        return false;
    }

    // Build the base single-file setup (FBO, tex_y_, tex_uv_, prog_2d_) —
    // unused by multi mode, but every specialized init_*() delegates here
    // for the shared FBO/viewport/timer-query setup, same as init_dual()/
    // init_bev() already do.
    if (!init(egl, w, h, stride)) return false;

    double t0 = now_ms();
    prog_multi_ = build_program(kVS, kFS_MULTI);
    if (!prog_multi_) return false;
    std::printf("[gpu] multi (%d-camera) shader compile+link : %.1f ms\n",
                num_cameras, now_ms() - t0);

    glUseProgram(prog_multi_);
    char name[32];
    for (int i = 0; i < num_cameras; ++i) {
        std::snprintf(name, sizeof(name), "uTexY[%d]", i);
        glUniform1i(glGetUniformLocation(prog_multi_, name), 2 * i);
        std::snprintf(name, sizeof(name), "uTexUV[%d]", i);
        glUniform1i(glGetUniformLocation(prog_multi_, name), 2 * i + 1);

        std::snprintf(name, sizeof(name), "uH[%d]", i);
        u_H_multi_[i] = glGetUniformLocation(prog_multi_, name);
        std::snprintf(name, sizeof(name), "uFacingDeg[%d]", i);
        u_facing_multi_[i] = glGetUniformLocation(prog_multi_, name);
    }
    glUniform1i(glGetUniformLocation(prog_multi_, "uNumCameras"), num_cameras);

    // Defaults — 60 deg overlap gives margin over the 45 deg minimum needed
    // for 4 evenly-spaced (90 deg apart) cameras to fully cover 360 deg;
    // tunable at runtime via set_stitch_overlap() (now in degrees).
    glUniform1f(glGetUniformLocation(prog_multi_, "uOverlap"),    60.0f);
    glUniform1f(glGetUniformLocation(prog_multi_, "uBlendEdge"), 0.45f);
    glUniform1f(glGetUniformLocation(prog_multi_, "uFreeYaw"),    0.0f);
    // BEV canvas size — fixed for the lifetime of this renderer, matching
    // kFS_DUAL's uBevWidth/uBevHeight.
    glUniform1f(glGetUniformLocation(prog_multi_, "uBevWidth"),  (float)w);
    glUniform1f(glGetUniformLocation(prog_multi_, "uBevHeight"), (float)h);
    glUniform1f(glGetUniformLocation(prog_multi_, "uPxPerM"),    100.0f);

    auto make_tex = [](GLuint &t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    for (int i = 0; i < num_cameras; ++i) {
        make_tex(tex_y_multi_[i]);
        make_tex(tex_uv_multi_[i]);
    }

    num_cameras_multi_ = num_cameras;
    std::printf("[gpu] renderer ready (%d-camera surround mode): %dx%d\n",
                num_cameras, W_, H_);
    return true;
}

void GpuRenderer::set_ipm_multi(int slot, const float H[9], float facing_deg)
{
    if (!prog_multi_ || slot < 0 || slot >= num_cameras_multi_) return;
    glUseProgram(prog_multi_);
    glUniformMatrix3fv(u_H_multi_[slot], 1, GL_FALSE, H);
    glUniform1f(u_facing_multi_[slot], facing_deg);
}

void GpuRenderer::render_frame_multi(const std::vector<DmaBufFrame> &frames)
{
    collect_timer();

    int n = std::min((int)frames.size(), num_cameras_multi_);
    for (int i = 0; i < n; ++i)
        upload_nv12(frames[i], tex_y_multi_[i], tex_uv_multi_[i], 2 * i, 2 * i + 1);

    glUseProgram(prog_multi_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glViewport(0, 0, W_, H_);

    if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }
    glFlush();
}

bool GpuRenderer::init_multi_pyramid(const EGLState &egl, int w, int h, int stride,
                                     int num_cameras, int num_levels)
{
    if (num_cameras < 1 || num_cameras > kPyramidMaxCameras) {
        std::cerr << "[gpu] init_multi_pyramid: num_cameras=" << num_cameras
                  << " out of range [1," << kPyramidMaxCameras << "]\n";
        return false;
    }
    if (num_levels < 2 || num_levels > kPyramidLevels) {
        std::cerr << "[gpu] init_multi_pyramid: num_levels=" << num_levels
                  << " out of range [2," << kPyramidLevels << "]\n";
        return false;
    }

    // Build the base single-file setup (FBO, tex_y_, tex_uv_, prog_2d_) --
    // unused by pyramid mode, but every specialized init_*() delegates here
    // for the shared FBO/viewport/timer-query setup, same as init_dual()/
    // init_multi() already do.
    if (!init(egl, w, h, stride)) return false;

    // Real RGBA16F color-renderability probe at every resolution the
    // pyramid will actually render to -- extension-string presence alone
    // isn't trusted here (see kFS_MULTI's sampler-array-indexing comment:
    // GLES spec-legal features don't always work on this driver).
    {
        int pw = w, ph = h;
        for (int lvl = 0; lvl < num_levels; ++lvl) {
            GLuint probe_tex = 0, probe_fbo = 0;
            glGenTextures(1, &probe_tex);
            glBindTexture(GL_TEXTURE_2D, probe_tex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, pw, ph, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
            glGenFramebuffers(1, &probe_fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, probe_fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, probe_tex, 0);
            GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glDeleteFramebuffers(1, &probe_fbo);
            glDeleteTextures(1, &probe_tex);
            if (status != GL_FRAMEBUFFER_COMPLETE) {
                const char *gl_exts = reinterpret_cast<const char *>(glGetString(GL_EXTENSIONS));
                bool has_half = gl_exts && std::strstr(gl_exts, "GL_EXT_color_buffer_half_float");
                bool has_full = gl_exts && std::strstr(gl_exts, "GL_EXT_color_buffer_float");
                std::cerr << "[gpu] init_multi_pyramid: GL_RGBA16F is not color-renderable "
                             "at level " << lvl << " (" << pw << "x" << ph << "), "
                             "glCheckFramebufferStatus()=0x" << std::hex << status << std::dec
                          << ". GL_EXT_color_buffer_half_float=" << has_half
                          << " GL_EXT_color_buffer_float=" << has_full
                          << ". No RGBA8 fallback is implemented in this build -- "
                             "use the feather-blend (--src without --blend) mode instead.\n";
                glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
                return false;
            }
            pw = std::max(1, pw / 2);
            ph = std::max(1, ph / 2);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    }

    double t0 = now_ms();
    prog_pyr_warp_       = build_program(kVS, kFS_PYR_WARP);
    prog_pyr_downsample_ = build_program(kVS, kFS_PYR_DOWNSAMPLE);
    prog_pyr_laplace_    = build_program(kVS, kFS_PYR_LAPLACE);
    prog_pyr_blend_      = build_program(kVS, kFS_PYR_BLEND);
    prog_pyr_recon_      = build_program(kVS, kFS_PYR_RECON);
    if (!prog_pyr_warp_ || !prog_pyr_downsample_ || !prog_pyr_laplace_ ||
        !prog_pyr_blend_ || !prog_pyr_recon_)
        return false;
    std::printf("[gpu] pyramid (%d-camera, %d-level) shaders compile+link : %.1f ms\n",
                num_cameras, num_levels, now_ms() - t0);

    // Fixed uniform assignments -- set once here, never touched again
    // (unlike u_H_pyr_loc_/u_facing_pyr_loc_/u_pyr_texel_loc_ below, whose
    // VALUES vary per camera/level every frame).
    glUseProgram(prog_pyr_warp_);
    glUniform1i(glGetUniformLocation(prog_pyr_warp_, "uTexY"),  0);
    glUniform1i(glGetUniformLocation(prog_pyr_warp_, "uTexUV"), 1);
    glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uOverlap"),    60.0f);
    glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uBlendEdge"), 0.45f);
    glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uFreeYaw"),    0.0f);
    glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uBevWidth"),  (float)w);
    glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uBevHeight"), (float)h);
    glUniform1f(glGetUniformLocation(prog_pyr_warp_, "uPxPerM"),    100.0f);
    u_H_pyr_loc_      = glGetUniformLocation(prog_pyr_warp_, "uH");
    u_facing_pyr_loc_ = glGetUniformLocation(prog_pyr_warp_, "uFacingDeg");

    glUseProgram(prog_pyr_downsample_);
    glUniform1i(glGetUniformLocation(prog_pyr_downsample_, "uSrc"), 0);
    u_pyr_texel_loc_ = glGetUniformLocation(prog_pyr_downsample_, "uSrcTexelSize");

    glUseProgram(prog_pyr_laplace_);
    glUniform1i(glGetUniformLocation(prog_pyr_laplace_, "uGaussCur"),  0);
    glUniform1i(glGetUniformLocation(prog_pyr_laplace_, "uGaussNext"), 1);

    glUseProgram(prog_pyr_blend_);
    char name[32];
    for (int i = 0; i < num_cameras; ++i) {
        std::snprintf(name, sizeof(name), "uLevelTex[%d]", i);
        glUniform1i(glGetUniformLocation(prog_pyr_blend_, name), i);
    }
    glUniform1i(glGetUniformLocation(prog_pyr_blend_, "uNumCameras"), num_cameras);

    glUseProgram(prog_pyr_recon_);
    glUniform1i(glGetUniformLocation(prog_pyr_recon_, "uBlended"),    0);
    glUniform1i(glGetUniformLocation(prog_pyr_recon_, "uPrevResult"), 1);

    // ── Textures/FBO ────────────────────────────────────────────────────────
    glGenFramebuffers(1, &fbo_pyr_);

    auto make_tex = [](GLuint &t) {
        glGenTextures(1, &t);
        glBindTexture(GL_TEXTURE_2D, t);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };
    auto make_pyr_tex = [&](GLuint &t, int tw, int th) {
        make_tex(t);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA16F, tw, th, 0, GL_RGBA, GL_HALF_FLOAT, nullptr);
    };

    for (int i = 0; i < num_cameras; ++i) {
        make_tex(tex_y_pyr_[i]);
        make_tex(tex_uv_pyr_[i]);
    }

    int lw = w, lh = h;
    for (int lvl = 0; lvl < num_levels; ++lvl) {
        for (int i = 0; i < num_cameras; ++i)
            make_pyr_tex(tex_pyr_gauss_[i][lvl], lw, lh);
        if (lvl < num_levels - 1)
            for (int i = 0; i < num_cameras; ++i)
                make_pyr_tex(tex_pyr_lap_[i][lvl], lw, lh);
        make_pyr_tex(tex_pyr_blended_[lvl], lw, lh);
        // Level 0's reconstruction writes straight to fbo_tex_ (see
        // render_frame_multi_pyramid()) and the coarsest level needs no
        // reconstruction pass at all -- only intermediate levels need their
        // own result texture.
        if (lvl > 0 && lvl < num_levels - 1)
            make_pyr_tex(tex_pyr_result_[lvl], lw, lh);
        lw = std::max(1, lw / 2);
        lh = std::max(1, lh / 2);
    }

    num_cameras_multi_pyramid_ = num_cameras;
    num_levels_multi_pyramid_  = num_levels;
    std::printf("[gpu] renderer ready (%d-camera, %d-level pyramid surround mode): %dx%d\n",
                num_cameras, num_levels, W_, H_);
    return true;
}

void GpuRenderer::set_ipm_multi_pyramid(int slot, const float H[9], float facing_deg)
{
    if (!prog_pyr_warp_ || slot < 0 || slot >= num_cameras_multi_pyramid_) return;
    // No GL calls here -- the actual glUniform upload happens per-camera
    // inside render_frame_multi_pyramid()'s warp pass, since prog_pyr_warp_
    // draws one camera at a time and reuses the same uH/uFacingDeg location
    // for each (see gpu_renderer.h's u_H_pyr_loc_ comment).
    std::memcpy(H_pyr_[slot], H, sizeof(float) * 9);
    facing_pyr_[slot] = facing_deg;
}

void GpuRenderer::render_frame_multi_pyramid(const std::vector<DmaBufFrame> &frames)
{
    collect_timer();

    int n = std::min((int)frames.size(), num_cameras_multi_pyramid_);
    int K = num_levels_multi_pyramid_;

    for (int i = 0; i < n; ++i)
        upload_nv12(frames[i], tex_y_pyr_[i], tex_uv_pyr_[i], 0, 1);

    int level_w[kPyramidLevels], level_h[kPyramidLevels];
    level_w[0] = W_;
    level_h[0] = H_;
    for (int lvl = 1; lvl < K; ++lvl) {
        level_w[lvl] = std::max(1, level_w[lvl - 1] / 2);
        level_h[lvl] = std::max(1, level_h[lvl - 1] / 2);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo_pyr_);
    if (gpu_stats_.available) pfn_BeginQuery(GL_TIME_ELAPSED_EXT, timer_query_);

    // ── Pass 1/5: per-camera warp + weight -> level-0 Gaussian ─────────────
    glUseProgram(prog_pyr_warp_);
    glViewport(0, 0, level_w[0], level_h[0]);
    for (int i = 0; i < n; ++i) {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_y_pyr_[i]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, tex_uv_pyr_[i]);
        glUniformMatrix3fv(u_H_pyr_loc_, 1, GL_FALSE, H_pyr_[i]);
        glUniform1f(u_facing_pyr_loc_, facing_pyr_[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex_pyr_gauss_[i][0], 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // ── Pass 2/5: Gaussian pyramid (blur+downsample), per camera ───────────
    glUseProgram(prog_pyr_downsample_);
    for (int lvl = 0; lvl < K - 1; ++lvl) {
        glUniform2f(u_pyr_texel_loc_, 1.0f / level_w[lvl], 1.0f / level_h[lvl]);
        glViewport(0, 0, level_w[lvl + 1], level_h[lvl + 1]);
        for (int i = 0; i < n; ++i) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex_pyr_gauss_[i][lvl]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex_pyr_gauss_[i][lvl + 1], 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // ── Pass 3/5: Laplacian-diff, per camera per level ──────────────────────
    glUseProgram(prog_pyr_laplace_);
    for (int lvl = 0; lvl < K - 1; ++lvl) {
        glViewport(0, 0, level_w[lvl], level_h[lvl]);
        for (int i = 0; i < n; ++i) {
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, tex_pyr_gauss_[i][lvl]);
            glActiveTexture(GL_TEXTURE1);
            glBindTexture(GL_TEXTURE_2D, tex_pyr_gauss_[i][lvl + 1]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex_pyr_lap_[i][lvl], 0);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }
    }

    // ── Pass 4/5: cross-camera blend, per level ─────────────────────────────
    // Coarsest level blends each camera's coarsest Gaussian (this is exactly
    // kFS_MULTI's normalized weighted average, evaluated at low res); every
    // other level blends each camera's Laplacian band at that level.
    glUseProgram(prog_pyr_blend_);
    for (int lvl = 0; lvl < K; ++lvl) {
        glViewport(0, 0, level_w[lvl], level_h[lvl]);
        for (int i = 0; i < n; ++i) {
            glActiveTexture(GL_TEXTURE0 + i);
            GLuint src = (lvl == K - 1) ? tex_pyr_gauss_[i][lvl] : tex_pyr_lap_[i][lvl];
            glBindTexture(GL_TEXTURE_2D, src);
        }
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, tex_pyr_blended_[lvl], 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }

    // ── Pass 5/5: reconstruction, coarsest to finest ────────────────────────
    // result_{K-1} = blended_{K-1} directly (no pass needed); level 0's
    // result is written straight to fbo_tex_ -- same output contract as
    // render_frame_multi(), so --preview/save_snapshot() need no changes.
    glUseProgram(prog_pyr_recon_);
    GLuint prev_result_tex = tex_pyr_blended_[K - 1];
    for (int lvl = K - 2; lvl >= 0; --lvl) {
        glViewport(0, 0, level_w[lvl], level_h[lvl]);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, tex_pyr_blended_[lvl]);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, prev_result_tex);
        GLuint dest = (lvl == 0) ? fbo_tex_ : tex_pyr_result_[lvl];
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, dest, 0);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        prev_result_tex = dest;
    }

    if (gpu_stats_.available) { pfn_EndQuery(GL_TIME_ELAPSED_EXT); timer_pending_ = true; }
    glFlush();
}

bool GpuRenderer::save_snapshot(const char *path)
{
    // Re-bind our FBO explicitly rather than assuming it's still current —
    // a caller may have rebound framebuffer 0 in between (e.g. --preview's
    // window composite pass), same fix as render_frame(FrameBuffer*) needed.
    // Reading framebuffer 0 right after eglSwapBuffers() is especially bad:
    // that buffer's contents are undefined until the next frame renders,
    // which is why this previously read back as uniform (0,0,0,0) garbage.
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);

    // glFlush was already called in render_frame(); wait for GPU to finish.
    glFinish();

    std::vector<uint8_t> pixels((size_t)W_ * (size_t)H_ * 4);
    double t0 = now_ms();
    glReadPixels(0, 0, W_, H_, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());

    GLenum err = glGetError();
    if (err != GL_NO_ERROR) {
        std::fprintf(stderr, "[gpu] glReadPixels error: 0x%x\n", err);
        return false;
    }
    std::printf("[perf] glReadPixels : %.1f ms\n", now_ms() - t0);

    // Write PNG — rows are bottom-to-top from glReadPixels, flip for PNG.
    FILE *fp = fopen(path, "wb");
    if (!fp) { perror("[gpu] fopen"); return false; }

    png_structp png  = png_create_write_struct(PNG_LIBPNG_VER_STRING,
                                               nullptr, nullptr, nullptr);
    png_infop   info = png ? png_create_info_struct(png) : nullptr;
    if (!png || !info || setjmp(png_jmpbuf(png))) {
        png_destroy_write_struct(&png, &info);
        fclose(fp);
        return false;
    }

    png_init_io(png, fp);
    png_set_IHDR(png, info, (png_uint_32)W_, (png_uint_32)H_, 8,
                 PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
                 PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
    png_set_compression_level(png, 1); // fast encode, larger file
    png_write_info(png, info);

    for (int y = H_ - 1; y >= 0; --y)
        png_write_row(png,
            reinterpret_cast<png_const_bytep>(pixels.data() + (size_t)y * W_ * 4));

    png_write_end(png, nullptr);
    png_destroy_write_struct(&png, &info);
    fclose(fp);

    std::printf("[gpu] snapshot saved → %s\n", path);
    return true;
}

void GpuRenderer::cleanup()
{
    if (!egl_) return;

    for (auto &[fd, res] : fd_cache_) {
        if (res.texture) glDeleteTextures(1, &res.texture);
        if (res.image != EGL_NO_IMAGE_KHR)
            pfn_DestroyImage(egl_->dpy, res.image);
    }
    fd_cache_.clear();

    if (timer_query_ && pfn_DelQueries) { pfn_DelQueries(1, &timer_query_); timer_query_ = 0; }
    if (fbo_)       { glDeleteFramebuffers(1,  &fbo_);       fbo_       = 0; }
    if (fbo_tex_)   { glDeleteTextures(1,      &fbo_tex_);   fbo_tex_   = 0; }
    if (prog_)      { glDeleteProgram(prog_);                 prog_      = 0; }
    if (prog_2d_)   { glDeleteProgram(prog_2d_);              prog_2d_   = 0; }
    if (prog_dual_) { glDeleteProgram(prog_dual_);            prog_dual_ = 0; }
    if (prog_bev_)  { glDeleteProgram(prog_bev_);             prog_bev_  = 0; }
    if (prog_multi_) { glDeleteProgram(prog_multi_);          prog_multi_ = 0; }
    if (tex_y_)     { glDeleteTextures(1, &tex_y_);          tex_y_     = 0; }
    if (tex_uv_)    { glDeleteTextures(1, &tex_uv_);         tex_uv_    = 0; }
    if (tex_y2_)    { glDeleteTextures(1, &tex_y2_);         tex_y2_    = 0; }
    if (tex_uv2_)   { glDeleteTextures(1, &tex_uv2_);        tex_uv2_   = 0; }
    for (int i = 0; i < num_cameras_multi_; ++i) {
        if (tex_y_multi_[i])  { glDeleteTextures(1, &tex_y_multi_[i]);  tex_y_multi_[i]  = 0; }
        if (tex_uv_multi_[i]) { glDeleteTextures(1, &tex_uv_multi_[i]); tex_uv_multi_[i] = 0; }
    }
    num_cameras_multi_ = 0;

    if (prog_pyr_warp_)       { glDeleteProgram(prog_pyr_warp_);       prog_pyr_warp_       = 0; }
    if (prog_pyr_downsample_) { glDeleteProgram(prog_pyr_downsample_); prog_pyr_downsample_ = 0; }
    if (prog_pyr_laplace_)    { glDeleteProgram(prog_pyr_laplace_);    prog_pyr_laplace_    = 0; }
    if (prog_pyr_blend_)      { glDeleteProgram(prog_pyr_blend_);      prog_pyr_blend_      = 0; }
    if (prog_pyr_recon_)      { glDeleteProgram(prog_pyr_recon_);      prog_pyr_recon_      = 0; }
    if (fbo_pyr_)             { glDeleteFramebuffers(1, &fbo_pyr_);    fbo_pyr_             = 0; }
    for (int i = 0; i < num_cameras_multi_pyramid_; ++i) {
        if (tex_y_pyr_[i])  { glDeleteTextures(1, &tex_y_pyr_[i]);  tex_y_pyr_[i]  = 0; }
        if (tex_uv_pyr_[i]) { glDeleteTextures(1, &tex_uv_pyr_[i]); tex_uv_pyr_[i] = 0; }
        for (int lvl = 0; lvl < num_levels_multi_pyramid_; ++lvl) {
            if (tex_pyr_gauss_[i][lvl]) {
                glDeleteTextures(1, &tex_pyr_gauss_[i][lvl]);
                tex_pyr_gauss_[i][lvl] = 0;
            }
            if (lvl < num_levels_multi_pyramid_ - 1 && tex_pyr_lap_[i][lvl]) {
                glDeleteTextures(1, &tex_pyr_lap_[i][lvl]);
                tex_pyr_lap_[i][lvl] = 0;
            }
        }
    }
    for (int lvl = 0; lvl < num_levels_multi_pyramid_; ++lvl) {
        if (tex_pyr_blended_[lvl]) { glDeleteTextures(1, &tex_pyr_blended_[lvl]); tex_pyr_blended_[lvl] = 0; }
        if (lvl > 0 && lvl < num_levels_multi_pyramid_ - 1 && tex_pyr_result_[lvl]) {
            glDeleteTextures(1, &tex_pyr_result_[lvl]);
            tex_pyr_result_[lvl] = 0;
        }
    }
    num_cameras_multi_pyramid_ = 0;
    num_levels_multi_pyramid_  = 0;

    egl_ = nullptr;
}
