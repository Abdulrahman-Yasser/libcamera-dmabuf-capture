# libcamera-dmabuf-capture

A zero-copy GPU camera pipeline written as part of the **AGL AI Backup Camera GSoC project**.

The goal is to prove that a camera frame captured via libcamera on a Raspberry Pi 4 running
AGL can travel from sensor to a rendered, shader-processed image entirely on the GPU —
with the CPU acting only as a traffic controller and never touching a single pixel.

---

## Table of Contents

1. [What it does](#what-it-does)
2. [Pipeline Architecture](#pipeline-architecture)
3. [Project Evolution](#project-evolution)
4. [Component Breakdown](#component-breakdown)
5. [Key Design Decisions](#key-design-decisions)
6. [Performance Results](#performance-results)
7. [Build](#build)
8. [Deploy and Run](#deploy-and-run)
9. [Controls](#controls)
10. [What Comes Next](#what-comes-next)

---

## What it does

- Opens the IMX219 camera via libcamera on AGL/RPi4
- Waits 30 frames for Auto-Exposure and Auto-White-Balance to settle
- Runs a continuous 30fps render loop — each frame:
  - Takes the camera DMA-BUF fd (zero-copy, no memcpy)
  - Imports it into OpenGL ES as an external texture via EGL
  - Runs a GLSL ES shader: NV12→RGB conversion + red tint + parking grid overlay
  - Renders into an FBO — GPU output stays on GPU
- Prints FPS, CPU render time, and RSS every second
- Press `s` to save a PNG snapshot at any time (only then does data touch CPU RAM)
- Press Ctrl+C for a clean shutdown with a full performance report

---

## Pipeline Architecture

```text
┌─────────────────────────────────────────────────────────────────────┐
│                         RPi4 (BCM2711)                              │
│                                                                     │
│  IMX219 sensor                                                      │
│      │ CSI-2 (raw Bayer)                                           │
│      ▼                                                              │
│  Unicam (camera interface) ──► ISP (vc4 pipeline)                  │
│                                    │ NV12 output                    │
│                                    ▼                                │
│                            ┌──────────────┐                        │
│                            │  DMA-BUF fd  │  ← shared memory fd   │
│                            └──────┬───────┘                        │
│                                   │  eglCreateImageKHR()           │
│                                   │  (zero-copy: fd only, no copy) │
│                                   ▼                                 │
│                            ┌──────────────┐                        │
│                            │  EGLImageKHR │  ← GPU import handle   │
│                            └──────┬───────┘                        │
│                                   │  glEGLImageTargetTexture2DOES() │
│                                   ▼                                 │
│                      ┌────────────────────────┐                    │
│                      │ GL_TEXTURE_EXTERNAL_OES │ ← GPU texture     │
│                      └────────────┬───────────┘                    │
│                                   │  samplerExternalOES             │
│                                   ▼                                 │
│                      ┌────────────────────────┐                    │
│  V3D 4.2 GPU         │   GLSL ES 3.1 Shader   │                    │
│  8 QPUs @ 516 MHz    │  • NV12 → RGB (HW)     │                    │
│  No L3 cache         │  • Red tint overlay    │                    │
│                      │  • Parking grid lines  │                    │
│                      └────────────┬───────────┘                    │
│                                   │                                 │
│                                   ▼                                 │
│                            ┌──────────────┐                        │
│                            │  FBO (RGBA8) │  ← rendered frame      │
│                            └─────────────-┘    stays on GPU        │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

At no point in the steady-state loop does a pixel value cross from GPU memory to CPU RAM.
The only time CPU RAM is touched is when `s` is pressed — `glReadPixels` pulls the FBO
to RAM for PNG encoding. This is intentional and on-demand only.

---

## Project Evolution

This project went through four major stages:

### Stage 1 — Single-shot capture (original)

Captured one frame, saved raw NV12 to `/tmp/frame.raw`, imported into EGL, ran one shader
pass, saved PNG via `glReadPixels`. Purpose: prove the DMA-BUF chain works at all.

**Problem:** Every run paid full JIT shader compile cost (~460ms), EGL import cost, and
`glReadPixels` readback cost — none of which reflects real pipeline performance.

### Stage 2 — Performance instrumentation

Added CPU-side phase timing (`clock_gettime`), GPU timer queries
(`GL_EXT_disjoint_timer_query`), a benchmark loop (`BENCH_FRAMES` env var), and a
`SAVE_PNG` env var to gate the readback. Purpose: measure where time actually goes.

**Finding:** GPU render was ~11ms, EGL import was 0.14ms (proves zero-copy), shader
JIT was a one-time ~460ms cost. CPU was ~7% utilized — the GPU was doing all the work.

### Stage 3 — EGLImage pre-caching (mentor's advice)

The original code called `eglCreateImageKHR()` every frame — expensive. The mentor pointed
out that the DMA-BUF fd→EGLImage binding is stable for the lifetime of the libcamera
buffer pool.

**Fix:** At init, loop over all buffers in the pool and call `eglCreateImageKHR()` once per
buffer. Store the result in a `std::unordered_map<int, FrameGLResources>` keyed by fd.
`render_frame()` then does a cheap map lookup — zero EGL calls per frame.

### Stage 4 — Continuous render loop (current)

Refactored from single-shot to a continuous pipeline:

- `GpuRenderer` class: init once (shader, FBO, EGLImage cache), render per frame, cleanup once
- `CaptureSession` refactored: producer/consumer queue with condition variables,
  separate warmup phase, `nextFrame()` blocks until frame ready
- Main loop: SIGINT/SIGTERM handling, all 4 buffers queued at startup, FPS counter,
  keyboard snapshot trigger, full performance report on exit

---

## Component Breakdown

### `egl_context.cpp` / `egl_context.h`

Sets up a **headless** (no display server) EGL context on the RPi4's V3D GPU.

- Opens `/dev/dri/renderD128` (the DRM render node — no root needed, no Wayland needed)
- Creates a GBM device wrapping the DRM fd (Mesa needs GBM to know this is a GPU, not an X11 display)
- Initialises EGL 1.5, binds OpenGL ES API, creates a GLES 3.1 context
- Uses `EGL_KHR_surfaceless_context` — renders to an FBO, not to a window
- Checks required extensions: `EGL_EXT_image_dma_buf_import`, `EGL_KHR_image_base`,
  `GL_OES_EGL_image`, `GL_OES_EGL_image_external`
- Checks optional extension: `GL_EXT_disjoint_timer_query` (GPU timer queries)

### `capture_session.cpp` / `capture_session.h`

Wraps the libcamera capture pipeline as a thread-safe producer/consumer queue.

**Warmup phase (frames 1–30):**
libcamera's AE and AWB algorithms need several frames to converge on correct
exposure and white balance. During warmup, `requestCompleted()` silently requeues
each request. On frame 30, it signals the main thread via a condition variable.

**Render phase (frames 31+):**
Each completed frame is pushed onto a `std::queue` protected by a `std::mutex`.
The main thread calls `nextFrame()` which blocks on a `std::condition_variable`
until a frame is ready.

**`stop()`** sets `running_ = false` and calls `cv_.notify_all()` to unblock
`nextFrame()` so the main loop can exit cleanly.

### `gpu_renderer.cpp` / `gpu_renderer.h`

The core GPU pipeline class. Lives for the entire process lifetime.

**`init()`:**
1. Loads EGL/GL extension function pointers (`eglCreateImageKHR`, `glEGLImageTargetTexture2DOES`,
   timer query functions)
2. Compiles and links the GLSL ES 3.1 shader once (~460ms one-time JIT cost)
3. Creates one FBO + renderbuffer (the render target)
4. Loops over the libcamera buffer pool and calls `eglCreateImageKHR()` **once per buffer**,
   caches the result in `fd_cache_` (an `unordered_map<int, FrameGLResources>`)
5. Creates one GPU timer query object for performance measurement

**`render_frame()`:**
1. Reads the **previous** frame's GPU timer query result (non-blocking — at 30fps the GPU
   finishes in ~6ms, the next frame arrives 33ms later, so the result is always ready)
2. Updates GPU stats (min/max/avg GPU time)
3. Looks up the pre-cached texture for this frame's DMA-BUF fd (O(1) map lookup)
4. Binds the texture and begins the GPU timer query
5. Calls `glDrawArrays(GL_TRIANGLES, 0, 3)` — the fullscreen triangle trick
6. Ends the timer query, calls `glFlush()` (submits GPU command buffer, returns immediately)

**`save_snapshot()`:**
Called only when `s` is pressed. Calls `glFinish()` (waits for GPU), then `glReadPixels()`
(pulls RGBA frame to CPU RAM), then encodes to PNG via libpng. This is the only CPU readback
in the entire pipeline.

**`cleanup()`:**
Destroys all GPU objects in order: timer query → textures → EGLImages → FBO → renderbuffer → shader program.

### `main.cpp`

The top-level orchestrator.

- Installs `SIGINT`/`SIGTERM` handlers (`g_running = 0`)
- Sets terminal to **raw mode** (keypresses without Enter), stdin to non-blocking
- Allocates all 4 camera buffers, queues all 4 requests at startup (keeps camera always full)
- Calls `session.waitWarmupDone()` (blocks until AE/AWB settled)
- Runs the render loop: `nextFrame()` → `render_frame()` → keyboard check → `req->reuse()` → `queueRequest()`
- Tracks per-frame wall-clock time (min/max/avg), CPU render time, RSS
- Prints FPS + stats every second
- On Ctrl+C: prints full performance report with profile, CPU load, GPU utilization,
  memory bandwidth, sysmem RSS

### `perf_timer.h`

Single inline function `now_ms()` using `CLOCK_MONOTONIC`. Used across multiple
compilation units without requiring a separate `.cpp`.

### `dmabuf_import.cpp` / `dmabuf_import.h`

Original single-shot import and PNG save function. Kept for debug use; not called
from the continuous loop. Contains `import_and_save_png()` which does the full
single-frame EGL import → shader → `glReadPixels` → PNG path.

---

## Key Design Decisions

### 1. DMA-BUF zero-copy vs alternatives

**What we chose:** `EGL_EXT_image_dma_buf_import` — import the camera's DMA-BUF fd
directly into EGL as an `EGLImageKHR`, then bind it as a GL texture. The GPU reads
the camera frame directly from the buffer the ISP wrote into. Zero copies.

**Alternative A — `mmap()` + `glTexImage2D()`:**
`mmap()` the DMA-BUF into CPU virtual address space, copy pixels into a GL texture
with `glTexImage2D()`. This requires a full memcpy of the frame (2.89 MB at 1640×1232)
on every single frame. At 30fps that is 87 MB/s of wasted CPU→GPU copies.
The EGL import time of 0.14ms vs a memcpy time of 10–50ms proves the difference.

**Alternative B — V4L2 `read()`:**
Use the V4L2 API with `read()` syscall instead of libcamera. This forces a kernel→user
copy on every frame. Bypasses the zero-copy DMA-BUF path entirely. Also loses
libcamera's ISP pipeline (no AE/AWB/debayer).

**Why DMA-BUF wins:** The camera ISP writes directly into a buffer that is simultaneously
accessible to the GPU. No CPU involvement in the data path at all.
`page-faults: 0` in `perf stat` confirms this at runtime.

---

### 2. `samplerExternalOES` vs manual NV12 shader math

**What we chose:** `GL_TEXTURE_EXTERNAL_OES` with `samplerExternalOES` in the shader.
One `texture()` call in GLSL samples the NV12 external texture. Mesa's V3D driver
performs the YUV→RGB conversion **in the texture sampler hardware** — it is not computed
in shader ALU instructions.

**Alternative — manual two-plane sampling:**
Import Y and UV planes as separate `GL_TEXTURE_2D` textures (`DRM_FORMAT_R8` and
`DRM_FORMAT_RG88`), then write the BT.601 matrix multiplication in the fragment shader:

```glsl
float y  = texture(uY,  uv).r - 0.0625;
float cb = texture(uUV, uv).r - 0.5;
float cr = texture(uUV, uv).g - 0.5;
vec3 rgb = mat3(1.164,  0.0,    1.596,
                1.164, -0.391, -0.813,
                1.164,  2.018,  0.0  ) * vec3(y, cb, cr);
```

This works but costs extra ALU cycles per pixel (matrix multiply), two texture fetches
per fragment instead of one, and two separate EGL image imports.

**Why `samplerExternalOES` wins:** The hardware sampler path is free. The V3D TMU
(Texture Memory Unit) does the conversion before the QPU (shader core) even sees the
pixel value. Fewer shader instructions, fewer texture fetches, single EGL import.

---

### 3. EGLImage pre-caching vs per-frame creation

**What we chose:** At `init()`, call `eglCreateImageKHR()` once per buffer in the
libcamera pool. Store `fd → {EGLImageKHR, GLuint texture}` in an `unordered_map`.
Each `render_frame()` call does only a map lookup — zero EGL calls per frame.

**Alternative — create EGLImage every frame:**
Call `eglCreateImageKHR()` at the start of `render_frame()`, use it, then
`eglDestroyImageKHR()`. This was the original approach.

**Why pre-caching wins:** `eglCreateImageKHR()` is not cheap. The driver must
validate the DMA-BUF fd, query its format, map it into the GPU's address space,
and create internal tracking structures. This added measurable overhead per frame.
The DMA-BUF fd is stable for the entire lifetime of the libcamera buffer pool
(libcamera allocates buffers once and reuses them), so there is no reason to
recreate the EGLImage. The mentor specifically identified this as a correctness
and performance issue.

---

### 4. `glFlush()` vs `glFinish()` in the render loop

**What we chose:** `glFlush()` in `render_frame()`. This submits the GPU command
buffer to the hardware and returns **immediately** — the CPU does not wait for the
GPU to finish rendering.

**Alternative — `glFinish()`:**
`glFinish()` blocks the CPU until the GPU has completed all submitted commands.
Used in `save_snapshot()` (required before `glReadPixels`) and in benchmark mode.

**Why `glFlush()` wins for the render loop:**
The camera delivers frames at 15–30fps (33–67ms intervals). The GPU renders each frame
in ~6–11ms. By using `glFlush()`, the CPU is free to immediately requeue the camera
buffer and call `nextFrame()`. The GPU renders in parallel. When the next camera frame
arrives (33ms later), the GPU has long finished (6ms). Natural camera pacing provides
the synchronization — no explicit CPU stall is needed.

Using `glFinish()` in the loop would add a 6–11ms CPU stall per frame for no benefit,
increasing CPU utilization and latency.

---

### 5. Headless EGL/GBM vs display server

**What we chose:** Open `/dev/dri/renderD128`, create a GBM device, initialize EGL
with `EGL_PLATFORM_GBM_MESA`, create a surfaceless context
(`EGL_KHR_surfaceless_context`). No window system, no Wayland, no X11.

**Alternative — Wayland/Weston surface:**
Connect to AGL's Weston compositor, create a Wayland surface, attach an EGL window
surface to it. The GPU renders directly to a compositor surface.

**Why headless wins for this stage:**
This project is a pipeline test — the output destination (Flutter texture, Wayland
surface, V4L2 encoder) is a separate concern. Headless EGL lets us validate the
camera→GPU path in isolation without depending on the compositor being up, without
needing display allocation, and without Wayland protocol overhead. The rendered FBO
can be handed to any consumer later.

---

### 6. 4-buffer pool — all queued at startup

**What we chose:** Allocate however many buffers libcamera gives us (typically 4),
create one `Request` per buffer, queue **all of them** at startup before entering
the render loop.

**Alternative — single buffer:**
Use one buffer, wait for it to complete, render, requeue, repeat.

**Why 4 buffers wins:**
With a single buffer the camera ISP must wait for the GPU render and requeue to
complete before it can write the next frame — pipeline stall. With 4 buffers queued,
the ISP always has an empty buffer waiting. While the GPU renders frame N, the ISP is
already capturing frame N+1 into another buffer. True pipeline parallelism, no stalls.

---

### 7. `FrameDurationLimits` to achieve 30fps

**What we chose:** Pass `FrameDurationLimits = {33333, 33333}` (µs) to `camera->start()`.
This clamps the minimum and maximum frame duration to exactly 33.333ms = 30fps.

**Why it was needed:**
By default libcamera's AE algorithm allows exposures up to 66ms (1/15s) to handle
low-light environments. Even though the IMX219 sensor at 1280×720 is physically capable
of 120fps, AE was holding the frame duration to 66ms (15fps) to maximize exposure time
indoors. The sensor capability is not the bottleneck — the AE policy is.

**Alternative — disable AE entirely:**
```cpp
controls.set(libcamera::controls::AeEnable, false);
controls.set(libcamera::controls::ExposureTime, 25000); // 25ms fixed
```
This works but gives fixed exposure — the image will be over- or under-exposed when
lighting changes. `FrameDurationLimits` keeps AE active but caps the maximum exposure
time to 33ms, so AE can still adapt within that budget.

---

### 8. GPU timer queries (`GL_EXT_disjoint_timer_query`)

**What we chose:** Load `glBeginQueryEXT`/`glEndQueryEXT`/`glGetQueryObjectui64vEXT`
at init (optional — no crash if extension missing). Wrap `glDrawArrays()` with
`glBeginQueryEXT(GL_TIME_ELAPSED_EXT)`. Read the result at the **start of the next
frame** (single-buffered, non-blocking — GPU finishes in 6ms, next frame arrives 33ms
later, result is always ready).

**Alternative — CPU wall clock around `glFinish()`:**
Wrap `glDrawArrays()` + `glFinish()` with `clock_gettime()`. This measures real time
but introduces a CPU stall per frame and includes driver overhead outside the GPU.

**Why GPU timer wins:**
`GL_EXT_disjoint_timer_query` measures **true GPU execution time** in nanoseconds as
counted by the GPU's own clock. It excludes CPU overhead, driver latency, and queue
wait time. At 30fps with no `glFinish()` stall, it gives accurate GPU utilization
numbers without perturbing the pipeline.

---

### 9. Terminal raw mode for keyboard input

**What we chose:** Set `STDIN_FILENO` to raw mode (`tcsetattr` with `~(ICANON|ECHO)`)
and non-blocking (`fcntl` with `O_NONBLOCK`). After each `render_frame()`, call
`read(STDIN_FILENO, &key, 1)`. If `s` is read, save snapshot. Cost when no key
is pressed: one failed `read()` syscall — microseconds.

**Alternative — separate keyboard thread:**
Spawn a `std::thread` that blocks on `getchar()` and sets an `std::atomic<bool>`.
Main loop checks the atomic flag.

**Why raw mode wins:**
A separate thread adds complexity (thread lifecycle, shared state, join on exit) for
no benefit. The main loop is already woken every 33ms by the camera frame — keyboard
latency of one frame (33ms) is imperceptible for a snapshot trigger. Non-blocking
`read()` in the main loop is simpler and correct.

---

## Performance Results

### Hardware

| Component | Detail |
|---|---|
| SoC | Broadcom BCM2711 (RPi4) |
| CPU | ARM Cortex-A72 @ 1500 MHz (4 cores) |
| GPU | V3D 4.2 — 8 QPUs, 2 slices, 2 TMUs |
| GPU clock | 251 MHz idle → 516 MHz under load |
| L3 cache | **None** (L3C: no, 0 KB) |
| DRAM | Unified Memory Architecture — CPU and GPU share DRAM |
| Camera | Sony IMX219 (RPi Camera Module 2) |
| OS | AGL (Automotive Grade Linux), kernel 6.12 |
| Mesa | 25.1.6 |

The absence of L3 cache is significant: every texture fetch that misses the per-TMU cache
goes directly to DRAM. This makes memory bandwidth the primary GPU performance constraint.

---

### Configuration Comparison

| Metric | 1280×720 @ 15fps | 1280×720 @ 30fps | 1640×1232 @ 15fps |
|---|---|---|---|
| **FrameDurationLimits** | not set | 33333 µs | not set |
| **CPU utilized** | 0.074 cores | 0.126 cores | 0.074 cores |
| **Context switches** | ~2,200 /10s | 3,838 /10s | ~1,961 /10s |
| **CPU cycles** | 428M /10s | 850M /10s | 428M /10s |
| **Page faults** | 0 | 0 | 0 |
| **GPU clock** | 516 MHz | 516 MHz | 502 MHz |
| **GPU BOs** | 25 objects | 25 objects | 25 objects |
| **GPU BO size** | 14.8 MB | 14.8 MB | **31.1 MB** |
| **vmstat interrupts/s** | ~900 | ~1,700 | ~900 |
| **VmRSS** | 28,100 kB | 28,632 kB | 28,100 kB |
| **Theo. bandwidth** | 72.6 MB/s | 145.2 MB/s | **159.0 MB/s** |

App output at 1280×720 @ 30fps:

```text
[perf] 30.0 fps | render_cpu avg 0.493 ms | RSS 28632 kB | frames 4976
```

---

### Analysis

**Zero-copy verified across all three configurations:**
`page-faults: 0` in every `perf stat` run. No new memory is touched cold at runtime.
All buffers are mapped and resident from init. A pipeline with CPU copies would show
thousands of page faults per second from new `mmap()` regions.

**CPU scales linearly with frame rate, not with resolution:**
Going from 15fps → 30fps at 1280×720 doubles CPU usage (7.4% → 12.6%).
Going from 1280×720 → 1640×1232 at the same 15fps: CPU stays identical (7.4%).
The CPU does no pixel work — it only handles camera callbacks and mutex operations,
which scale with frame rate, not frame size.

**GPU memory scales with resolution, not frame rate:**
14.8 MB at 720p vs 31.1 MB at 1232p (2.1× more). The count of BOs stays at 25 —
same number of objects, just larger. This reflects the actual frame sizes:
- 4× NV12 camera buffers at 720p: 4 × 1.32MB = 5.28 MB
- 4× NV12 camera buffers at 1232p: 4 × 2.89MB = 11.56 MB
- RGBA FBO at 720p: 3.52 MB
- RGBA FBO at 1232p: 7.71 MB

**GPU clock drops slightly at 1640×1232 (502 vs 516 MHz):**
At 15fps the GPU renders each frame in ~11ms then sits idle for ~55ms. The devfreq
governor has time to ramp the clock back down during those idle windows. At 30fps
the idle window is only ~22ms — the governor keeps the clock higher.

**1640×1232 @ 15fps uses MORE memory bandwidth than 1280×720 @ 30fps:**

```text
1280×720  @ 30fps: (1.32 + 3.52) MB × 30 = 145.2 MB/s
1640×1232 @ 15fps: (2.89 + 7.71) MB × 15 = 159.0 MB/s
```
The larger frame more than offsets the lower frame rate. For a backup camera,
**1280×720 @ 30fps is the better choice** — lower DRAM bandwidth, smoother motion,
smaller GPU memory footprint.

**render_frame() CPU overhead — 0.493ms per frame:**
This is the cost of: GL state machine calls (`glBindTexture`, `glDrawArrays`),
GPU timer query result retrieval, `glFlush()`, and the fd→texture map lookup.
At 33.3ms per frame, this is **1.48% of the frame budget** — the GPU does the rest.

---

## Build

The AGL image has no compiler. Cross-compile from the host using the Yocto SDK:

```bash
source /opt/agl-sdk/<version>-aarch64/environment-setup-aarch64-agl-linux
mkdir -p build && cd build
cmake -DCMAKE_TOOLCHAIN_FILE="$CMAKE_TOOLCHAIN_FILE" ..
make -j$(nproc)
```

The binary is at `build/libcamera-dmabuf-capture`.

**Dependencies (resolved by pkg-config via the SDK sysroot):**
- `libcamera` — camera pipeline
- `libdrm` — DRM fourcc constants for EGL image attributes
- `gbm` — GBM device for headless EGL
- `egl` — EGL 1.5
- `glesv2` — OpenGL ES 3.1
- `libpng` — PNG encoding for snapshots

---

## Deploy and Run

```bash
# Copy to Pi
scp build/libcamera-dmabuf-capture root@<rpi4-ip>:/home/measure_app/

# Run (30fps mode)
ssh root@<rpi4-ip> /home/measure_app/libcamera-dmabuf-capture
```

Expected startup output:

```text
[egl] opened /dev/dri/renderD128 (fd=3)
[egl] GL renderer : V3D 4.2.14.0
[egl] GL version  : OpenGL ES 3.1 Mesa 25.1.6
[egl] Extension check:
  [OK]  EGL_EXT_image_dma_buf_import
  [OK]  GL_EXT_disjoint_timer_query
[capture] camera: /base/soc/i2c0mux/i2c@1/imx219@10
[capture] 4 buffer(s) allocated
[gpu] shader compile+link : 464.8 ms (one-time)
[gpu] cached fd=19 → EGLImage+texture
[gpu] cached fd=20 → EGLImage+texture
[gpu] cached fd=21 → EGLImage+texture
[gpu] cached fd=22 → EGLImage+texture
[gpu] renderer ready: 1280x720, 4 buffer(s) cached
[capture] warming up AE/AWB (30 frames)...
[capture] warmup done — entering render loop
[loop] running — press 's' to save snapshot, Ctrl+C to stop
[perf] 30.0 fps | render_cpu avg 0.493 ms | RSS 28632 kB | frames 30
```

### Changing resolution / frame rate

In [main.cpp](main.cpp):

```cpp
// Resolution
sc.size = {1280, 720};   // 30fps with FrameDurationLimits
sc.size = {1640, 1232};  // 15fps (AE-limited), larger FOV

// Frame rate control — in camera->start() call:
int64_t fdl[2] = { 33333LL, 33333LL };  // 30fps
// Remove FrameDurationLimits entirely → AE chooses (typically 15fps indoors)
```

### System-level performance monitoring (while app runs)

```bash
PID=$(pgrep -f libcamera-dmabuf-capture)

# GPU clock (scales from 251 MHz idle to 516 MHz under load)
cat /sys/kernel/debug/dri/fec00000.v3d/measure_clock

# GPU memory allocations (BOs)
cat /sys/kernel/debug/dri/fec00000.v3d/bo_stats

# Confirm app is using the GPU
cat /sys/kernel/debug/dri/fec00000.v3d/clients

# CPU hardware counters (10s sample)
perf stat -p $PID sleep 10

# Memory bandwidth + CPU idle
vmstat 1 10

# Process RSS
grep -E "VmRSS|VmSize|RssAnon" /proc/$PID/status
```

---

## Controls

| Key / Signal | Action |
|---|---|
| `s` | Save current FBO frame as `/tmp/snapshot_NNN.png` |
| `Ctrl+C` | Clean shutdown + print full performance report |
| `SIGTERM` | Same as Ctrl+C |

The snapshot is the **only** time `glReadPixels` is called. In normal operation
no pixel data ever leaves the GPU.

---

## What Comes Next

The rendered frame currently lives in the GPU FBO and goes nowhere visible. The next
step is to feed it to a Flutter application running on AGL's Weston compositor.

**Flutter texture plugin approach:**
Flutter on Linux exposes a texture registry API (`FlTextureGL`). Native code registers
a GL texture ID with Flutter's engine; Flutter's compositor samples it directly as a
`Texture()` widget — no `glReadPixels`, no CPU copy.

The required change is small: replace the `GL_RENDERBUFFER` FBO color attachment with
a `GL_TEXTURE_2D`, expose the texture ID, and call `markTextureFrameAvailable()` after
each `render_frame()`. The EGL context used by our renderer must be created as a shared
context with Flutter's EGL context so the texture is visible across both.

```text
Camera → DMA-BUF → EGL texture (shader: NV12→RGB + overlay)
                        ↓  GL texture ID (same GPU VRAM)
               Flutter texture registry
                        ↓  Texture() widget
               Flutter UI (overlays, alerts)
                        ↓  Weston compositor
                      Display
```

End-to-end zero-copy is maintained: camera DMA-BUF → GPU shader → Flutter compositor →
display, with no CPU readback at any stage.
