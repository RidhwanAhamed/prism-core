// Task 6 smoke app (build plan): start/stop, live PSV readout, audio playing — the
// Prism Engine core running on a physical device through dart:ffi.
//
// The shell's job is deliberately thin (CLAUDE.md: bindings capture and forward raw
// events only): buttons simulate the behavior a real capture layer would observe
// (focused / scattered / away), a timer forwards those events through prism_report_*,
// and the UI polls prism_get_psv. All inference happens in the native core; all audio
// happens on the core's own device thread.

import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:flutter/services.dart' show rootBundle;
import 'package:path_provider/path_provider.dart';
import 'package:prism_core_bindings/prism_core_bindings.dart';

void main() {
  runApp(const PrismSmokeApp());
}

class PrismSmokeApp extends StatelessWidget {
  const PrismSmokeApp({super.key});

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      title: 'Prism Smoke',
      theme: ThemeData(
        colorScheme: ColorScheme.fromSeed(
          seedColor: const Color(0xFF5E7CE2),
          brightness: Brightness.dark,
        ),
        useMaterial3: true,
      ),
      home: const SmokeScreen(),
    );
  }
}

/// The behavior the simulated capture layer feeds the engine.
enum Behavior { focused, scattered, away }

class SmokeScreen extends StatefulWidget {
  const SmokeScreen({super.key});

  @override
  State<SmokeScreen> createState() => _SmokeScreenState();
}

class _SmokeScreenState extends State<SmokeScreen> {
  PrismCore? _core;
  String _status = 'extracting scene assets…';
  String? _scenesPath;
  bool _running = false;
  Behavior _behavior = Behavior.focused;
  Psv? _psv;
  int _tick = 0;
  Timer? _ingressTimer;
  Timer? _uiTimer;
  DateTime? _startedAt;

  @override
  void initState() {
    super.initState();
    _prepare();
  }

  // prism_load_scene needs real filesystem paths; Android assets are not files, so the
  // bundled scene is written out once to the app's documents directory.
  Future<void> _prepare() async {
    try {
      final dir = await getApplicationSupportDirectory();
      final sceneDir = Directory('${dir.path}/scene')..createSync(recursive: true);
      Directory('${sceneDir.path}/stems').createSync(recursive: true);
      const files = [
        'scenes.json',
        'stems/air.wav',
        'stems/bed.wav',
        'stems/lead.wav',
        'stems/pulse.wav',
        'stems/sub.wav',
      ];
      for (final name in files) {
        final out = File('${sceneDir.path}/$name');
        final data = await rootBundle.load('assets/scene/$name');
        // Re-extract on size mismatch too: a partial write from an interrupted first
        // launch would otherwise fail scene loading on every run (review finding).
        if (!out.existsSync() || out.lengthSync() != data.lengthInBytes) {
          await out.writeAsBytes(data.buffer.asUint8List(), flush: true);
        }
      }
      setState(() {
        _scenesPath = '${sceneDir.path}/scenes.json';
        _status = 'ready';
      });
    } catch (e) {
      setState(() => _status = 'asset extraction failed: $e');
    }
  }

  void _start() {
    if (_scenesPath == null || _running) return;
    // Declared OUTSIDE the try: on any failure the just-created engine must be disposed,
    // or its native inference thread keeps running invisibly — one leaked engine per
    // retry (adversarial review reproduced this against the real core).
    PrismCore? core;
    try {
      core = PrismCore.create(
        tzOffsetMin: -DateTime.now().timeZoneOffset.inMinutes,
      );
      core.loadScene(_scenesPath!);
      // One synthetic deadline tomorrow so deadline pressure participates.
      core.reportTaskDeadlines([
        TaskDeadline(
          dueMs: DateTime.now().millisecondsSinceEpoch + 24 * 3600 * 1000,
          priority: TaskPriority.high,
        ),
      ]);
      core.start();
      core.deviceStart();

      _core = core;
      _startedAt = DateTime.now();
      _tick = 0;
      // The simulated capture layer: one sample every 5s, like the real shells.
      _ingressTimer = Timer.periodic(const Duration(seconds: 5), (_) => _ingress());
      _uiTimer = Timer.periodic(const Duration(seconds: 1), (_) => _poll());
      setState(() {
        _running = true;
        _status = 'running | ${core.sampleRate} Hz | core ${core.version}';
      });
      _poll();
    } on PrismException catch (e) {
      setState(() => _status = 'start failed: $e');
      core?.dispose(); // the local engine, not the (still-null) field
      _core = null;
    }
  }

  void _ingress() {
    final core = _core;
    if (core == null) return;
    final t = DateTime.now().millisecondsSinceEpoch;
    _tick++;
    switch (_behavior) {
      case Behavior.focused:
        core.reportIdle(t, (_tick % 3) * 700); // sub-threshold idle noise
      case Behavior.scattered:
        core.reportIdle(t, 500);
        core.reportAppSwitch(t); // a switch every capture tick
      case Behavior.away:
        core.reportIdle(t, 20000 + (_tick % 6) * 5000); // long idle
    }
  }

  void _poll() {
    final core = _core;
    if (core == null) return;
    final psv = core.psv;
    if (psv != null && psv.sequence != _psv?.sequence) {
      setState(() => _psv = psv);
    }
  }

  void _stop() {
    _ingressTimer?.cancel();
    _uiTimer?.cancel();
    _core?.dispose(); // stops device + inference internally
    _core = null;
    setState(() {
      _running = false;
      _status = 'stopped';
    });
  }

  @override
  void dispose() {
    _ingressTimer?.cancel();
    _uiTimer?.cancel();
    _core?.dispose();
    _core = null;
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final psv = _psv;
    final uptime =
        _startedAt == null || !_running ? null : DateTime.now().difference(_startedAt!);
    return Scaffold(
      appBar: AppBar(title: const Text('Prism Engine — smoke')),
      body: Padding(
        padding: const EdgeInsets.all(16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.stretch,
          children: [
            Text(_status, style: Theme.of(context).textTheme.bodySmall),
            if (uptime != null)
              Text('uptime ${uptime.inMinutes}m ${uptime.inSeconds % 60}s '
                  '(budget run: 30m)'),
            const SizedBox(height: 16),
            FilledButton.icon(
              onPressed: _scenesPath == null ? null : (_running ? _stop : _start),
              icon: Icon(_running ? Icons.stop : Icons.play_arrow),
              label: Text(_running ? 'Stop' : 'Start'),
            ),
            const SizedBox(height: 16),
            SegmentedButton<Behavior>(
              segments: const [
                ButtonSegment(value: Behavior.focused, label: Text('Focused')),
                ButtonSegment(value: Behavior.scattered, label: Text('Scattered')),
                ButtonSegment(value: Behavior.away, label: Text('Away')),
              ],
              selected: {_behavior},
              onSelectionChanged: (s) => setState(() => _behavior = s.first),
            ),
            const SizedBox(height: 24),
            if (psv == null)
              const Text('no PSV yet')
            else ...[
              Text('PSV seq ${psv.sequence}'
                  '${psv.modeHint != null ? ' | hint ${psv.modeHint}' : ''}'),
              const SizedBox(height: 8),
              _DimensionBar('arousal', psv.arousal, psv.arousalConfidence),
              _DimensionBar('valence', psv.valence, psv.valenceConfidence),
              _DimensionBar(
                  'cognitive load', psv.cognitiveLoad, psv.cognitiveLoadConfidence),
              _DimensionBar('readiness', psv.readiness, psv.readinessConfidence),
            ],
            const Spacer(),
            Text(
              'behavior feeds the engine every 5s; the PSV updates on the ~30s cadence '
              'or on significant change, and the audio adapts at loop boundaries.',
              style: Theme.of(context).textTheme.bodySmall,
            ),
          ],
        ),
      ),
    );
  }
}

class _DimensionBar extends StatelessWidget {
  const _DimensionBar(this.name, this.value, this.confidence);

  final String name;
  final double value;
  final double confidence;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 6),
      child: Row(
        children: [
          SizedBox(width: 110, child: Text(name)),
          Expanded(
            child: LinearProgressIndicator(
              value: value,
              minHeight: 10,
              // Confidence dims the bar: an inert dimension reads as faint (spec §4.2).
              color: Theme.of(context)
                  .colorScheme
                  .primary
                  .withValues(alpha: 0.25 + 0.75 * confidence),
            ),
          ),
          SizedBox(
            width: 92,
            child: Text(
              ' ${value.toStringAsFixed(2)} @${confidence.toStringAsFixed(2)}',
              textAlign: TextAlign.right,
            ),
          ),
        ],
      ),
    );
  }
}
