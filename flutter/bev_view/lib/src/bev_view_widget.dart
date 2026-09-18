import 'dart:async';
import 'dart:convert';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter/widgets.dart';

import 'controller.dart';
import 'ffi/bev_ffi.dart' as ffi;
import 'source.dart';

/// The factory libbev_view.so registers (ihs/bev_ihs_pv.cpp kViewType).
const String _viewType = 'views/bev-view';

typedef BevViewCreatedCallback = void Function(BevViewController controller);

/// The pipeline as an ivi-homescreen platform view.
///
/// The native producer renders offscreen and hands the shell a dma-buf per
/// frame; the shell composites it where this widget is laid out, at its
/// physical size. Flutter widgets stacked above it receive input as usual.
class BevView extends StatefulWidget {
  const BevView({
    super.key,
    required this.source,
    this.fit = BevFit.contain,
    this.ringSize = 4,
    this.scanout = false,
    this.onCreated,
  });

  /// What to render. Read when the platform view is created; give the widget a
  /// new [Key] to switch to a different source.
  final BevSource source;

  final BevFit fit;

  /// dma-bufs in the view's ring, 2..6.
  ///
  /// Four by default. A slot is redrawn only once the compositor releases it,
  /// and with three the producer waited out the release timeout on ~7% of
  /// frames on a Pi 4 — each one a slot redrawn while the display may still be
  /// reading it, i.e. a tear. Raise it further for a compositor that holds
  /// buffers longer; [BevStats.releaseTimeouts] is how you tell.
  final int ringSize;

  /// Take a KMS overlay plane when the shell offers one (drm-kms-egl). The view
  /// then composites below all Flutter content rather than inline.
  final bool scanout;

  /// Called once the shell has created the view.
  final BevViewCreatedCallback? onCreated;

  @override
  State<BevView> createState() => _BevViewState();

  @override
  void debugFillProperties(DiagnosticPropertiesBuilder properties) {
    super.debugFillProperties(properties);
    properties
      ..add(DiagnosticsProperty<BevSource>('source', source))
      ..add(EnumProperty<BevFit>('fit', fit))
      ..add(IntProperty('ringSize', ringSize))
      ..add(FlagProperty('scanout', value: scanout, ifTrue: 'scanout'));
  }
}

class _BevViewState extends State<BevView> {
  /// The factory is process-wide, so it is registered once. Nothing but Dart
  /// loads the library, so without this the shell has no factory to call.
  static bool _factoryRegistered = false;

  Size _viewSize = const Size(1, 1);
  _BevPlatformViewController? _controller;

  @override
  void initState() {
    super.initState();
    if (defaultTargetPlatform == TargetPlatform.linux && !_factoryRegistered) {
      // Not caught: a library that fails to load should say so here, rather
      // than as a view that never appears.
      ffi.bev_register_ihs_pv();
      _factoryRegistered = true;
    }
  }

  @override
  void dispose() {
    unawaited(_controller?.dispose());
    _controller = null;
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (defaultTargetPlatform != TargetPlatform.linux) {
      return const Center(
        child: Text(
          'BevView needs ivi-homescreen on Linux',
          textDirection: TextDirection.ltr,
        ),
      );
    }
    return LayoutBuilder(
      builder: (context, constraints) {
        final double w = constraints.maxWidth.isFinite && constraints.maxWidth > 0
            ? constraints.maxWidth
            : 1;
        final double h = constraints.maxHeight.isFinite && constraints.maxHeight > 0
            ? constraints.maxHeight
            : 1;
        final size = Size(w, h);
        if (size != _viewSize) {
          _viewSize = size;
          final controller = _controller;
          if (controller != null) {
            // invokeMethod during layout is unsafe; resize after this pass.
            WidgetsBinding.instance.addPostFrameCallback((_) {
              unawaited(controller.resize(w, h));
            });
          }
        }
        return PlatformViewLink(
          viewType: _viewType,
          surfaceFactory: (context, controller) => PlatformViewSurface(
            controller: controller,
            hitTestBehavior: PlatformViewHitTestBehavior.transparent,
            gestureRecognizers: const <Factory<OneSequenceGestureRecognizer>>{},
          ),
          onCreatePlatformView: (params) {
            final controller = _BevPlatformViewController(
              id: params.id,
              width: _viewSize.width,
              height: _viewSize.height,
              params: encodeBevParams(
                widget.source,
                fit: widget.fit,
                ringSize: widget.ringSize,
                scanout: widget.scanout,
              ),
            );
            _controller = controller;
            unawaited(
              controller.create().then(
                (_) {
                  params.onPlatformViewCreated(params.id);
                  if (mounted) widget.onCreated?.call(BevViewController(params.id));
                },
                onError: (Object error, StackTrace stack) {
                  FlutterError.reportError(
                    FlutterErrorDetails(
                      exception: describeViewCreateFailure(error) ?? error,
                      stack: stack,
                      library: 'bev_view',
                      context: ErrorDescription('while creating a BevView'),
                    ),
                  );
                },
              ),
            );
            return controller;
          },
        );
      },
    );
  }
}

/// Explains a view-creation failure the shell reports only in terms of what it
/// did not find, or returns null for one there is nothing to add to.
@visibleForTesting
Exception? describeViewCreateFailure(Object error) {
  if (error is PlatformException && error.code == 'no_factory') {
    return Exception(
      'the shell has no platform-view factory for "$_viewType". Either '
      'libbev_view.so never loaded, or this shell was built without the ihs_pv '
      'platform-view host. ($error)',
    );
  }
  if (error is PlatformException) {
    return Exception(
      'the shell refused the view. The native log says why -- look for '
      '"[bev/ihs_pv]": bad creation params, no dma-buf path on this backend, '
      'or EGL could not start on the render node. ($error)',
    );
  }
  if (error is MissingPluginException) {
    return Exception(
      'this embedder does not handle flutter/platform_views, so it cannot host '
      'a BevView; run the app on ivi-homescreen. ($error)',
    );
  }
  return null;
}

/// Drives ivi-homescreen's flutter/platform_views handler directly. The view is
/// a producer, not a Flutter texture, so the Android-specific platform-view
/// controllers don't apply.
class _BevPlatformViewController extends PlatformViewController {
  _BevPlatformViewController({
    required this.id,
    required this.width,
    required this.height,
    required String params,
  }) : _params = Uint8List.fromList(utf8.encode(params));

  final int id;
  double width;
  double height;

  /// Sent raw: StandardMethodCodec passes a byte list through untouched, so
  /// the factory receives exactly these bytes as IhsPvCreateInfo::params.
  final Uint8List _params;

  bool _created = false;

  /// PlatformViewLink's onCreatePlatformView and the framework's create() can
  /// both drive creation; the cached future makes the shell's create run once.
  Future<void>? _creation;

  @override
  int get viewId => id;

  @override
  bool get awaitingCreation => !_created;

  @override
  Future<void> create({Size? size, Offset? position}) => _creation ??= _createOnce();

  Future<void> _createOnce() async {
    try {
      await SystemChannels.platform_views.invokeMethod<void>('create', <String, Object>{
        'id': id,
        'viewType': _viewType,
        'direction': 0,
        'width': width,
        'height': height,
        'params': _params,
      });
    } on Object {
      _creation = null;
      rethrow;
    }
    _created = true;
  }

  Future<void> resize(double newWidth, double newHeight) async {
    width = newWidth;
    height = newHeight;
    if (!_created) return;
    await SystemChannels.platform_views.invokeMethod<void>('resize', <String, Object>{
      'id': id,
      'width': newWidth,
      'height': newHeight,
    });
  }

  @override
  Future<void> clearFocus() async {
    if (!_created) return;
    await SystemChannels.platform_views.invokeMethod<void>('clearFocus', id);
  }

  @override
  Future<void> dispatchPointerEvent(PointerEvent event) async {
    // Not forwarded: the view takes no input of its own.
  }

  @override
  Future<void> dispose() async {
    // Keyed off _creation rather than _created, so a dispose during an
    // in-flight create still tears the view down.
    final creating = _creation;
    if (creating == null) return;
    _creation = null;
    try {
      await creating;
    } on Object {
      return; // a failed create left nothing to dispose
    } finally {
      _created = false;
    }
    await SystemChannels.platform_views.invokeMethod<void>('dispose', id);
  }
}
