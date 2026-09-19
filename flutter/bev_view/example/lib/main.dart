import 'dart:async';
import 'dart:io';

import 'package:bev_view/bev_view.dart';
import 'package:flutter/material.dart';

/// The source, chosen by environment variables.
///
/// The bundle builder compiles only this entrypoint and the shell passes the
/// app no arguments, so the environment is the one channel that reaches it
/// through the normal build and deploy path.
///
///   BEV_MODE        pattern (default) | camera | file | surround
///   BEV_CAMERA      camera index                               (camera)
///   BEV_CAM_FPS     pin the sensor's frame rate                (camera)
///   BEV_TUNING_FILE IPA tuning JSON                            (camera)
///   BEV_FILE        HEVC recording                             (file)
///   BEV_FILE_BEV    1 = forward bird's-eye view                (file)
///   BEV_SRC         recordings, ':'-separated, in slot order   (surround)
///   BEV_FORWARD_LEFT / _RIGHT, BEV_BACKWARD_LEFT / _RIGHT      (surround)
///   BEV_CONFIG      bev_config.ini path                        (surround)
///   BEV_LENS_DIR, BEV_BLEND, BEV_PX_PER_M, BEV_CAR_ICON        (surround)
///   BEV_FIT         contain (default) | cover | fill
///   BEV_SCANOUT     1 = take a KMS plane when the shell offers one
BevSource sourceFromEnvironment(Map<String, String> env) {
  String? get(String key) {
    final value = env[key];
    return value == null || value.isEmpty ? null : value;
  }

  switch (get('BEV_MODE') ?? 'pattern') {
    case 'camera':
      return BevCameraSource(
        index: int.tryParse(get('BEV_CAMERA') ?? '') ?? 0,
        fps: double.tryParse(get('BEV_CAM_FPS') ?? ''),
        tuningFile: get('BEV_TUNING_FILE'),
      );
    case 'file':
      final path = get('BEV_FILE');
      if (path == null) throw StateError('BEV_MODE=file needs BEV_FILE');
      return BevFileSource(
        path,
        bev: get('BEV_FILE_BEV') == '1' ? const BevGroundPose() : null,
      );
    case 'surround':
      final blend = BevBlend.values.asNameMap()[get('BEV_BLEND') ?? 'feather'] ??
          BevBlend.feather;
      final carIcon = get('BEV_CAR_ICON');
      final common = (
        config: get('BEV_CONFIG') ?? 'bev_config.ini',
        lensDir: get('BEV_LENS_DIR'),
        pxPerMeter: double.tryParse(get('BEV_PX_PER_M') ?? ''),
        carIcon: carIcon == null ? null : BevCarIcon(carIcon),
      );
      final src = get('BEV_SRC');
      if (src != null) {
        return BevSurroundSource(
          src.split(':'),
          config: common.config,
          lensDir: common.lensDir,
          blend: blend,
          pxPerMeter: common.pxPerMeter,
          carIcon: common.carIcon,
        );
      }
      return BevSurroundSource.pairs(
        forwardLeft: get('BEV_FORWARD_LEFT'),
        forwardRight: get('BEV_FORWARD_RIGHT'),
        backwardLeft: get('BEV_BACKWARD_LEFT'),
        backwardRight: get('BEV_BACKWARD_RIGHT'),
        config: common.config,
        lensDir: common.lensDir,
        blend: blend,
        pxPerMeter: common.pxPerMeter,
        carIcon: common.carIcon,
      );
    case 'pattern':
      // BEV_PATTERN_FPS=0 runs it uncapped, to measure the ceiling.
      return BevPatternSource(
        fps: double.tryParse(get('BEV_PATTERN_FPS') ?? '') ?? 30,
      );
    case final other:
      throw StateError('unknown BEV_MODE "$other"');
  }
}

void main() {
  runZonedGuarded(
    () {
      WidgetsFlutterBinding.ensureInitialized();
      final env = Platform.environment;
      runApp(
        BevExampleApp(
          source: sourceFromEnvironment(env),
          fit: BevFit.values.asNameMap()[env['BEV_FIT']] ?? BevFit.contain,
          scanout: env['BEV_SCANOUT'] == '1',
          // Ring depth, for trying more slack against a compositor that is
          // slow to release a buffer (BEV_RING=4).
          ringSize: int.tryParse(env['BEV_RING'] ?? '') ?? 4,
        ),
      );
    },
    (error, stack) {
      stdout.write('runZonedGuarded caught: $error\n$stack\n');
    },
  );
}

class BevExampleApp extends StatefulWidget {
  const BevExampleApp({
    super.key,
    required this.source,
    this.fit = BevFit.contain,
    this.scanout = false,
    this.ringSize = 3,
  });

  final BevSource source;
  final BevFit fit;
  final bool scanout;

  /// dma-bufs in the view's ring (BEV_RING), 4 by default. More slack for a
  /// compositor that is slow to release a slot; watch `fence … timeouts` in
  /// the overlay (or BEV_STAT=1) to see whether it is enough.
  final int ringSize;

  @override
  State<BevExampleApp> createState() => _BevExampleAppState();
}

class _BevExampleAppState extends State<BevExampleApp> {
  BevViewController? _controller;
  BevStats? _stats;
  Timer? _poll;
  bool _freeYaw = false;
  int _snapshots = 0;
  String? _notice;

  bool get _surround => widget.source is BevSurroundSource;

  @override
  void dispose() {
    _poll?.cancel();
    super.dispose();
  }

  /// BEV_STAT=1 also prints each sample. The HUD is on the board's panel; a
  /// headless run (or one watched over ssh) has no other way to read it.
  static final bool _statToStdout = Platform.environment['BEV_STAT'] == '1';

  void _onCreated(BevViewController controller) {
    _controller = controller;
    // Counters live natively; a low poll rate costs nothing.
    _poll = Timer.periodic(const Duration(milliseconds: 500), (_) {
      final stats = controller.stats();
      setState(() => _stats = stats);
      if (_statToStdout && stats != null) {
        stdout.write(
          '[stat] ${stats.fps.toStringAsFixed(1)} fps  '
          '${stats.sourceWidth}x${stats.sourceHeight} -> '
          '${stats.viewWidth}x${stats.viewHeight}  '
          '${stats.grantedKindName} ${stats.fourccName} '
          'mod 0x${stats.modifier.toRadixString(16)} via ${stats.allocatorName}  '
          'submitted ${stats.framesSubmitted}  '
          'fence waits ${stats.releaseWaits} timeouts ${stats.releaseTimeouts} '
          '(${stats.releaseWaitMs.toStringAsFixed(0)} ms)\n',
        );
      }
    });
  }

  void _snapshot() {
    final path = '/tmp/bev_snapshot_${(_snapshots++).toString().padLeft(3, '0')}.png';
    final queued = _controller?.snapshot(path) ?? false;
    setState(() => _notice = queued ? 'snapshot -> $path' : 'snapshot not queued');
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      debugShowCheckedModeBanner: false,
      theme: ThemeData.dark(),
      home: Scaffold(
        backgroundColor: Colors.black,
        body: Stack(
          fit: StackFit.expand,
          children: [
            BevView(
              source: widget.source,
              fit: widget.fit,
              scanout: widget.scanout,
              ringSize: widget.ringSize,
              onCreated: _onCreated,
            ),
            Positioned(
              left: 16,
              top: 16,
              child: IgnorePointer(child: _StatsPanel(stats: _stats, notice: _notice)),
            ),
            Positioned(
              right: 16,
              bottom: 16,
              child: _Controls(
                enabled: _controller != null,
                surround: _surround,
                freeYaw: _freeYaw,
                onOverlap: (delta) {
                  final current = _stats?.overlap ?? 0;
                  _controller?.setOverlap(current + delta);
                },
                onBlendEdge: (delta) {
                  final current = _stats?.blendEdge ?? 0;
                  _controller?.setBlendEdge(current + delta);
                },
                onFreeYaw: () {
                  setState(() => _freeYaw = !_freeYaw);
                  _controller?.setFreeYaw(_freeYaw);
                },
                onSnapshot: _snapshot,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _StatsPanel extends StatelessWidget {
  const _StatsPanel({required this.stats, required this.notice});

  final BevStats? stats;
  final String? notice;

  @override
  Widget build(BuildContext context) {
    final s = stats;
    final lines = <String>[
      if (s == null) 'waiting for the view',
      if (s != null) ...[
        '${s.fps.toStringAsFixed(1)} fps  ${s.running ? '' : '(source stopped)'}',
        'source ${s.sourceWidth}x${s.sourceHeight} -> view ${s.viewWidth}x${s.viewHeight}',
        '${s.grantedKindName}  ${s.fourccName} mod 0x${s.modifier.toRadixString(16)}',
        'buffers: ${s.allocatorName}',
        // Timeouts here are the compositor never releasing a slot, which is
        // what a tear looks like from this side.
        'fence waits ${s.releaseWaits} timeouts ${s.releaseTimeouts} '
            '(${s.releaseWaitMs.toStringAsFixed(0)} ms)',
        if (s.overlap > 0)
          'overlap ${s.overlap.toStringAsFixed(0)} deg  edge ${s.blendEdge.toStringAsFixed(2)}',
      ],
      ?notice,
    ];
    return DecoratedBox(
      decoration: BoxDecoration(
        color: Colors.black54,
        borderRadius: BorderRadius.circular(8),
      ),
      child: Padding(
        padding: const EdgeInsets.all(12),
        child: Text(
          lines.join('\n'),
          style: const TextStyle(fontFamily: 'monospace', fontSize: 14, height: 1.4),
        ),
      ),
    );
  }
}

class _Controls extends StatelessWidget {
  const _Controls({
    required this.enabled,
    required this.surround,
    required this.freeYaw,
    required this.onOverlap,
    required this.onBlendEdge,
    required this.onFreeYaw,
    required this.onSnapshot,
  });

  final bool enabled;
  final bool surround;
  final bool freeYaw;
  final ValueChanged<double> onOverlap;
  final ValueChanged<double> onBlendEdge;
  final VoidCallback onFreeYaw;
  final VoidCallback onSnapshot;

  @override
  Widget build(BuildContext context) {
    return Card(
      color: Colors.black54,
      child: Padding(
        padding: const EdgeInsets.all(8),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            // The same steps as the CLI's +/- and [/] keys.
            if (surround) ...[
              _Stepper(label: 'overlap', enabled: enabled, onStep: (s) => onOverlap(5.0 * s)),
              _Stepper(label: 'edge', enabled: enabled, onStep: (s) => onBlendEdge(0.01 * s)),
              TextButton(
                onPressed: enabled ? onFreeYaw : null,
                child: Text(freeYaw ? 'free yaw: on' : 'free yaw: off'),
              ),
            ],
            FilledButton.icon(
              onPressed: enabled ? onSnapshot : null,
              icon: const Icon(Icons.camera_alt),
              label: const Text('snapshot'),
            ),
          ],
        ),
      ),
    );
  }
}

class _Stepper extends StatelessWidget {
  const _Stepper({required this.label, required this.enabled, required this.onStep});

  final String label;
  final bool enabled;
  final ValueChanged<int> onStep;

  @override
  Widget build(BuildContext context) {
    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        Text(label),
        IconButton(
          onPressed: enabled ? () => onStep(-1) : null,
          icon: const Icon(Icons.remove),
        ),
        IconButton(
          onPressed: enabled ? () => onStep(1) : null,
          icon: const Icon(Icons.add),
        ),
      ],
    );
  }
}
