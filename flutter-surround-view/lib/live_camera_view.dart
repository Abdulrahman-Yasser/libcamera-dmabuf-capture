import 'dart:async';
import 'dart:io';

import 'package:bev_view/bev_view.dart';
import 'package:flutter/material.dart';

import 'bev_source.dart';
import 'models.dart';

class LiveCameraView extends StatefulWidget {
  const LiveCameraView({
    super.key,
    required this.camera,
    required this.birdsEye,
    this.showLabel = false,
  });

  final CameraId camera;
  final bool birdsEye;
  final bool showLabel;

  @override
  State<LiveCameraView> createState() => _LiveCameraViewState();
}

class _LiveCameraViewState extends State<LiveCameraView> {
  static final bool _showStats = Platform.environment['BEV_STAT'] == '1';

  Timer? _poll;
  BevStats? _stats;

  @override
  void dispose() {
    _poll?.cancel();
    super.dispose();
  }

  void _onCreated(BevViewController controller) {
    if (!_showStats) return;
    _poll = Timer.periodic(const Duration(milliseconds: 500), (_) {
      final BevStats? stats = controller.stats();
      if (!mounted) return;
      setState(() => _stats = stats);
    });
  }

  @override
  Widget build(BuildContext context) {
    final CameraId camera = widget.camera;
    final bool birdsEye = widget.birdsEye;
    final int index = camera.cameraIndex!;
    final BevSource source = birdsEye
        ? birdsEyeSourceFromEnvironment(index)
        : cameraSourceFromEnvironment(index);
    return Container(
      margin: const EdgeInsets.all(3),
      clipBehavior: Clip.hardEdge,
      decoration: BoxDecoration(
        color: Colors.black,
        border: Border.all(color: Colors.white12),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Stack(
        fit: StackFit.expand,
        children: <Widget>[
          BevView(
            key: ValueKey<String>('${camera.name}-$birdsEye'),
            source: source,
            onCreated: _onCreated,
          ),
          if (_showStats && _stats != null)
            Positioned(
              right: 8,
              top: 8,
              child: IgnorePointer(
                child: DecoratedBox(
                  decoration: BoxDecoration(
                    color: Colors.black54,
                    borderRadius: BorderRadius.circular(4),
                  ),
                  child: Padding(
                    padding:
                        const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                    child: Text(
                      '${_stats!.fps.toStringAsFixed(1)} fps',
                      style: const TextStyle(
                        fontFamily: 'monospace',
                        fontSize: 13,
                        color: Colors.white,
                      ),
                    ),
                  ),
                ),
              ),
            ),
          if (widget.showLabel)
            Positioned(
              left: 8,
              top: 8,
              child: DecoratedBox(
                decoration: BoxDecoration(
                  color: Colors.black.withValues(alpha: 0.55),
                  borderRadius: BorderRadius.circular(4),
                ),
                child: Padding(
                  padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                  child: Text(
                    birdsEye ? '${camera.label} BEV' : camera.label,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 12,
                      fontWeight: FontWeight.w600,
                    ),
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }
}
