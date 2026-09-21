# bev_view

The capture pipeline from this repository as an ivi-homescreen platform view.

```dart
BevView(
  source: const BevSurroundSource(
    ['/data/front_l.hevc', '/data/front_r.hevc'],
    config: '/data/bev_config.ini',
    blend: BevBlend.pyramid,
  ),
  onCreated: (controller) => controller.setOverlap(120),
)
```

- `BevView` creates a `views/bev-view` platform view. Its native producer,
  `libbev_view.so`, is built by `hook/build.dart` from the repository's
  `CMakeLists.txt` and bundled as a native asset.
- Sources: `BevCameraSource`, `BevFileSource`, `BevSurroundSource`,
  `BevPatternSource` — the CLI's run modes, plus a synthetic test pattern.
- `BevViewController` reads the view's counters and drives the runtime
  controls (overlap, blend edge, free yaw, car centre, snapshot).

The view runs on ivi-homescreen's EGL and Vulkan backends alike; see
[example/README.md](example/README.md) for how buffers are allocated on each,
how to run it, and the hook's `user_defines`.
