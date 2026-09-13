// The control chrome: a full-height left rail (overlays, snapshot, calibrate,
// settings), a bottom bar (view-mode segmented control + backend-link pill),
// and the calibration panel the rail reveals.
//
// Layout mock: every callback here is where a gRPC request RPC goes later.

import 'package:flutter/material.dart';

import 'models.dart';

const Color _accent = Color(0xFF3D7EFF);
const Color _barColor = Color(0xFF15151A);

// ---------------------------------------------------------------------------
// Left rail
// ---------------------------------------------------------------------------

class LeftRail extends StatelessWidget {
  const LeftRail({
    super.key,
    required this.overlays,
    required this.calibrating,
    required this.onToggleOverlay,
    required this.onToggleCalibrate,
    required this.onSnapshot,
    required this.onSettings,
  });

  final Set<CameraOverlay> overlays;
  final bool calibrating;
  final ValueChanged<CameraOverlay> onToggleOverlay;
  final VoidCallback onToggleCalibrate;
  final VoidCallback onSnapshot;
  final VoidCallback onSettings;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 96,
      color: _barColor,
      child: Column(
        children: <Widget>[
          const SizedBox(height: 10),
          _RailButton(
            icon: Icons.timeline,
            label: 'Guides',
            active: overlays.contains(CameraOverlay.guidelines),
            onTap: () => onToggleOverlay(CameraOverlay.guidelines),
          ),
          _RailButton(
            icon: Icons.grid_on,
            label: 'Distance',
            active: overlays.contains(CameraOverlay.distanceGrid),
            onTap: () => onToggleOverlay(CameraOverlay.distanceGrid),
          ),
          const Divider(
            height: 20,
            indent: 16,
            endIndent: 16,
            color: Colors.white12,
          ),
          _RailButton(
            icon: Icons.photo_camera,
            label: 'Snapshot',
            onTap: onSnapshot,
          ),
          _RailButton(
            icon: Icons.tune,
            label: 'Calibrate',
            active: calibrating,
            onTap: onToggleCalibrate,
          ),
          const Spacer(),
          _RailButton(
            icon: Icons.settings,
            label: 'Settings',
            onTap: onSettings,
          ),
          const SizedBox(height: 10),
        ],
      ),
    );
  }
}

class _RailButton extends StatelessWidget {
  const _RailButton({
    required this.icon,
    required this.label,
    required this.onTap,
    this.active = false,
  });

  final IconData icon;
  final String label;
  final VoidCallback onTap;
  final bool active;

  @override
  Widget build(BuildContext context) {
    final Color fg = active ? Colors.white : Colors.white70;
    return Padding(
      padding: const EdgeInsets.fromLTRB(8, 4, 8, 4),
      child: Material(
        color: active ? _accent : Colors.white10,
        borderRadius: BorderRadius.circular(10),
        child: InkWell(
          borderRadius: BorderRadius.circular(10),
          onTap: onTap,
          child: Padding(
            padding: const EdgeInsets.symmetric(vertical: 10),
            child: Column(
              children: <Widget>[
                Icon(icon, size: 22, color: fg),
                const SizedBox(height: 4),
                Text(label, style: TextStyle(fontSize: 10, color: fg)),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Bottom bar
// ---------------------------------------------------------------------------

class BottomBar extends StatelessWidget {
  const BottomBar({
    super.key,
    required this.mode,
    required this.onModeChanged,
    this.backendConnected = false,
  });

  final ViewMode mode;
  final ValueChanged<ViewMode> onModeChanged;
  final bool backendConnected;

  @override
  Widget build(BuildContext context) {
    return Container(
      height: 76,
      color: _barColor,
      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
      child: Row(
        children: <Widget>[
          Expanded(
            child: SingleChildScrollView(
              scrollDirection: Axis.horizontal,
              child: Row(
                children: <Widget>[
                  for (final ViewMode m in ViewMode.values) ...<Widget>[
                    _SegItem(
                      label: m.label,
                      icon: _modeIcon(m),
                      selected: m == mode,
                      onTap: () => onModeChanged(m),
                    ),
                    const SizedBox(width: 6),
                  ],
                ],
              ),
            ),
          ),
          const SizedBox(width: 12),
          _BackendPill(connected: backendConnected),
        ],
      ),
    );
  }
}

IconData _modeIcon(ViewMode m) => switch (m) {
      ViewMode.grid => Icons.grid_view,
      ViewMode.front => Icons.arrow_upward,
      ViewMode.rear => Icons.arrow_downward,
      ViewMode.left => Icons.arrow_back,
      ViewMode.right => Icons.arrow_forward,
      ViewMode.bev => Icons.directions_car,
    };

class _SegItem extends StatelessWidget {
  const _SegItem({
    required this.label,
    required this.icon,
    required this.selected,
    required this.onTap,
  });

  final String label;
  final IconData icon;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final Color fg = selected ? Colors.white : Colors.white70;
    return Material(
      color: selected ? _accent : Colors.white10,
      borderRadius: BorderRadius.circular(8),
      child: InkWell(
        borderRadius: BorderRadius.circular(8),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
          child: Row(
            children: <Widget>[
              Icon(icon, size: 18, color: fg),
              const SizedBox(width: 6),
              Text(
                label,
                style: TextStyle(color: fg, fontWeight: FontWeight.w600),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _BackendPill extends StatelessWidget {
  const _BackendPill({required this.connected});

  final bool connected;

  @override
  Widget build(BuildContext context) {
    final Color c = connected ? const Color(0xFF43A047) : const Color(0xFFFFB300);
    return Tooltip(
      message: connected
          ? 'gRPC control link up'
          : 'gRPC WatchState — not wired yet',
      child: Row(
        mainAxisSize: MainAxisSize.min,
        children: <Widget>[
          Container(
            width: 8,
            height: 8,
            decoration: BoxDecoration(color: c, shape: BoxShape.circle),
          ),
          const SizedBox(width: 6),
          Text(
            connected ? 'backend' : 'mock',
            style: const TextStyle(color: Colors.white54, fontSize: 11),
          ),
        ],
      ),
    );
  }
}

// ---------------------------------------------------------------------------
// Calibration panel (revealed by the rail's Calibrate button)
// ---------------------------------------------------------------------------

class CalibrationPanel extends StatelessWidget {
  const CalibrationPanel({
    super.key,
    required this.camera,
    required this.delta,
    required this.onSelectCamera,
    required this.onNudge,
    required this.onSave,
    required this.onReset,
  });

  final CameraId camera;
  final CalDelta delta;
  final ValueChanged<CameraId> onSelectCamera;
  final void Function(CalAxis axis, int sign) onNudge;
  final VoidCallback onSave;
  final VoidCallback onReset;

  @override
  Widget build(BuildContext context) {
    return Container(
      width: 268,
      padding: const EdgeInsets.all(12),
      decoration: BoxDecoration(
        color: const Color(0xFF1B1B21),
        borderRadius: BorderRadius.circular(10),
        border: Border.all(color: Colors.white12),
      ),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: <Widget>[
          const Text(
            'Calibration',
            style: TextStyle(fontWeight: FontWeight.w700),
          ),
          const SizedBox(height: 8),
          Wrap(
            spacing: 4,
            runSpacing: 4,
            children: <Widget>[
              for (final CameraId c in CameraId.values)
                _CamChip(
                  label: c.label,
                  selected: c == camera,
                  onTap: () => onSelectCamera(c),
                ),
            ],
          ),
          const SizedBox(height: 10),
          _NudgeRow(
            label: 'X',
            onMinus: () => onNudge(CalAxis.x, -1),
            onPlus: () => onNudge(CalAxis.x, 1),
          ),
          _NudgeRow(
            label: 'Y',
            onMinus: () => onNudge(CalAxis.y, -1),
            onPlus: () => onNudge(CalAxis.y, 1),
          ),
          _NudgeRow(
            label: 'Yaw',
            onMinus: () => onNudge(CalAxis.yaw, -1),
            onPlus: () => onNudge(CalAxis.yaw, 1),
          ),
          const SizedBox(height: 8),
          Text(
            'x ${delta.x.toStringAsFixed(1)}   '
            'y ${delta.y.toStringAsFixed(1)}   '
            'yaw ${delta.yaw.toStringAsFixed(1)}°',
            style: const TextStyle(color: Colors.white54, fontSize: 11),
          ),
          const SizedBox(height: 10),
          Row(
            children: <Widget>[
              Expanded(
                child: FilledButton(
                  onPressed: onSave,
                  child: const Text('Save'),
                ),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: OutlinedButton(
                  onPressed: onReset,
                  child: const Text('Reset'),
                ),
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _CamChip extends StatelessWidget {
  const _CamChip({
    required this.label,
    required this.selected,
    required this.onTap,
  });

  final String label;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: selected ? _accent : Colors.white10,
      borderRadius: BorderRadius.circular(6),
      child: InkWell(
        borderRadius: BorderRadius.circular(6),
        onTap: onTap,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 6),
          child: Text(
            label,
            style: TextStyle(
              fontSize: 11,
              color: selected ? Colors.white : Colors.white70,
            ),
          ),
        ),
      ),
    );
  }
}

class _NudgeRow extends StatelessWidget {
  const _NudgeRow({
    required this.label,
    required this.onMinus,
    required this.onPlus,
  });

  final String label;
  final VoidCallback onMinus;
  final VoidCallback onPlus;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 3),
      child: Row(
        children: <Widget>[
          SizedBox(
            width: 40,
            child: Text(label, style: const TextStyle(color: Colors.white70)),
          ),
          _SquareBtn(icon: Icons.remove, onTap: onMinus),
          const SizedBox(width: 6),
          _SquareBtn(icon: Icons.add, onTap: onPlus),
        ],
      ),
    );
  }
}

class _SquareBtn extends StatelessWidget {
  const _SquareBtn({required this.icon, required this.onTap});

  final IconData icon;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    return Material(
      color: Colors.white10,
      borderRadius: BorderRadius.circular(6),
      child: InkWell(
        borderRadius: BorderRadius.circular(6),
        onTap: onTap,
        child: SizedBox(
          width: 34,
          height: 30,
          child: Center(child: Icon(icon, size: 18)),
        ),
      ),
    );
  }
}
