import 'dart:async';
import 'dart:io';

import 'package:bev_view/bev_view.dart';
import 'package:flutter/material.dart';

import 'bev_source.dart';

class SurroundView extends StatefulWidget {
  const SurroundView({super.key});

  @override
  State<SurroundView> createState() => _SurroundViewState();
}

class _SurroundViewState extends State<SurroundView> {
  static final BevSource _source = surroundSourceFromEnvironment();
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
      if (stats != null) {
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

  @override
  Widget build(BuildContext context) {
    return Stack(
      fit: StackFit.expand,
      children: <Widget>[
        BevView(source: _source, onCreated: _onCreated),
        if (_showStats)
          Positioned(
            right: 10,
            top: 10,
            child: IgnorePointer(child: _StatsPanel(stats: _stats)),
          ),
      ],
    );
  }
}

class _StatsPanel extends StatelessWidget {
  const _StatsPanel({required this.stats});

  final BevStats? stats;

  @override
  Widget build(BuildContext context) {
    final BevStats? s = stats;
    final List<String> lines = <String>[
      if (s == null) 'waiting for the view',
      if (s != null) ...<String>[
        '${s.fps.toStringAsFixed(1)} fps  ${s.running ? '' : '(source stopped)'}',
        'source ${s.sourceWidth}x${s.sourceHeight} -> view ${s.viewWidth}x${s.viewHeight}',
        '${s.grantedKindName}  ${s.fourccName} mod 0x${s.modifier.toRadixString(16)}',
        'buffers: ${s.allocatorName}',
        'fence waits ${s.releaseWaits} timeouts ${s.releaseTimeouts} '
            '(${s.releaseWaitMs.toStringAsFixed(0)} ms)',
        if (s.overlap > 0)
          'overlap ${s.overlap.toStringAsFixed(0)} deg  edge ${s.blendEdge.toStringAsFixed(2)}',
      ],
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
          style: const TextStyle(
            fontFamily: 'monospace',
            fontSize: 14,
            height: 1.4,
            color: Colors.white,
          ),
        ),
      ),
    );
  }
}
