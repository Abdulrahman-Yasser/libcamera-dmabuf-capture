import 'package:bev_view/bev_view.dart';
import 'package:flutter/material.dart';

import 'bev_source.dart';
import 'models.dart';

class LiveCameraView extends StatelessWidget {
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
  Widget build(BuildContext context) {
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
          ),
          if (showLabel)
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
