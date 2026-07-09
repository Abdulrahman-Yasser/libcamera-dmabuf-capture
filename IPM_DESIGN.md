# IPM (Inverse Perspective Mapping) — Design Notes

Status: **PART 1 only** (`ipm.h` / `ipm.cpp`, pure CPU math, no GL). PART 2–4
(shader rewrite, `gpu_renderer` wiring, `main.cpp` CLI flags) not yet done.

This file explains what was built, the coordinate conventions used, and —
importantly — two places where the implementation deliberately deviates from
the original task write-up, because following it literally would have
produced a broken (or unbuildable) result. Both deviations were discussed
and confirmed before writing any code.

---

## 1. Goal

Each camera is warped onto a shared ground plane so the fragment shader can,
for every pixel of the bird's-eye-view (BEV) output, ask "where in camera
image space does this ground point appear?" and sample from there
(backward/inverse mapping — the standard approach for GPU warps, since it
guarantees every output pixel is filled with no holes).

`ground_to_image_H(...)` builds exactly that: a 3×3 homography `H` such that

```
p = (px, py, 1)              // a BEV output pixel, homogeneous
s = H * p
uv = (s.x / s.z, s.y / s.z)  // camera image coordinate, already in [0,1]
```

One `H` per camera (left/right), rebuilt only when a parameter changes —
never per-frame.

---

## 2. Coordinate conventions

**World / ground frame**: `X` right, `Y` forward, `Z` up. Ground plane is
`Z = 0`. The two cameras sit at `(cam_x, cam_y, h)`, differing only in
`cam_x = ∓baseline/2`.

**Camera frame** (OpenCV/standard pinhole convention): `X` right, `Y` down,
`Z` forward (into the scene, i.e. depth).

**BEV canvas**: `(px, py)` pixel coordinates, `bev_w × bev_h`, with
`(bev_w/2, bev_h/2)` the canvas center. The convention baked into the `M`
matrix (see §4) is *top-origin*, i.e. `py = 0` is the far/top row — the same
convention as a normal image, not GL's bottom-origin `gl_FragCoord`. That
mismatch matters later (see §6).

---

## 3. `build_intrinsics(width, height)` — `ipm.cpp:56`

Straight pinhole intrinsics for the IMX219, scaled from its 3280 px
calibration width:

```
fx = fy = 2714.0 * (width / 3280.0)
cx = width / 2,  cy = height / 2
```

Returned as a standard 3×3 `K`:

```
| fx  0  cx |
| 0  fy  cy |
| 0   0   1 |
```

---

## 4. `build_rotation(pitch_deg, yaw_deg)` — `ipm.cpp:74` (internal, not in the header)

Builds `R`, the **world→camera** rotation, by tracking where the camera's
own right/forward/up axes end up in world coordinates, then reading off
`R = transpose([right | up | forward])` (valid because it's a pure rotation
matrix, so its inverse is its transpose).

Sequence: yaw about world `Z` first (pans `{right, forward}` in the
horizontal plane), then pitch about the *yawed* right axis (tilts
`{forward, up}`). `pitch < 0` tilts `forward` toward `-up`, i.e. **downward**
— matching the spec's "negative = looking down" convention and the default
`pitch = -30`.

Sanity-checked with the degenerate case `pitch = -90°` (straight down): a
camera directly over the world origin should see that origin dead-center.
Confirmed by test (§8).

---

## 5. `ground_to_image_H(...)` — `ipm.cpp:104`

```cpp
mat3 ground_to_image_H(double cam_x, double cam_y, double h,
                       double pitch_deg, double yaw_deg,
                       const mat3 &K,
                       int img_w, int img_h,
                       double px_per_m, int bev_w, int bev_h);
```

Steps:
1. `R = build_rotation(pitch_deg, yaw_deg)`.
2. `t = -R * C` where `C = (cam_x, cam_y, h)` — the standard extrinsic
   translation (camera-center-to-translation-vector conversion).
3. `E = [R | t; 0 0 0 1]` (4×4, via `mat4::from_R_t`).
4. `P = K * E` truncated to 3×4 (rows 0–2) — the classic
   world→image projection matrix.
5. `M` (4×3) maps a BEV pixel to its ground point, exactly as specified:
   ```
   row0: [ 1/r,   0,   -(bev_w/2)/r ]
   row1: [ 0,   -1/r,   (bev_h/2)/r ]
   row2: [ 0,     0,        0       ]   (Z = 0, flat ground)
   row3: [ 0,     0,        1       ]
   ```
6. `H = P * M` (3×4 · 4×3 = 3×3).
7. Normalize: divide `H`'s row 0 by `img_w` and row 1 by `img_h`, so the
   shader's perspective-divided `(u, v)` come out in `[0,1]` directly instead
   of pixel units.

### Deviation A — no inverse

The task write-up said `H = inverse(P * M)`. That's wrong for this shader.
`M` maps BEV-pixel→ground and `P` maps ground→image; by plain matrix
associativity, `P * (M * p) = (P * M) * p`, so **`P*M` already *is* the
direct BEV→image mapping** — no inversion needed, or wanted. Inverting it
would flip the mapping to image→BEV, which is backwards for a shader that
does `uH_L * bevPixel` per fragment (a genuine backward/inverse-warp).

The likely origin of the "inverse" step: OpenCV's `cv2.warpPerspective`
expects a *src→dst* matrix and inverts it internally to do the actual
backward sampling. This project doesn't use OpenCV — the GLSL shader does
the inversion "by construction" (it's already written as backward mapping),
so applying `inverse()` a second time would be a double-inversion bug.

Confirmed with you before implementing; the `mat3::inverse()` method and its
unit self-test (originally requested in the same part of the spec) were
dropped entirely and replaced with `ipm_debug_check()` (§7).

### Deviation B — signature and normalization

The spec's function signature was `(cam_x, cam_y, h, pitch_deg, yaw_deg, K)`
only — but building `M` needs `px_per_m`, `bev_w`, `bev_h`, which aren't
derivable from those. And `K` uses pixel-unit intrinsics, so `P*M` naturally
comes out in pixel units, while the shader's validity check
(`uv >= 0 && uv <= 1`) assumes normalized `[0,1]` UV.

Fix (per your direction): extend the signature with `img_w, img_h`
(passed explicitly by the caller, **not** recovered from `K.cx/cy` — the
caller already has the decoded frame size) plus `px_per_m, bev_w, bev_h`.
Normalization happens *inside* `H` (step 7 above) so the shader needs no
normalization logic of its own — just perspective-divide and use the result
directly.

---

## 6. The `gl_FragCoord.y` flip (flagged now, applies to PART 3)

`M`'s `py` convention is top-origin (`py=0` = far/top row, like a normal
image). GLES's `gl_FragCoord.y` is **bottom-origin** (`y=0` = bottom of the
window, increasing upward) — the opposite direction.

Verified with the standalone vertical-sweep test (§8): feeding `py` directly
from `gl_FragCoord.y` without correction would put "far" ground at the
*bottom* of the rendered image and invalid/behind-camera territory at the
*top* — backwards from the expected "car near the bottom, road recedes
toward the top" BEV look.

**Fix for PART 3** (not yet written): build the homogeneous BEV point in the
shader as
```glsl
vec3 p = vec3(gl_FragCoord.x, uBevHeight - gl_FragCoord.y, 1.0);
```
i.e. flip once before multiplying by `H`. This needs `uBevHeight` (`=bev_h`)
as a uniform, set once at `init_dual` time (not user-tunable at runtime).

---

## 7. `ipm_debug_check(H, bev_w, bev_h, label)` — `ipm.cpp:172`

Replaces the originally-requested `inverse(A)*A == identity` self-test
(which no longer makes sense without `inverse()`). Projects the BEV canvas
center through `H` and prints the resulting camera uv plus whether it lands
inside `[0,1]` — a fast way to catch a sign/axis mistake at startup as an
obvious "OUTSIDE image!" print instead of a silent black or garbled render.

Intended to be called once per camera right after building `H_left`/`H_right`
in `main.cpp` (PART 4, not yet wired up).

---

## 8. Verification performed so far

All done with a throwaway standalone build — `ipm.cpp` has zero GL/libcamera
dependencies, so it compiles fine on a plain dev machine:

```sh
g++ -std=c++17 -DIPM_STANDALONE_TEST ipm.cpp -o /tmp/ipm_test -lm && /tmp/ipm_test
```

**Test 1 — straight down, camera over world origin** (`pitch=-90, cam_x=cam_y=0`):
BEV center → image uv = **(0.500, 0.500)**, `z=1.2` (=height). Exactly the
principal point, as it must be for this degenerate case.

**Test 2 — default config** (`baseline=0.40, h=1.2, pitch=-30, yaw=0`):
left/right cameras give mirror-symmetric uv at the shared centerline
(`0.776` vs `0.224`) — confirms the left/right symmetry is correct.

**Test 3 — vertical centerline sweep** (left camera, `py` from top-origin 0
to `bev_h`): `z` (depth) starts positive and inside the image for small `py`
(far ground), crosses zero and goes negative (invalid/behind-camera) for
large `py` (near/behind the rig) — physically sensible monotonic behavior,
confirming no accidental sign flip in the projection.

---

## 9. Not yet done

- **PART 2**: `gpu_renderer.h/.cpp` — `set_ipm()`, `uH_L`/`uH_R` uniforms,
  `uPxPerM`, plus (new, needed to make the shader's blend and Y-flip work)
  `uBevWidth`/`uBevHeight` uniforms set once at `init_dual`.
- **PART 3**: `kFS_DUAL` rewrite — per-fragment homography warp, `okL`/`okR`
  validity, smoothstep blend. Blend `t` will be computed from the ground
  plane's world-`X` coordinate (recoverable from `gl_FragCoord.x`,
  `uPxPerM`, `uBevWidth`) since the two cameras are mounted symmetrically
  about `X=0` — this repurposes `uOverlap` as a world-meters half-width
  rather than the old fixed image-fraction, but keeps the same
  `set_stitch_overlap()`/`set_blend_edge()` API.
- **PART 4**: `main.cpp` CLI flags (`--baseline`, `--h`, `--pitch`, `--yaw`,
  `--cam-y`, `--px-per-m`) and wiring `build_intrinsics` +
  `ground_to_image_H` + `set_ipm()` + `ipm_debug_check()` into
  `run_dual_file_mode`. `CMakeLists.txt` needs `ipm.cpp` added to the
  executable's source list.

These all require a real GLES3/EGL/libcamera build (the RPi target), so
they can't be self-verified numerically the way PART 1 was above.
