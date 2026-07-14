@TestOn('mac-os')
library;

// Device-free validation of the Dart bindings against the REAL native core (the macOS
// dylib), exercising the same code paths the Android app uses: create → loadScene →
// start → ingress → PSV read-out → pull render → restart → dispose.
//
// Run from this directory:
//   PRISM_CORE_LIB=$REPO/build/debug/core/libprism_core.dylib \
//   PRISM_SCENES=$REPO/assets/scenes.json dart test

import 'dart:io';
import 'dart:math' as math;

import 'package:prism_core_bindings/prism_core_bindings.dart';
import 'package:test/test.dart';

void main() {
  final lib = Platform.environment['PRISM_CORE_LIB'];
  final scenes = Platform.environment['PRISM_SCENES'];
  if (lib == null || scenes == null) {
    test('host round trip (skipped)', () {
      markTestSkipped('set PRISM_CORE_LIB and PRISM_SCENES to run');
    });
    return;
  }

  test('full lifecycle through dart:ffi matches the ABI contract', () async {
    final core = PrismCore.create(checkIntervalMs: 25, cadenceMs: 100);
    addTearDown(core.dispose);

    expect(core.version, '0.1.0');
    expect(core.psv, isNull); // nothing emitted before start
    expect(() => core.start(), throwsA(isA<PrismException>())); // scene first

    core.loadScene(scenes);
    expect(core.sampleRate, greaterThan(0));
    core.start();

    // Cold-start neutral vector (spec §6).
    final neutral = core.psv;
    expect(neutral, isNotNull);
    expect(neutral!.sequence, 0);
    expect(neutral.arousal, 0.5);
    expect(neutral.arousalConfidence, 0.0);
    expect(neutral.modeHint, isNull);

    // Ingress drives fused emissions from the internal inference thread.
    final now = DateTime.now().millisecondsSinceEpoch;
    core.reportTaskDeadlines(
        [TaskDeadline(dueMs: now + 3600 * 1000, priority: TaskPriority.high)]);
    for (var i = 0; i < 24; i++) {
      core.reportIdle(now - (23 - i) * 5000, 0);
    }
    core.reportAppSwitch(now);

    Psv? fused;
    final deadline = DateTime.now().add(const Duration(seconds: 3));
    while (DateTime.now().isBefore(deadline)) {
      fused = core.psv;
      if (fused != null && fused.sequence >= 1 && fused.arousalConfidence > 0.3) break;
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }
    expect(fused!.sequence, greaterThanOrEqualTo(1));
    expect(fused.arousalConfidence, greaterThan(0.3));

    // Pull-model render through FFI: bounded, finite, limiter rail held.
    var peak = 0.0;
    for (var rendered = 0; rendered < core.sampleRate * 2; rendered += 512) {
      for (final s in core.render(512)) {
        expect(s.isFinite, isTrue);
        peak = math.max(peak, s.abs());
      }
    }
    expect(peak, greaterThan(0.01));
    expect(peak, lessThanOrEqualTo(math.pow(10, -3 / 20) + 1e-9));

    // Restart continues the sequence (ABI review finding, regression-covered here too).
    final beforeStop = core.psv!;
    core.stop();
    core.start();
    final restarted = core.psv!;
    expect(restarted.sequence, greaterThan(beforeStop.sequence));
    expect(restarted.arousal, 0.5);

    // A negative-length cast must surface as an error code, not kill the process.
    expect(() => core.reportTaskDeadlines(List.generate(5000, (_) => TaskDeadline(dueMs: now))),
        throwsA(isA<PrismException>()));
  });
}
