import 'package:flutter/material.dart';

import 'models.dart';
import 'pipeline_view.dart';

class CameraViewport extends StatelessWidget {
  const CameraViewport({
    super.key,
    required this.mode,
    required this.overlays,
    required this.camSubViews,
    this.frontRearCamera = CameraId.front,
    this.livePipeline = false,
  });

  final ViewMode mode;
  final Set<CameraOverlay> overlays;
  final CameraId frontRearCamera;
  final bool livePipeline;
  final Map<CameraId, CameraSubView> camSubViews;

  @override
  Widget build(BuildContext context) {
    final Widget content = switch (mode) {
      ViewMode.surround360 => livePipeline
          ? const PipelineView()
          : const Surround360Placeholder(),
      ViewMode.sideViews => _SideViews(subViews: camSubViews),
      ViewMode.frontRear =>
        CameraBox(camera: frontRearCamera, showLabel: true),
      _ => SingleCameraView(
          camera: mode.asCamera!,
          subView: camSubViews[mode.asCamera!]!,
          showLabel: true,
        ),
    };

    final bool showOverlays = mode != ViewMode.surround360;

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

class Surround360Placeholder extends StatelessWidget {
  const Surround360Placeholder({super.key});

  @override
  Widget build(BuildContext context) {
    return const Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Icon(Icons.threesixty, size: 64, color: Colors.white24),
          SizedBox(height: 8),
          Text(
            '360° panorama\nstitched output goes here',
            textAlign: TextAlign.center,
            style: TextStyle(color: Colors.white38, fontSize: 12),
          ),
        ],
      ),
    );
  }
}

class _SideViews extends StatelessWidget {
  const _SideViews({required this.subViews});

  final Map<CameraId, CameraSubView> subViews;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: <Widget>[
        Expanded(
          child: SingleCameraView(
            camera: CameraId.left,
            subView: subViews[CameraId.left]!,
            showLabel: true,
          ),
        ),
        Expanded(
          child: SingleCameraView(
            camera: CameraId.right,
            subView: subViews[CameraId.right]!,
            showLabel: true,
          ),
        ),
      ],
    );
  }
}

class SingleCameraView extends StatelessWidget {
  const SingleCameraView({
    super.key,
    required this.camera,
    required this.subView,
    this.showLabel = false,
  });

  final CameraId camera;
  final CameraSubView subView;
  final bool showLabel;

  @override
  Widget build(BuildContext context) {
    return switch (subView) {
      CameraSubView.normal => CameraBox(camera: camera, showLabel: showLabel),
      _ => _CameraSubViewPlaceholder(camera: camera, subView: subView),
    };
  }
}

class _CameraSubViewPlaceholder extends StatelessWidget {
  const _CameraSubViewPlaceholder({
    required this.camera,
    required this.subView,
  });

  final CameraId camera;
  final CameraSubView subView;

  @override
  Widget build(BuildContext context) {
    return Container(
      margin: const EdgeInsets.all(3),
      decoration: BoxDecoration(
        color: const Color(0xFF14141A),
        border: Border.all(color: Colors.white12),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Center(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: <Widget>[
            Icon(
              subView == CameraSubView.bev
                  ? Icons.directions_car
                  : Icons.crop_free,
              size: 40,
              color: Colors.white24,
            ),
            const SizedBox(height: 6),
            Text(
              '${camera.label} - ${subView.label} placeholder',
              textAlign: TextAlign.center,
              style: const TextStyle(color: Colors.white38, fontSize: 12),
            ),
          ],
        ),
      ),
    );
  }
}

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
