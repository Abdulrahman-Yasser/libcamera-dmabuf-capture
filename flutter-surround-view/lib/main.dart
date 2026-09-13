import 'package:flutter/material.dart';

import 'controls.dart';
import 'models.dart';
import 'viewport.dart';

/// Entry point.
///
/// Same shape as the AGL `camera_streams_app` demo: an async `main` that makes
/// sure the Flutter bindings exist before any plugin / async work runs, then
/// hands a single root widget to `runApp`.
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

/// Layout mock.
///
/// Every button drives local state here. When the gRPC control channel lands,
/// each handler sends a request RPC and this state is replaced by whatever the
/// backend streams back over `WatchState`.
class SurroundViewScreen extends StatefulWidget {
  const SurroundViewScreen({super.key});

  @override
  State<SurroundViewScreen> createState() => _SurroundViewScreenState();
}

class _SurroundViewScreenState extends State<SurroundViewScreen> {
  ViewMode _mode = ViewMode.grid;
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

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: SafeArea(
        child: Row(
          children: <Widget>[
            LeftRail(
              overlays: _overlays,
              calibrating: _calibrating,
              onToggleOverlay: _toggleOverlay,
              onToggleCalibrate: () =>
                  setState(() => _calibrating = !_calibrating),
              onSnapshot: () => _toast('Snapshot saved (mock)'),
              onSettings: () => _toast('Settings (mock)'),
            ),
            Expanded(
              child: Column(
                children: <Widget>[
                  Expanded(
                    child: Stack(
                      children: <Widget>[
                        Positioned.fill(
                          child: CameraViewport(
                            mode: _mode,
                            overlays: _overlays,
                          ),
                        ),
                        Positioned(
                          left: 10,
                          top: 10,
                          child: _ModeTag(mode: _mode),
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
                              onReset: () => setState(
                                () => _deltas[_calibCamera] = CalDelta(),
                              ),
                            ),
                          ),
                      ],
                    ),
                  ),
                  BottomBar(
                    mode: _mode,
                    onModeChanged: (ViewMode m) => setState(() => _mode = m),
                  ),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _ModeTag extends StatelessWidget {
  const _ModeTag({required this.mode});

  final ViewMode mode;

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
          '${mode.label} view',
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
