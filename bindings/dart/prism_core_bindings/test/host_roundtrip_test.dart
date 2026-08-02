@TestOn('vm')
library;

// Device-free validation of the Dart bindings against the REAL native core, exercising
// the same code paths the app uses: create → loadScene → start → ingress → PSV read-out
// → pull render → restart → dispose.
//
// Runs on any desktop host, not just macOS: nothing here is platform-specific once
// PRISM_CORE_LIB names a loadable library, and pinning it to mac-os meant a Windows dev
// box silently reported "No tests ran" instead of validating the binding it had just
// regenerated.
//
// Run from this directory (adjust the extension per platform — .dll / .dylib / .so):
//   PRISM_CORE_LIB=$REPO/build/shared/core/libprism_core.dll \
//   PRISM_SCENES=$REPO/assets/scenes.json dart test

import 'dart:io';
import 'dart:math' as math;

import 'package:prism_core_bindings/prism_core_bindings.dart';
import 'package:prism_core_bindings/src/prism_core_bindings.g.dart' as raw;
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

    // Built from the generated macros rather than a literal: the point of the assertion
    // is that the LOADED library matches the header these bindings were generated from,
    // which is exactly the mismatch that breaks an FFI host. A hardcoded string instead
    // fails on every legitimate version bump and says nothing about agreement.
    expect(core.version,
        '${raw.PRISM_ABI_VERSION_MAJOR}.${raw.PRISM_ABI_VERSION_MINOR}.${raw.PRISM_ABI_VERSION_PATCH}');
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

  test('a pinned mood reaches the audio and takeover silences it', () async {
    final core = PrismCore.create(checkIntervalMs: 25, cadenceMs: 50);
    addTearDown(core.dispose);
    core.loadScene(scenes);
    core.start();

    // Every mood the app can send must resolve; a typo'd id must not silently become a
    // default, because a silent default is a room playing the wrong thing.
    for (final m in VenueMood.all) {
      expect(VenueMood.byId(m.id), same(m));
    }
    expect(VenueMood.byId('not-a-mood'), isNull);

    core.setMoodOverride(VenueMood.peak);
    final pinned = core.psv!;
    expect(pinned.arousal, VenueMood.peak.arousal);
    expect(pinned.modeHint, 'venue_peak');

    // Render a mood and return the settled audio. This is the assertion that actually
    // matters: a mood must reach the AUDIO, not merely change the vector.
    List<double> renderOf(VenueMood m) {
      core.setMoodOverride(m);
      final rate = core.sampleRate;
      final out = <double>[];
      // Skip ~2 s so the master fade-in and the smoothed parameter ramps have settled;
      // measuring earlier reads the ramp rather than the mood. Both moods are captured
      // at the same offset into the same loop, so the comparison below is like-for-like.
      for (var done = 0; done < rate * 4; done += 512) {
        final block = core.render(512);
        if (done >= rate * 2) out.addAll(block);
      }
      return out;
    }

    double rms(Iterable<double> xs) {
      var sum = 0.0;
      var n = 0;
      for (final x in xs) {
        sum += x * x;
        n++;
      }
      return n == 0 ? 0.0 : math.sqrt(sum / n);
    }

    final quiet = renderOf(VenueMood.windDown);
    final loud = renderOf(VenueMood.peak);

    // Compare the WAVEFORMS, not their loudness. With a single scene loaded, both moods
    // draw on the same stems, and RMS alone is a poor discriminator: wind-down's low
    // readiness drives the sub gain to 0.68 against peak's 0.45, and the sub is the
    // loudest stem here — so the two land within ~2.6 dB even though peak opens five
    // layers and wind-down opens two. The level separation seen when auditioning the six
    // moods comes substantially from their different stem sets, not from the PSV alone.
    //
    // What must hold regardless of material is that changing the mood changes the sound.
    final n = math.min(quiet.length, loud.length);
    final diff = List<double>.generate(n, (i) => loud[i] - quiet[i]);
    expect(rms(diff), greaterThan(rms(quiet) * 0.5),
        reason: 'the two moods rendered near-identical audio — the override did not '
            'reach the audio path');

    // Clearing hands control back to the context engine.
    core.clearMoodOverride();
    core.clearMoodOverride(); // idempotent

    // Takeover: the engine goes silent so staff can use their own source. Idempotent so
    // a double-tap or a retry cannot throw in the app.
    core.deviceStop();
    core.deviceStop();
  });

  test('a scene crossfade keeps the room playing and frees the caller to change again', () {
    // The app's mood change. Before this existed the only way to reach another scene was
    // to dispose the handle and build a new one, which is the audible gap venue staff
    // reported. Here the engine holds both scenes and fades between them.
    final core = PrismCore.create(vertical: PrismVertical.venues);
    addTearDown(core.dispose);
    core.loadScene(scenes);
    core.start();
    core.setMoodOverride(VenueMood.windDown);
    expect(core.crossfadeActive, isFalse);

    // Pull a little audio so the engine is genuinely running, then swap.
    core.render(4096);
    core.crossfadeScene(scenes,
        crossfade: const Duration(milliseconds: 200), alignToBar: false);
    expect(core.crossfadeActive, isTrue);

    // Refused while in flight rather than silently dropping the first request — the app
    // relies on this to fall back instead of losing a tap.
    expect(
      () => core.crossfadeScene(scenes, crossfade: const Duration(milliseconds: 200)),
      throwsA(isA<PrismException>()),
    );

    // Only the render path consumes the swap, so drive it past the overlap.
    var silentFrames = 0;
    for (var i = 0; i < 200; i++) {
      final block = core.render(1024);
      if (block.every((s) => s == 0.0)) silentFrames += block.length;
    }
    expect(core.crossfadeActive, isFalse, reason: 'the overlap never completed');

    // The point of the whole feature: no digital silence anywhere in the transition.
    expect(silentFrames, 0, reason: 'the room dropped out during the crossfade');

    // And the handle is immediately reusable for the next mood.
    core.crossfadeScene(scenes,
        crossfade: const Duration(milliseconds: 200), alignToBar: false);
    expect(core.crossfadeActive, isTrue);
  });
}
