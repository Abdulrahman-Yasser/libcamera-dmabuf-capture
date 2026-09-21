import 'package:flutter/material.dart';

import 'controls.dart';
import 'models.dart';
import 'settings_page.dart';
import 'viewport.dart';

Future<void> main() async {
  WidgetsFlutterBinding.ensureInitialized();
  runApp(const SurroundViewApp());
}

class SurroundViewApp extends StatelessWidget {
  const SurroundViewApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Surround View',
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark().copyWith(
        scaffoldBackgroundColor: const Color(0xFF0C0C0E),
      ),
      home: const SurroundViewScreen(),
    );
  }
}

class SurroundViewScreen extends StatefulWidget {
  const SurroundViewScreen({super.key});

  @override
  State<SurroundViewScreen> createState() => _SurroundViewScreenState();
}

const Set<ViewMode> _fullScreenModes = <ViewMode>{
  ViewMode.surround360,
  ViewMode.frontRear,
};

ViewMode _viewModeForCamera(CameraId camera) => switch (camera) {
      CameraId.front => ViewMode.front,
      CameraId.rear => ViewMode.rear,
      CameraId.left => ViewMode.left,
      CameraId.right => ViewMode.right,
    };

class _SurroundViewScreenState extends State<SurroundViewScreen> {
  ViewMode _mode = ViewMode.surround360;
  CameraId _frontRearCamera = CameraId.front;
  final Map<CameraId, CameraSubView> _camSubView = <CameraId, CameraSubView>{
    for (final CameraId c in CameraId.values) c: CameraSubView.normal,
  };
  final Set<CameraOverlay> _overlays = <CameraOverlay>{};
  bool _calibrating = false;
  CameraId _calibCamera = CameraId.front;
  final Map<CameraId, CalDelta> _deltas = <CameraId, CalDelta>{
    for (final CameraId c in CameraId.values) c: CalDelta(),
  };

  void _toast(String message) {
    ScaffoldMessenger.of(context)
      ..hideCurrentSnackBar()
      ..showSnackBar(
        SnackBar(content: Text(message), duration: const Duration(seconds: 1)),
      );
  }

  void _cameraTap(CameraId camera) => setState(() {
        final ViewMode m = _viewModeForCamera(camera);
        if (_mode == m) {
          _camSubView[camera] = _camSubView[camera]!.next;
        } else {
          _mode = m;
          _camSubView[camera] = CameraSubView.normal;
        }
      });

  void _sideViewsTap() => setState(() {
        if (_mode == ViewMode.sideViews) {
          final CameraSubView next = _camSubView[CameraId.left]!.next;
          _camSubView[CameraId.left] = next;
          _camSubView[CameraId.right] = next;
        } else {
          _mode = ViewMode.sideViews;
          _camSubView[CameraId.left] = CameraSubView.normal;
          _camSubView[CameraId.right] = CameraSubView.normal;
        }
      });

  void _frontRearTap() => setState(() {
        if (_mode == ViewMode.frontRear) {
          _frontRearCamera = _frontRearCamera == CameraId.front
              ? CameraId.rear
              : CameraId.front;
        } else {
          _mode = ViewMode.frontRear;
        }
      });

  void _toggleOverlay(CameraOverlay o) => setState(() {
        if (!_overlays.remove(o)) _overlays.add(o);
      });

  void _nudge(CalAxis axis, int sign) => setState(() {
        final CalDelta d = _deltas[_calibCamera]!;
        switch (axis) {
          case CalAxis.x:
            d.x += sign * 1.0;
          case CalAxis.y:
            d.y += sign * 1.0;
          case CalAxis.yaw:
            d.yaw += sign * 0.5;
        }
      });

  void _openSettings() {
    Navigator.of(context).push(
      MaterialPageRoute<void>(
        builder: (BuildContext context) => SettingsPage(
          initialDeltas: _deltas,
          onSave: (CameraId camera, CalDelta delta) => setState(() {
            _deltas[camera] = delta;
            _toast('Calibration saved for ${camera.label} (mock)');
          }),
          onReset: (CameraId camera) => setState(() {
            _deltas[camera] = CalDelta();
          }),
        ),
      ),
    );
  }

  String _modeTagLabel() {
    if (_mode == ViewMode.frontRear) return '${_frontRearCamera.label} view';
    final CameraId? cam = _mode.asCamera;
    if (cam != null) {
      final CameraSubView sub = _camSubView[cam]!;
      return sub == CameraSubView.normal
          ? '${_mode.label} view'
          : '${_mode.label} - ${sub.label}';
    }
    if (_mode == ViewMode.sideViews) {
      final CameraSubView sub = _camSubView[CameraId.left]!;
      return sub == CameraSubView.normal
          ? 'Left + Right view'
          : 'Left + Right - ${sub.label}';
    }
    return '${_mode.label} view';
  }

  Widget _controlledView() {
    return Stack(
      children: <Widget>[
        Positioned.fill(
          child: CameraViewport(
            mode: _mode,
            overlays: _overlays,
            camSubViews: _camSubView,
            frontRearCamera: _frontRearCamera,
          ),
        ),
        Positioned(
          left: 10,
          top: 10,
          child: _ModeTag(label: _modeTagLabel()),
        ),
        if (_calibrating)
          Positioned(
            left: 10,
            bottom: 10,
            child: CalibrationPanel(
              camera: _calibCamera,
              delta: _deltas[_calibCamera]!,
              onSelectCamera: (CameraId c) =>
                  setState(() => _calibCamera = c),
              onNudge: _nudge,
              onSave: () => _toast(
                'Calibration saved for '
                '${_calibCamera.label} (mock)',
              ),
              onReset: () =>
                  setState(() => _deltas[_calibCamera] = CalDelta()),
            ),
          ),
      ],
    );
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Column(
          children: <Widget>[
            Expanded(
              child: _fullScreenModes.contains(_mode)
                  ? _controlledView()
                  : Row(
                      children: <Widget>[
                        Expanded(flex: 7, child: _controlledView()),
                        const Expanded(
                          flex: 3,
                          child: ColoredBox(
                            color: Color(0xFF0A0A0C),
                            child: BevPlaceholder(),
                          ),
                        ),
                      ],
                    ),
            ),
            TopToolbar(
              mode: _mode,
              onModeChanged: (ViewMode m) => setState(() => _mode = m),
              camSubViews: _camSubView,
              onCameraTap: _cameraTap,
              onSideViewsTap: _sideViewsTap,
              frontRearCamera: _frontRearCamera,
              onFrontRearTap: _frontRearTap,
              overlays: _overlays,
              calibrating: _calibrating,
              onToggleOverlay: _toggleOverlay,
              onToggleCalibrate: () =>
                  setState(() => _calibrating = !_calibrating),
              onSnapshot: () => _toast('Snapshot saved (mock)'),
              onSettings: _openSettings,
            ),
          ],
        ),
      ),
    );
  }
}

class _ModeTag extends StatelessWidget {
  const _ModeTag({required this.label});

  final String label;

  @override
  Widget build(BuildContext context) {
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.black54,
        borderRadius: BorderRadius.circular(6),
      ),
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 6),
        child: Text(
          label,
          style: const TextStyle(
            color: Colors.white,
            fontSize: 12,
            fontWeight: FontWeight.w600,
          ),
        ),
      ),
    );
  }
}
