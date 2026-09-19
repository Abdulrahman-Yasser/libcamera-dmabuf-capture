import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';

const String kPipelineViewType = 'surround_view_bev';

class PipelineView extends StatefulWidget {
  const PipelineView({super.key});

  @override
  State<PipelineView> createState() => _PipelineViewState();
}

class _PipelineViewState extends State<PipelineView> {
  Size _size = const Size(1, 1);
  _PipelineViewController? _controller;

  @override
  void dispose() {
    unawaited(_controller?.dispose());
    _controller = null;
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (defaultTargetPlatform != TargetPlatform.linux) {
      return const Center(child: Text('Needs ivi-homescreen on Linux'));
    }
    return LayoutBuilder(
      builder: (BuildContext context, BoxConstraints constraints) {
        final double w =
            constraints.maxWidth.isFinite && constraints.maxWidth > 0
                ? constraints.maxWidth
                : 1;
        final double h =
            constraints.maxHeight.isFinite && constraints.maxHeight > 0
                ? constraints.maxHeight
                : 1;
        final Size size = Size(w, h);
        if (size != _size) {
          _size = size;
          final _PipelineViewController? controller = _controller;
          if (controller != null) {
            WidgetsBinding.instance.addPostFrameCallback((_) {
              unawaited(controller.resize(w, h));
            });
          }
        }
        return PlatformViewLink(
          viewType: kPipelineViewType,
          surfaceFactory:
              (BuildContext context, PlatformViewController controller) {
            return PlatformViewSurface(
              controller: controller,
              hitTestBehavior: PlatformViewHitTestBehavior.transparent,
              gestureRecognizers:
                  const <Factory<OneSequenceGestureRecognizer>>{},
            );
          },
          onCreatePlatformView: (PlatformViewCreationParams params) {
            final _PipelineViewController controller = _PipelineViewController(
              id: params.id,
              width: _size.width,
              height: _size.height,
            );
            _controller = controller;
            unawaited(controller.create().then(
                  (_) => params.onPlatformViewCreated(params.id),
                  onError: (Object error, StackTrace stack) {
                    FlutterError.reportError(FlutterErrorDetails(
                      exception: error,
                      stack: stack,
                      library: 'pipeline_view',
                      context: ErrorDescription('while creating the view'),
                    ));
                  },
                ));
            return controller;
          },
        );
      },
    );
  }
}

class _PipelineViewController extends PlatformViewController {
  _PipelineViewController({
    required this.id,
    required this.width,
    required this.height,
  });

  final int id;
  double width;
  double height;
  bool _created = false;
  Future<void>? _creation;

  @override
  int get viewId => id;

  @override
  bool get awaitingCreation => !_created;

  @override
  Future<void> create({Size? size, Offset? position}) =>
      _creation ??= _createOnce();

  Future<void> _createOnce() async {
    try {
      await SystemChannels.platform_views
          .invokeMethod<void>('create', <String, Object>{
        'id': id,
        'viewType': kPipelineViewType,
        'direction': 0,
        'width': width,
        'height': height,
        'params': Uint8List(0),
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
    await SystemChannels.platform_views
        .invokeMethod<void>('resize', <String, Object>{
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
  Future<void> dispatchPointerEvent(PointerEvent event) async {}

  @override
  Future<void> dispose() async {
    final Future<void>? creating = _creation;
    if (creating == null) return;
    _creation = null;
    try {
      await creating;
    } on Object {
      return;
    } finally {
      _created = false;
    }
    await SystemChannels.platform_views.invokeMethod<void>('dispose', id);
  }
}
