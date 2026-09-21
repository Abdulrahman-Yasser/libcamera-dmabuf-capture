# bev_view_example

A full-screen `BevView` — the capture pipeline running as an **ivi-homescreen
platform view** — with a stats overlay and the runtime controls the CLI binds to
keys. It is the harness the `bev_view` package is developed against: it
exercises the creation params, the dma-buf submit path on each shell backend,
and the `bev_view_*` controls end to end.

The view is a pure producer. `libbev_view.so` renders the pipeline offscreen on
its own thread and EGL context, hands the shell one dma-buf per frame through
`ihs_pv_submit`, and the shell owns compositing, placement and z-order. There is
no Flutter texture and no Linux desktop runner — the shell is the runner.

## Running

```bash
./run.sh                                   # wayland-egl, test pattern
BACKEND=wayland-vulkan ./run.sh            # same view on the Vulkan backend
BEV_MODE=camera BACKEND=drm-kms-egl ./run.sh
BEV_MODE=surround BEV_SRC=front_l.hevc:front_r.hevc \
  BEV_CONFIG=/path/to/bev_config.ini BEV_BLEND=pyramid ./run.sh
```

`run.sh` builds the bundle (the native-assets hook compiles `libbev_view.so`
from the repository's CMake project), assembles an ivi-layout bundle beside it,
and launches the shell. Host paths (`IHS_DIR`, `IHS_BUILD`, `ENGINE_DIR`,
`FLUTTER`) are all overridable — see the header of the script.

The ivi-homescreen build must include `libihs_shared` and the platform-view
host. The hook finds a checkout beside this repository on its own; point it
elsewhere with the `user_defines` block commented in `pubspec.yaml`.

### The SDK and the engine have to match

`flutter build bundle` emits a JIT kernel, so the shell needs a **debug** engine
bundle built from the same engine revision as the Flutter SDK that built it. A
mismatch fails at launch with

```text
Dart Error: Can't load Kernel binary: Invalid kernel binary format version (expected 130, found 140)
```

which looks like an app failure and is only a mismatched pair. `run.sh` prefers
an engine whose embedded revision equals the SDK's `bin/cache/engine.stamp`, and
warns when it has to settle for another. Override either side:

```bash
FLUTTER=/path/to/flutter/bin/flutter \
ENGINE_DIR=/path/to/flutter-engine/bundle-debug-x86_64 ./run.sh
```

## On a board (emb cross)

`run.sh` runs the example on the dev host. For a Raspberry Pi, the shell, the
engine, the app image and `libbev_view.so` are cross-built and deployed by
[emb](https://github.com/toyota-connected/emb_cli):

```bash
. /path/to/workspace/setup_env.sh          # dart + flutter on PATH
emb cross /path/to/ivi-homescreen \
  --target rpi4-trixie --build --backend drm-kms-egl \
  --app /path/to/libcamera-dmabuf-capture/flutter/bev_view/example \
  --mode release --deploy <user>@<host> --deploy-dir bev-view
```

The positional project is **ivi-homescreen** — it owns the board profile and
builds the embedder; this example is the `--app` layer on top. `rpi5-trixie`
swaps the board in the same command.

- **`.emb/raspberry-pi.emb.yaml`** beside this README is that app layer. It adds
  only `libpng-dev` to the sysroot (snapshots); libcamera, GStreamer, EGL/GLES,
  DRM, gbm and the Vulkan loader already come from ivi-homescreen's Pi targets
  and emb's board library. `sysroot.dev_packages` union across layers, so this
  adds to that set rather than replacing it.
- **`libbev_view.so` is built by the package's Dart build hook**, not declared
  as a `cross.modules` entry: the hook is what produces the native-assets
  manifest the `@Native` bindings resolve through. It inherits the cross
  toolchain because it drives plain `cmake`, and emb puts a wrapper naming
  `CMAKE_TOOLCHAIN_FILE` on `PATH` for exactly that case. The bundle audit then
  refuses any library in `lib/` that is not built for the target.
- **The target's `libihs_shared` is found automatically.** emb's shell build
  tree carries a profile hash that moves with the configuration, so
  `CMakeLists.txt` derives the workspace from `CMAKE_SYSROOT` and takes the
  newest `cross-build-<triple>-*/build-*` under it. `-DIHS_BUILD_DIR=` still
  overrides.
- **`emb.lock` drift.** If emb refuses with *inputs changed (manifest edited)*,
  add `--no-verify` for the run, or `--update-lock` to accept the new inputs
  into ivi-homescreen's lock.

Then start it on the device, which `--run` cannot do for you — it launches
`./homescreen -b .` with no backend or geometry:

```bash
scp board/run-bev.sh <user>@<host>:bev-view/
ssh <user>@<host> 'cd bev-view && ./run-bev.sh'        # test pattern
ssh <user>@<host> 'cd bev-view && BEV_MODE=camera ./run-bev.sh'
```

`run-bev.sh` finds the output instead of assuming one: it prefers a connected
DSI panel, falls back to the first connected connector, takes the mode the
connector reports, and passes `--drm-device`. It has to — a Pi 4 has the DSI
panel on vc4's card at 800x480, while a Pi 5 drives DSI from a separate
`rp1-dsi` card at 800x1280 with vc4 holding HDMI, so one hard-coded card or
geometry is wrong on one of the two. It prints what it picked:

```text
[run-bev] drm-kms-egl /dev/dri/card1 800x1280 mode=pattern
```

`DEVICE`, `W` and `H` override it — `DEVICE=/dev/dri/card0 W=1280 H=1024
./run-bev.sh` drives HDMI on a board where DSI is also connected.

Passing `--backend` twice builds both, and the deploy then lands a **complete
bundle per backend** — `bev-view/drm-kms-egl/` and `bev-view/drm-kms-vulkan/`,
each with its own `homescreen`, `lib/` and `data/`. Run from inside one of
them, and stage `run-bev.sh` into each.

> **`--deploy` prunes the destination.** It rsyncs the bundle and deletes
> anything it did not put there, so the run script and any hand-staged library
> (see below) have to be copied **after every deploy**, not once. A run that
> silently does nothing, with `setsid: failed to execute ./run-bev.sh`, is this
> and not the app.

### What the board needs

A PiOS *lite* image carries neither of the first two:

| | |
|---|---|
| GStreamer runtime | `sudo apt install libgstreamer1.0-0 libgstreamer-plugins-base1.0-0 gstreamer1.0-libav gstreamer1.0-plugins-good gstreamer1.0-plugins-bad` — `libbev_view.so` links it for the file and surround modes, so without it the library will not load at all |
| `libdisplay-info` | `sudo apt install libdisplay-info2` — the trixie shell links it; alternatively copy `libdisplay-info.so.0.2.0` out of the emb sysroot into the bundle's `lib/` and recreate the soname link beside it (`ln -sf libdisplay-info.so.0.2.0 lib/libdisplay-info.so.2`), which the prune above eats along with the library. The Pi 5 image already carries it |
| a camera | `BEV_MODE=camera` needs one attached; check with `rpicam-hello --list-cameras` |

### Tearing, and what the fence counters say

A producer may only redraw a ring slot once the compositor has released it. The
release arrives as a per-buffer eventfd from `ihs_pv_submit`, and the view waits
up to 34 ms (two vsyncs) for it; past that it draws anyway (a tear beats a hang). Every such
timeout is a slot overwritten while the display may still be reading it —
visible tearing — so `releaseTimeouts` is the number to watch:

```bash
ssh <user>@<host> 'cd bev-view && BEV_STAT=1 ./run-bev.sh'   # prints each sample
```

```text
[stat] 30.0 fps  1280x720 -> 800x480  texture-dmabuf-import XR24 ... fence waits 721 timeouts 3 (306 ms)
```

Read waits and timeouts as a pair: a run that never waited reports zero
timeouts too, so healthy is `waits > 0 && timeouts == 0` (bar the first frames,
before anything has been released).

Measured on a Pi 4 / drm-kms-egl, 25 s per run, with the earlier 100 ms timeout,
which is why the ring defaults to **4**:

| ring | grant | timeouts / waits | fps |
|---|---|---|---|
| 3 | texture-dmabuf-import | 42 / 622 | 30 |
| 4 | texture-dmabuf-import | 3 / 721 | 30 |
| 3 | drm-plane (`BEV_SCANOUT=1`) | 110 / 351 | **14** |
| 4 | drm-plane | 3 / 721 | 30 |

Two things to take from it. The frame rate collapse at ring 3 on the plane path
is the producer *blocking*: 110 timeouts is 11 s of a 25 s run spent waiting.
And a deeper ring is what fixes both, because the stalls come from frames the
shell composited rather than scanned out, whose eventfd is never signalled at
all (ivi-homescreen #530) — waiting longer would not have helped, but having
another slot to draw into does. `BEV_RING` (2..6) overrides it per run.

### Tearing that the counters do *not* see

A deeper ring removes the timeouts but not necessarily the tearing, and the
counters will happily read `timeouts 3` while the panel bands. The reason is in
the shell: on the GL-composited path (`drm_plane_id == 0`) `HostSubmit` signals
a frame's release as soon as a **newer frame supersedes it**
(`platform_view_host.cc`, the `SignalRelease` call in the EGL branch). The GL
import happens lazily on the raster thread in `GetGlTextureName` and stays bound
as `current_egl`, so "superseded" says nothing about whether the compositor has
finished sampling that texture. The producer is told the slot is free, redraws
it, and the frame on screen tears — with no timeout recorded, because the
release did arrive.

On a KMS plane the release is honest: it comes from `OnScanoutRelease` when the
plane stops scanning the buffer out. That is what `BEV_SCANOUT=1` asks for, and
it measures clean (0 timeouts over the first ~400 frames on a Pi 4) where the
texture-import path shows its first timeouts around 7 s in.

### The once-per-run stall: a shell fd leak (fixed in v3.0)

Even with a ring of 4, every run showed exactly three consecutive timeouts, at
frame 192 (+6.4 s) or 448 (+15 s) — never anywhere else. Those are where the
shell's file-descriptor count crosses 256 and 512: ivi-homescreen leaked one
`sync_file` per explicit-sync submit (`HandBackReleaseFence` stored a dup of the
compositor fence in `*out_release_fence_fd`, then `HandBackReleaseEventfd`
overwrote it without closing). Each fd-table expansion in a multi-threaded
process waits out an RCU grace period, which on a Pi 4 is a ~120 ms stall; at
30 fps the table reached the 1024 soft limit about half a minute in.

Fixed upstream in v3.0 by `83887302` (PR #593) — the eventfd path now closes
`*out_fd` before overwriting it. On a shell that carries it, a Pi 4 run holds at
65–69 fds across 1164 fence waits with no timeouts. If you are on an older
shell, check for the leak with the view running:

```bash
ls /proc/$(pgrep -x homescreen)/fd | wc -l      # climbing ~30/s = the leak
```

and set `BEV_IMPLICIT_SYNC=1`, which makes the producer submit without an
acquire fence — it `glFinish()`es instead — skipping the leaking branch. That
costs the pipelining an acquire fence buys, so it is a fallback for old shells,
not a setting to leave on.

### Judder

Motion in the test pattern is a function of **elapsed time**, not of the frame
counter. That is not a detail: with per-frame motion, every hitch and every
change of frame rate becomes a change of *speed*, so a rate that wobbles looks
like judder no matter how cleanly each frame is drawn — and a run at a higher
frame rate silently plays faster, which makes two runs incomparable. If you add
a source, pace its motion the same way.

### libcamera: check which one the sysroot resolved

PiOS carries libcamera in **two** apt indexes, at different versions — Debian
trixie ships `libcamera-dev` 0.4.0-7, the Raspberry Pi archive ships
0.7.0+rpt, and a current PiOS device runs the rpt one. emb merges indexes
first-writer-wins (`AptIndex.addAll`) rather than comparing versions the way
apt does, so the Debian package can win and leave a sysroot whose headers,
`libcamera.pc` and `libcamera.so` symlink are 0.4 while its *runtime*
`libcamera0.7` is 0.7. Nothing fails at build time; the library links
`libcamera.so.0.4` and then will not load on the board:

```text
Failed to load dynamic library 'libbev_view.so': libcamera.so.0.4:
cannot open shared object file: No such file or directory
```

Check the sysroot before trusting a camera-mode build, and stage the rpt dev
package over it if it resolved low:

```sh
SR=<workspace>/.config/flutter_workspace/cross-<triple>-<key>/sysroot
grep Version "$SR/usr/lib/aarch64-linux-gnu/pkgconfig/libcamera.pc"   # want 0.7.x
curl -sSLO http://archive.raspberrypi.com/debian/pool/main/libc/libcamera/libcamera-dev_0.7.0+rpt20260205-1_arm64.deb
dpkg-deb -x libcamera-dev_0.7.0+rpt20260205-1_arm64.deb "$SR"
```

Then delete the hook's CMake tree so the library actually relinks —
a rebuild alone reuses the cached pkg-config result and silently keeps 0.4:

```sh
rm -rf .dart_tool/hooks_runner/shared/bev_view/build .dart_tool/hooks_runner/bev_view
```

The Pi runs headless here: `vc4` drives the display (`card1`) and `v3d` is the
renderer (`renderD128`), so the shell takes a DRM/KMS backend rather than a
Wayland one, and the view allocates its buffers on the shell's gbm device.

## EGL and Vulkan backends

Nothing about the view has to match the backend at build time — one
`libbev_view.so` runs on all of them. It always renders with GLES (that is what
the pipeline's shaders are), and picks *who allocates its buffers* from what the
shell exposes at run time:

| Backend | Buffers come from | Why |
|---|---|---|
| `wayland-vulkan`, `drm-kms-vulkan` | exportable `VkImage`s on the shell's own device, imported into GL | the modifier is one the compositor's device reported it can sample |
| `drm-kms-egl` | scanout-capable gbm bos on the shell's gbm device | a `DRM_PLANE` grant (`BEV_SCANOUT=1`) can put them straight on a KMS plane |
| `wayland-egl` | gbm bos on the view's own render node | the shell exposes no gbm device |

The EGL display opens on the render node that matches the shell's GPU
(`VK_EXT_physical_device_drm` on Vulkan, the gbm device's node on DRM), falling
back to `/dev/dri/renderD128`; `BEV_RENDER_NODE` overrides it. The overlay shows
the grant, format and allocator actually chosen.

A build with `enable_vulkan: false` (or no Vulkan headers) still runs on a Vulkan
backend, allocating from the render node; the compositor then has to import a
buffer its device did not allocate.

### What the Vulkan backends need from the shell

`drm-kms-vulkan` blends platform views into its backing store rather than
putting them on a plane, and until v3.0 `4bb31fda` it never handed the producer
a release fence for that read — every submit came back with `-1`, so the ring
depth was the only thing keeping the compositor and the producer apart. On an
older shell the counters look *clean* (`fence waits 0`) precisely because
nothing is being waited on; read that as "no signal", not "no contention".

A run on the Pi 5's DSI panel also needs `IVI_DRMVK_VSYNC=0`:

```bash
ssh <user>@<host> 'cd bev-view && IVI_DRMVK_VSYNC=0 BACKEND=drm-kms-vulkan ./run-bev.sh'
```

That panel's driver (`rp1-dsi`) delivers no page-flip completion events, and the
backend's flip-driven vsync waits on one forever: the compositor presents two
frames and goes silent while the producer keeps submitting at full rate, so the
screen freezes on a stale frame with healthy-looking producer stats. The
wall-clock vsync source sidesteps it. HDMI on vc4 is unaffected, as is
`drm-kms-egl` on the same panel.

## What you should see

- **pattern** — scrolling colour bars with a dark band sweeping down. Bars move
  left, band moves down: if either is reversed, the orientation is wrong.
- **camera** — the IMX219 through the red-tint + parking-grid shader, after the
  30-frame AE/AWB warm-up.
- **file** / **surround** — the recording(s), looping on EOS. Surround adds the
  overlap/edge steppers and the free-yaw toggle, with the same steps as the CLI's
  `+`/`-` and `[`/`]`.

The top-left overlay reads the view's counters every 500 ms: fps, source and
view size, the grant, and the buffer allocator. **snapshot** writes
`/tmp/bev_snapshot_NNN.png` on the machine running the shell.

The first submit also prints, once, which halves of explicit sync are actually
in play:

```text
[bev/ihs_pv] first submit: acquire fence yes, release fence no
```

`fence waits 0` in the stats reads the same whether the producer never made an
acquire fence or the compositor never handed a release one back; this line says
which. `release fence no` on the very first submit is normal — no composite has
run yet — but a run that never accumulates fence waits afterwards is the
compositor staying silent.

Producer counters alone cannot tell you the picture reached the display: they
count submits, not presents. If the stats look healthy while the screen is
frozen, check the shell's own present log before suspecting the view.

If the pipeline cannot start (no camera, a missing file, a config slot without
`hb2i`), the view shows black and the reason is in the native log under
`[bev/ihs_pv]` / `[bev/surround]` etc. The creation params are checked before
the view is created, so a malformed one fails the create with the parse error
in the log.

## Environment

The bundle builder compiles only `lib/main.dart` and the shell passes the app no
arguments, so the source is chosen from the environment:

| | |
|---|---|
| `BEV_MODE` | `pattern` (default), `camera`, `file`, `surround` |
| `BEV_CAMERA`, `BEV_TUNING_FILE` | camera index; IPA tuning JSON (`--tuning-file`) |
| `BEV_CAM_FPS` | pin the sensor's frame duration; unset leaves the rate to auto-exposure, which trades it for light |
| `BEV_FILE`, `BEV_FILE_BEV=1` | HEVC recording; forward bird's-eye view (`--bev`) |
| `BEV_SRC` | recordings in config-slot order, `:`-separated (`--src`) |
| `BEV_FORWARD_LEFT` … `BEV_BACKWARD_RIGHT` | the fixed-slot pair rig (`--forward-left` …) |
| `BEV_CONFIG`, `BEV_LENS_DIR` | `bev_config.ini`; lens file directory |
| `BEV_BLEND`, `BEV_PX_PER_M`, `BEV_CAR_ICON` | `feather`/`pyramid`/`coverage`; overrides; car PNG |
| `BEV_FIT` | `contain` (default), `cover`, `fill` |
| `BEV_SCANOUT=1` | request a KMS plane when the shell offers one |
| `BEV_RENDER_NODE` | native: the DRM render node the view's EGL display opens |

Paths are read by the shell process. A path prefixed `asset:` resolves inside
the bundle's `flutter_assets`.

## Creation params

`BevSource` encodes to UTF-8 `key=value` lines, sent raw as the platform view's
creation params and parsed by `ihs/bev_params.cpp`. The keys mirror the CLI's
flags one for one (`--forward-left` → `forward_left=`), so a working command
line translates directly. The full list is at the top of `ihs/bev_params.h`.

## Building the native library by hand

```bash
cmake -S ../../.. -B build-pv -G Ninja \
  -DENABLE_IHS_PV=ON -DENABLE_WAYLAND_PREVIEW=OFF \
  -DIHS_DIR=/path/to/ivi-homescreen -DIHS_BUILD_DIR=/path/to/ivi-homescreen/build
cmake --build build-pv --target bev_view
```

Without `IHS_DIR` (and no checkout beside the repository) CMake uses an installed
`ivi-homescreen-shared` package. `-DENABLE_IHS_PV_VULKAN=OFF` drops the Vulkan
allocator.

## Files

- `lib/main.dart` — environment → `BevSource`, the full-screen view, the stats
  overlay and the controls.
- `run.sh` — build, assemble the ivi bundle, launch the shell.
- `../lib/` — the `bev_view` package: `BevView`, `BevViewController`, the
  sources, and the `@Native` bindings.
- `../hook/build.dart` — builds `libbev_view.so` from the repository's
  `CMakeLists.txt` (`ENABLE_IHS_PV=ON`).
- `../../../ihs/` — the native seam: `bev_ihs_pv.cpp` (factory, negotiation,
  render loop, submit), `dmabuf_ring.cpp` (buffers + blit), 
  `vk_dmabuf_allocator.cpp`, `bev_pipeline.cpp` (the CLI's modes, non-interactive)
  and `bev_params.cpp`.
