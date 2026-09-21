import 'dart:ffi';

import 'package:ffi/ffi.dart';

import 'ffi/bev_ffi.dart' as ffi;

/// A snapshot of a view's counters and negotiated surface path.
class BevStats {
  const BevStats({
    required this.framesRendered,
    required this.framesSubmitted,
    required this.fps,
    required this.overlap,
    required this.blendEdge,
    required this.sourceWidth,
    required this.sourceHeight,
    required this.viewWidth,
    required this.viewHeight,
    required this.grantedKind,
    required this.fourcc,
    required this.modifier,
    required this.allocator,
    required this.running,
    required this.releaseWaits,
    required this.releaseTimeouts,
    required this.releaseWaitMs,
  });

  final int framesRendered;
  final int framesSubmitted;
  final double fps;

  /// Surround mode's current seam controls; 0 in the other modes.
  final double overlap;
  final double blendEdge;

  final int sourceWidth;
  final int sourceHeight;
  final int viewWidth;
  final int viewHeight;

  /// `IhsPvKind` of the grant.
  final int grantedKind;
  final int fourcc;
  final int modifier;

  /// One of [ffi.BevAllocator].
  final int allocator;

  /// Whether the source is still producing frames.
  final bool running;

  /// Release-fence waits this view has performed, and how many ran out the
  /// clock instead of being signalled. Read as a pair: a run that never waited
  /// reports zero timeouts too, so the healthy check is
  /// `releaseWaits > 0 && releaseTimeouts == 0`.
  ///
  /// Timeouts mean the compositor never released the buffer and the producer
  /// drew over it anyway, which on a scanned-out buffer is visible tearing.
  final int releaseWaits;
  final int releaseTimeouts;

  /// Total time spent in those waits.
  final double releaseWaitMs;

  String get grantedKindName => switch (grantedKind) {
    1 => 'texture-dmabuf-import',
    2 => 'drm-plane',
    4 => 'software-shm',
    _ => 'none',
  };

  String get allocatorName => switch (allocator) {
    ffi.BevAllocator.vulkan => 'vulkan',
    ffi.BevAllocator.shellGbm => 'shell-gbm',
    ffi.BevAllocator.renderNode => 'render-node-gbm',
    _ => 'none',
  };

  /// The fourcc as its four characters, e.g. `XR24`.
  String get fourccName => String.fromCharCodes([
    fourcc & 0xff,
    (fourcc >> 8) & 0xff,
    (fourcc >> 16) & 0xff,
    (fourcc >> 24) & 0xff,
  ]);
}

/// Runtime controls for one live BevView, addressed by platform-view id.
///
/// Every call is applied on the view's render thread before its next frame. A
/// call against a view that has been disposed does nothing.
class BevViewController {
  const BevViewController(this.viewId);

  final int viewId;

  /// The view's counters, or null once it has been disposed.
  BevStats? stats() {
    final out = calloc<ffi.BevViewStats>();
    try {
      if (ffi.bev_view_stats(viewId, out) == 0) return null;
      final s = out.ref;
      return BevStats(
        framesRendered: s.framesRendered,
        framesSubmitted: s.framesSubmitted,
        fps: s.fps,
        overlap: s.overlap,
        blendEdge: s.blendEdge,
        sourceWidth: s.sourceWidth,
        sourceHeight: s.sourceHeight,
        viewWidth: s.viewWidth,
        viewHeight: s.viewHeight,
        grantedKind: s.grantedKind,
        fourcc: s.fourcc,
        modifier: s.modifier,
        allocator: s.allocator,
        running: s.running != 0,
        releaseWaits: s.releaseWaits,
        releaseTimeouts: s.releaseTimeouts,
        releaseWaitMs: s.releaseWaitMs,
      );
    } finally {
      calloc.free(out);
    }
  }

  /// Surround seam blend half-width, degrees (1..360).
  void setOverlap(double degrees) => ffi.bev_view_set_overlap(viewId, degrees);

  /// Surround crossover sharpness, 0 (gradual) .. 0.49 (near-instant cut).
  void setBlendEdge(double edge) => ffi.bev_view_set_blend_edge(viewId, edge);

  /// Surround testing aid: ignore each camera's facing wedge.
  void setFreeYaw(bool enabled) =>
      ffi.bev_view_set_free_yaw(viewId, enabled ? 1 : 0);

  /// Moves the surround car icon, world meters from the canvas centre.
  void setCarCenter(double xMeters, double yMeters) =>
      ffi.bev_view_set_car_center(viewId, xMeters, yMeters);

  /// Writes a PNG of the pipeline output to [path], on the shell's filesystem.
  /// Returns whether it was queued.
  bool snapshot(String path) {
    final native = path.toNativeUtf8();
    try {
      return ffi.bev_view_snapshot(viewId, native.cast<Char>()) != 0;
    } finally {
      malloc.free(native);
    }
  }
}
