// The camera area in the centre of the screen: the 2x2 grid, a single feed,
// or the BEV placeholder, with optional guide-line / distance-grid overlays.

import 'package:flutter/material.dart';

import 'models.dart';

class CameraViewport extends StatelessWidget {
  const CameraViewport({super.key, required this.mode, required this.overlays});

  final ViewMode mode;
  final Set<CameraOverlay> overlays;

  @override
  Widget build(BuildContext context) {
    final Widget content = switch (mode) {
      ViewMode.grid => const _Grid(),
      ViewMode.bev => const BevPlaceholder(),
      _ => CameraBox(camera: mode.asCamera!, showLabel: true),
    };

    final bool showOverlays = mode != ViewMode.grid;

    return ColoredBox(
      color: const Color(0xFF0A0A0C),
      child: Stack(
        fit: StackFit.expand,
        children: <Widget>[
          content,
          if (showOverlays && overlays.contains(CameraOverlay.distanceGrid))
            const IgnorePointer(
              child: CustomPaint(painter: _DistanceGridPainter()),
            ),
          if (showOverlays && overlays.contains(CameraOverlay.guidelines))
            const IgnorePointer(
              child: CustomPaint(painter: _GuidelinesPainter()),
            ),
        ],
      ),
    );
  }
}

class _Grid extends StatelessWidget {
  const _Grid();

  @override
  Widget build(BuildContext context) {
    return const Column(
      children: <Widget>[
        Expanded(
          child: Row(
            children: <Widget>[
              Expanded(child: CameraBox(camera: CameraId.front, showLabel: true)),
              Expanded(child: CameraBox(camera: CameraId.rear, showLabel: true)),
            ],
          ),
        ),
        Expanded(
          child: Row(
            children: <Widget>[
              Expanded(child: CameraBox(camera: CameraId.left, showLabel: true)),
              Expanded(child: CameraBox(camera: CameraId.right, showLabel: true)),
            ],
          ),
        ),
      ],
    );
  }
}

/// One camera feed. A bundled placeholder image today; a `Texture` fed by the
/// libcamera / dmabuf pipeline later — the surrounding layout does not change.
class CameraBox extends StatelessWidget {
  const CameraBox({super.key, required this.camera, this.showLabel = false});

  final CameraId camera;
  final bool showLabel;

  @override
  Widget build(BuildContext context) {
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
          Image.asset(
            camera.asset,
            fit: BoxFit.cover,
            gaplessPlayback: true,
            errorBuilder: (BuildContext context, Object error, StackTrace? stack) {
              return const ColoredBox(
                color: Color(0xFF1E1E1E),
                child: Center(
                  child: Icon(Icons.videocam_off, color: Colors.white24),
                ),
              );
            },
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
                    camera.label,
                    style: const TextStyle(
                      color: Colors.white,
                      fontSize: 13,
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

/// Stand-in for the stitched top-down view: the four feeds arranged around a
/// vehicle marker, with a note that the real composite lands here.
class BevPlaceholder extends StatelessWidget {
  const BevPlaceholder({super.key});

  @override
  Widget build(BuildContext context) {
    return Center(
      child: ConstrainedBox(
        constraints: const BoxConstraints(maxWidth: 340, maxHeight: 460),
        child: AspectRatio(
          aspectRatio: 3 / 4,
          child: Stack(
            alignment: Alignment.center,
            children: const <Widget>[
              Align(alignment: Alignment.topCenter, child: _BevThumb(CameraId.front)),
              Align(alignment: Alignment.bottomCenter, child: _BevThumb(CameraId.rear)),
              Align(alignment: Alignment.centerLeft, child: _BevThumb(CameraId.left)),
              Align(alignment: Alignment.centerRight, child: _BevThumb(CameraId.right)),
              Column(
                mainAxisSize: MainAxisSize.min,
                children: <Widget>[
                  Icon(Icons.directions_car, size: 56, color: Colors.white24),
                  SizedBox(height: 8),
                  Text(
                    "Bird's-eye view\nstitched output goes here",
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.white38, fontSize: 12),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _BevThumb extends StatelessWidget {
  const _BevThumb(this.camera);

  final CameraId camera;

  @override
  Widget build(BuildContext context) {
    return SizedBox(
      width: 96,
      height: 72,
      child: CameraBox(camera: camera, showLabel: true),
    );
  }
}

class _GuidelinesPainter extends CustomPainter {
  const _GuidelinesPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final double w = size.width;
    final double h = size.height;

    final Offset lNear = Offset(w * 0.30, h * 0.98);
    final Offset rNear = Offset(w * 0.70, h * 0.98);
    final Offset lFar = Offset(w * 0.43, h * 0.55);
    final Offset rFar = Offset(w * 0.57, h * 0.55);

    final Paint rail = Paint()
      ..color = Colors.white
      ..strokeWidth = 3
      ..style = PaintingStyle.stroke;
    canvas.drawLine(lNear, lFar, rail);
    canvas.drawLine(rNear, rFar, rail);

    void bar(double t, Color color) {
      final Paint p = Paint()
        ..color = color
        ..strokeWidth = 5;
      canvas.drawLine(
        Offset.lerp(lNear, lFar, t)!,
        Offset.lerp(rNear, rFar, t)!,
        p,
      );
    }

    bar(0.10, const Color(0xFFE53935));
    bar(0.45, const Color(0xFFFFB300));
    bar(0.82, const Color(0xFF43A047));
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}

class _DistanceGridPainter extends CustomPainter {
  const _DistanceGridPainter();

  @override
  void paint(Canvas canvas, Size size) {
    final Paint p = Paint()
      ..color = Colors.white24
      ..strokeWidth = 1;

    for (final double f in <double>[0.55, 0.68, 0.80, 0.92]) {
      canvas.drawLine(
        Offset(size.width * 0.15, size.height * f),
        Offset(size.width * 0.85, size.height * f),
        p,
      );
    }
    canvas.drawLine(
      Offset(size.width * 0.30, size.height * 0.55),
      Offset(size.width * 0.12, size.height * 0.98),
      p,
    );
    canvas.drawLine(
      Offset(size.width * 0.70, size.height * 0.55),
      Offset(size.width * 0.88, size.height * 0.98),
      p,
    );
  }

  @override
  bool shouldRepaint(covariant CustomPainter oldDelegate) => false;
}
