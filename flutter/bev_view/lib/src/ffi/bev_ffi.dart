// dart:ffi bindings to libbev_view.so (built by hook/build.dart). The
// @DefaultAsset id matches the CodeAsset the hook publishes. Mirrors
// ihs/bev_view_api.h.
@DefaultAsset('package:bev_view/src/ffi/bev_ffi.dart')
library;

// The externals keep the C names, so a grep finds both sides.
// ignore_for_file: non_constant_identifier_names

import 'dart:ffi';

/// Values of [BevViewStats.allocator]. Mirrors `BEV_ALLOCATOR_*`.
abstract final class BevAllocator {
  static const int none = 0;
  static const int vulkan = 1;
  static const int shellGbm = 2;
  static const int renderNode = 3;
}

/// Mirrors `BevViewStats`.
final class BevViewStats extends Struct {
  @Uint64()
  external int framesRendered;
  @Uint64()
  external int framesSubmitted;
  @Double()
  external double fps;
  @Double()
  external double overlap;
  @Double()
  external double blendEdge;
  @Int32()
  external int sourceWidth;
  @Int32()
  external int sourceHeight;
  @Int32()
  external int viewWidth;
  @Int32()
  external int viewHeight;
  @Uint32()
  external int grantedKind;
  @Uint32()
  external int fourcc;
  @Uint64()
  external int modifier;
  @Int32()
  external int allocator;
  @Int32()
  external int running;
  @Uint64()
  external int releaseWaits;
  @Uint64()
  external int releaseTimeouts;
  @Double()
  external double releaseWaitMs;
}

/// Installs the "views/bev-view" factory. Dart is the only thing that loads the
/// library, so this must run once before a view of that type is created.
@Native<Void Function()>()
external void bev_register_ihs_pv();

@Native<Void Function()>()
external void bev_unregister_ihs_pv();

@Native<Int32 Function(Int64, Pointer<BevViewStats>)>()
external int bev_view_stats(int viewId, Pointer<BevViewStats> out);

@Native<Void Function(Int64, Double)>()
external void bev_view_set_overlap(int viewId, double value);

@Native<Void Function(Int64, Double)>()
external void bev_view_set_blend_edge(int viewId, double value);

@Native<Void Function(Int64, Int32)>()
external void bev_view_set_free_yaw(int viewId, int enable);

@Native<Void Function(Int64, Double, Double)>()
external void bev_view_set_car_center(int viewId, double xM, double yM);

@Native<Int32 Function(Int64, Pointer<Char>)>()
external int bev_view_snapshot(int viewId, Pointer<Char> path);
