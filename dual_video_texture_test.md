# `dual_video_texture_test.cpp` — What Was Built and Why

This document explains the Step 1 validation harness in `dual_video_texture_test.cpp`:
what each part does, why it was designed that way, and — most importantly — a real
bug that was found and fixed while verifying it actually works.

## 1. Goal

Prove that two pre-recorded `.mp4` files can be decoded independently and placed into
two separate GL textures, rendered side-by-side, and read back correctly — with no
hardcoded resolution, no warping/stitching (that's a later step), and no dependency on
the existing Stage 1-3 camera/EGL code. Everything lives in one self-contained file.

## 2. High-level flow

```
open_video(left) + open_video(right)          — FFmpeg: demux + find decoder + open codec
decode_next_rgba_frame() x2 (priming)         — decode frame 0 of each, learn real w/h/pix_fmt
setup_egl_headless()                          — GBM + EGL, no display/window needed
build_program() + build_quad_vao()            — compile shaders, upload a fullscreen quad
create_rgba_texture() x2 + create_color_fbo() — GL resources sized from *decoded* dimensions
loop:
    decode_next_rgba_frame() x2                — advance both videos by one frame
    update_texture() x2                        — glTexSubImage2D, reusing the texture object
    draw_textured_quad() into left/right half of the FBO
    every 30th frame: glReadPixels + stbi_write_png
close_video() x2, teardown_egl_headless()
```

## 3. Design decisions and why

### 3.1 Headless EGL via GBM (`setup_egl_headless`, lines 59-139)

- Opens `/dev/dri/renderD128` (a DRM **render node**) directly — no X11, no Wayland, no
  display attached, and no root privileges needed. This mirrors the style of the
  existing `egl_context.cpp` in this repo, but is reimplemented locally so this file has
  zero dependency on Stage 1-3 code, per the task's "standalone" constraint.
- Uses `eglGetPlatformDisplayEXT(EGL_PLATFORM_GBM_MESA, ...)` when available (the
  "correct" way to tell Mesa this is a GBM device), falling back to the older
  `eglGetDisplay` cast for older drivers.
- Checks for `EGL_KHR_surfaceless_context` explicitly and fails loudly if missing,
  rather than segfaulting later on `eglMakeCurrent(..., EGL_NO_SURFACE, ...)`.
- **Context version**: requests GLES **3.2** explicitly via the EGL 1.5 core tokens
  `EGL_CONTEXT_MAJOR_VERSION` / `EGL_CONTEXT_MINOR_VERSION` (not the older
  `EGL_CONTEXT_CLIENT_VERSION` used in the existing `egl_context.cpp`, which only asks
  for version `3` and happens to get 3.0). This is a deliberate deviation from the
  existing code's style, needed because the shaders here use `#version 320 es`.
- Every failure path prints a specific `[egl] ...` message with the hex EGL error code
  and returns `false` — nothing is allowed to silently segfault.

### 3.2 FFmpeg decode (`VideoStream`, `open_video`, `decode_next_rgba_frame`, lines 325-478)

- Uses the modern **push/pull decode API**: `avcodec_send_packet` /
  `avcodec_receive_frame`, not the deprecated `avcodec_decode_video2`.
- **Resolution is never hardcoded.** `open_video` only opens the demuxer/codec;
  `vs.width` / `vs.height` / `vs.pix_fmt` are set from the *first decoded frame's*
  `AVFrame::width/height/format` inside `decode_next_rgba_frame` (lines 451-453), not
  from stream metadata that could theoretically be stale or unset for some containers.
- **Priming pattern**: `main()` calls `decode_next_rgba_frame` once for each video
  *before* creating any GL texture (lines 512-515), specifically so the real decoded
  dimensions are known before `create_rgba_texture` runs. This also means frame 0's
  pixels are immediately available for the first texture upload — no wasted decode.
- **Infinite looping**: when `av_read_frame` returns EOF (or any read error), the code
  seeks back to the start (`av_seek_frame(..., AVSEEK_FLAG_BACKWARD)`) and calls
  `avcodec_flush_buffers` to reset decoder state, then continues the same loop
  (lines 422-428). This is what lets both videos "keep playing indefinitely" per the
  requirement, even if the two clips have different durations.
- `vs.dec_ctx->thread_count = 1` (line 397): FFmpeg's internal frame-threading is
  disabled. This was added for determinism while debugging (see §4) and kept because a
  validation harness benefits from simple, predictable single-threaded decode — there's
  no performance requirement here, unlike the live-camera path that comes later.
- Every FFmpeg call checks its return code and prints `av_strerror()`'s text via the
  `av_err_str()` helper (line 348), rather than assuming success.

### 3.3 GL texture upload (`create_rgba_texture`, `update_texture`, lines 270-294)

- Texture object is created **exactly once** per stream, sized to that stream's own
  decoded resolution, with `glTexImage2D(..., nullptr)` to just allocate storage
  (line 279). Every subsequent frame calls `glTexSubImage2D` (line 291) to reuse the
  same texture object — satisfying "do not recreate textures every frame."
- `update_texture` takes an explicit `row_stride_bytes` parameter and sets
  `GL_UNPACK_ROW_LENGTH` before the upload (line 290). This is not cosmetic — see the
  bug writeup in §4; the RGBA buffer's row stride is not guaranteed to equal
  `width * 4`, and using row length lets GL skip the pad bytes correctly instead of
  reading a shifted/skewed image.

### 3.4 Side-by-side FBO render (`create_color_fbo`, `draw_textured_quad`, lines 296-321, 537-570)

- FBO color attachment is `(left.width + right.width) x max(left.height, right.height)`
  — computed from the *actual* decoded sizes (line 542-543), not assumed to be equal.
- Rather than one shader doing both textures with a split computed in-shader (as the
  existing dual-camera stitch shader in `gpu_renderer.cpp` does), this harness draws
  the **same trivial pass-through quad twice**, once per texture, each time with
  `glViewport` restricted to that texture's half of the FBO (lines 569-570). Clip space
  `[-1,1]` always maps to "the current viewport," so a single hardcoded fullscreen quad
  correctly fills whichever sub-rectangle is active — no shader-side coordinate math
  needed for "step 1, just place them side by side."
- **Orientation**: the quad's vertex data maps NDC top (`y=+1`) to texcoord `v=0`
  (lines 241-247), i.e. the *first* row of the decoded/uploaded pixel buffer is drawn at
  the top of the viewport. Combined with `stbi_flip_vertically_on_write(1)` (line 553)
  — which is needed anyway because `glReadPixels` always returns bottom-up rows — the
  final PNG ends up right-side-up with no separate Y-flip logic required in the shader.
  This was verified visually (§5), not just reasoned about.

### 3.5 Snapshotting (lines 572-592)

- Every 30th frame: `glFinish()` (ensure rendering is actually done before reading, not
  just queued) → `glReadPixels` on the whole FBO → `stbi_write_png`. `glReadPixels`'s
  return is checked via `check_gl_error` (line 576) rather than assumed to succeed.
- Exactly 5 snapshots (`kNumSnapshots`), then the loop exits and everything is torn down
  in reverse-acquisition order (GL objects → EGL/GBM → FFmpeg contexts, lines 597-607).

### 3.6 stb_image_write

Fetched the single-header library from the canonical upstream
(`github.com/nothings/stb/master/stb_image_write.h`) into `extern/stb_image_write.h`,
included with `STB_IMAGE_WRITE_IMPLEMENTATION` defined once in this translation unit
(lines 27-28), per the requirement to use it for PNG output instead of pulling in
`libpng` (which the existing `gpu_renderer.cpp` uses, but that's more setup than a
single-file test harness needs).

## 4. A real bug found while verifying: heap corruption in `sws_scale`

The task explicitly asked for real verification, not just "it compiles." Testing with
two different-resolution synthetic videos (320x240 and 200x150, generated via
`ffmpeg -f lavfi -i testsrc...`) reliably crashed with:

```
malloc(): corrupted size vs. prev_size / corrupted top size
```

### Why this happened

The first version of this code allocated the RGBA scratch buffer as a plain
`std::vector<uint8_t>` sized to exactly `width * height * 4` bytes, and passed
`width * 4` as `sws_scale`'s destination stride. This is the "obvious" way to size the
buffer, and it worked fine for the 320x240 stream. It silently corrupted the heap for
the 200x150 stream.

Root cause: `sws_scale`'s SIMD-optimized inner loops process pixels in fixed-size
blocks (e.g. 16-row blocks for certain vertical filters). When a frame's height (or
width) isn't a multiple of that block size, the optimized path can write a few bytes
**past** the exact `height * stride` boundary of the output buffer. 240 is a multiple
of 16, so the 320x240 stream never triggered it. 150 is *not* a multiple of 16
(150 / 16 = 9.375), so the write past the tightly-sized buffer corrupted adjacent heap
metadata. This is a known, documented FFmpeg gotcha — the API expects callers to use
`av_image_alloc` (or otherwise over-allocate/pad), not a size computed by hand from
`width * height * bytes_per_pixel`.

### How it was diagnosed

This was not "AI vibes" debugging — it was root-caused with tools:

1. First reproduced consistently (3/3 runs) with `-O2`, but the crash **vanished**
   under AddressSanitizer at both `-O0` and `-O2`. That mismatch was the key clue: a
   real out-of-bounds write should be caught by ASan regardless of optimization level.
   ASan's redzone-padded allocator happened to absorb the small overflow harmlessly,
   while glibc's tightly-packed malloc chunks got their header corrupted by the same
   write — different allocators, same bug, different symptom.
2. Ruled out the GL/EGL/GBM code entirely by extracting a minimal EGL-only
   reproduction — ran clean 3/3 times, proving the bug wasn't in headless context setup.
3. Ruled out multithreaded decode as the cause by isolating a pure-FFmpeg-only
   reproduction (no GL at all) — still crashed, and setting `thread_count = 1` didn't
   fix it either, so it wasn't a decoder-thread race.
4. Bisected further: decoding *only* the 320x240 video ran clean; decoding *only* the
   200x150 video crashed on its own, right after `sws_scale`. That isolated the bug to
   the interaction between `sws_scale` and that specific resolution.
5. Connected 150 not being a multiple of 16 to FFmpeg's known padding requirement for
   `sws_scale` destination buffers, and switched to `av_image_alloc`, which added a
   `struct VideoStream` and confirmed 5/5 clean runs afterward.

### The fix

- Replaced `std::vector<uint8_t> rgba` with `uint8_t *rgba_data[4]` /
  `int rgba_linesize[4]` allocated via `av_image_alloc(..., AV_PIX_FMT_RGBA, 32)`
  (line 344-345, 455-461), which pads both row stride and total buffer size to a safe
  alignment.
- `sws_scale` now writes into `vs.rgba_data` / `vs.rgba_linesize` directly instead of a
  hand-rolled destination array (lines 472-473).
- `update_texture` was changed to take the real stride and apply it via
  `GL_UNPACK_ROW_LENGTH` (line 290) instead of assuming `stride == width * 4` — because
  after this fix, it usually isn't (the allocator may pad it).
- `close_video` frees the buffer with `av_freep(&vs.rgba_data[0])` (line 482).

This is the single most important thing to take away from this session: **the same
code can look completely correct, compile without warnings, and even run "fine" for
one input resolution while corrupting memory for another** — it only surfaced because
the harness was tested with two *differently-sized* videos, which the task's
"read the actual decoded width/height, don't hardcode" requirement made a natural thing
to test.

## 5. Verification performed

- Compiled clean with `-Wall -Wextra` (no warnings from this file's own code).
- Linked and ran on this dev machine's AMD Vega GPU via its DRM render node (the actual
  target is RPi5/V3D, but the code paths — GBM, EGL, GLESv2 — are portable; only the
  renderer name printed at startup will differ).
- Ran 5 consecutive times after the fix with **zero crashes**, each producing exactly
  5 PNGs.
- Visually inspected `sidebyside_0000.png`, `sidebyside_0060.png`, `sidebyside_0120.png`:
  - Left half: FFmpeg `testsrc` color bars, correctly oriented, static pattern as
    expected (not distorted, no R/B channel swap).
  - Right half: FFmpeg `testsrc2` pattern showing an on-screen decoded timestamp and
    frame counter that visibly advances — `00:00:00.000` / counter `0` at frame 0,
    `00:00:02.000` / `60` at frame 60, `00:00:04.000` / `120` at frame 120 — proving
    both streams decode and update **independently**, frame-accurately, with correct
    colors and orientation.

## 6. Build

```bash
g++ -std=c++17 -O2 -Wall -Wextra dual_video_texture_test.cpp -o dual_video_texture_test \
  $(pkg-config --cflags egl gbm glesv2 libavformat libavcodec libavutil libswscale) \
  $(pkg-config --libs   egl gbm glesv2 libavformat libavcodec libavutil libswscale) -lm

./dual_video_texture_test left.mp4 right.mp4
```

A plain `g++` command was used instead of adding a second `CMakeLists.txt` so this
standalone test wouldn't collide with or need to be wired into the existing project's
build system, keeping it a true drop-in single-file harness as requested.
