import 'package:flutter/material.dart';

import 'controls.dart';
import 'models.dart';

class SettingsPage extends StatefulWidget {
  const SettingsPage({
    super.key,
    required this.initialDeltas,
    required this.onSave,
    required this.onReset,
  });

  final Map<CameraId, CalDelta> initialDeltas;
  final void Function(CameraId camera, CalDelta delta) onSave;
  final ValueChanged<CameraId> onReset;

  @override
  State<SettingsPage> createState() => _SettingsPageState();
}

class _SettingsPageState extends State<SettingsPage> {
  late CameraId _camera = CameraId.front;
  late final Map<CameraId, CalDelta> _deltas = <CameraId, CalDelta>{
    for (final CameraId c in CameraId.values)
      c: CalDelta(
        x: widget.initialDeltas[c]?.x ?? 0,
        y: widget.initialDeltas[c]?.y ?? 0,
        yaw: widget.initialDeltas[c]?.yaw ?? 0,
      ),
  };

  void _nudge(CalAxis axis, int sign) => setState(() {
        final CalDelta d = _deltas[_camera]!;
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
      appBar: AppBar(title: const Text('Settings')),
      body: SafeArea(
        child: Padding(
          padding: const EdgeInsets.all(16),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: <Widget>[
              const Text(
                'Camera calibration',
                style: TextStyle(fontWeight: FontWeight.w700, fontSize: 16),
              ),
              const SizedBox(height: 12),
              CalibrationPanel(
                camera: _camera,
                delta: _deltas[_camera]!,
                onSelectCamera: (CameraId c) => setState(() => _camera = c),
                onNudge: _nudge,
                onSave: () => widget.onSave(_camera, _deltas[_camera]!),
                onReset: () => setState(() {
                  _deltas[_camera] = CalDelta();
                  widget.onReset(_camera);
                }),
              ),
            ],
          ),
        ),
      ),
    );
  }
}
