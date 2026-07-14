/// Idiomatic Dart wrapper over the Prism Engine core C ABI.
///
/// The raw ffigen output lives in `src/prism_core_bindings.g.dart`; this file is the
/// surface Flutter apps use. It mirrors the ABI's contract exactly (see
/// include/prism/prism_core.h for lifecycle and threading rules) and converts error
/// codes into [PrismException]s.
library;

import 'dart:ffi' as ffi;
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'src/prism_core_bindings.g.dart' as raw;

export 'src/prism_core_bindings.g.dart' show prism_result;

/// Spec §4.1 dimensions with per-dimension confidence, plus emission metadata.
class Psv {
  const Psv({
    required this.arousal,
    required this.arousalConfidence,
    required this.valence,
    required this.valenceConfidence,
    required this.cognitiveLoad,
    required this.cognitiveLoadConfidence,
    required this.readiness,
    required this.readinessConfidence,
    required this.updateTimestampMs,
    required this.sequence,
    required this.modeHint,
  });

  final double arousal, arousalConfidence;
  final double valence, valenceConfidence;
  final double cognitiveLoad, cognitiveLoadConfidence;
  final double readiness, readinessConfidence;
  final int updateTimestampMs;

  /// Strictly monotonic per engine instance; the first cold-start vector is 0.
  final int sequence;

  /// Null when the engine emitted no hint (spec §9).
  final String? modeHint;

  @override
  String toString() =>
      'Psv(seq=$sequence a=${arousal.toStringAsFixed(2)}@${arousalConfidence.toStringAsFixed(2)} '
      'l=${cognitiveLoad.toStringAsFixed(2)}@${cognitiveLoadConfidence.toStringAsFixed(2)} '
      'r=${readiness.toStringAsFixed(2)}@${readinessConfidence.toStringAsFixed(2)})';
}

enum PrismVertical { aqademiq, venues, automotive }

enum TaskPriority { low, medium, high }

class TaskDeadline {
  const TaskDeadline({required this.dueMs, this.priority = TaskPriority.medium});
  final int dueMs; // epoch ms
  final TaskPriority priority;
}

class PrismException implements Exception {
  PrismException(this.result, this.operation);
  final raw.prism_result result;
  final String operation;
  @override
  String toString() => 'PrismException($operation: ${result.name})';
}

/// One engine instance behind the opaque handle. Not thread-safe against itself for
/// lifecycle calls — same contract as the C header. Use from the main isolate; the
/// engine runs its own native inference thread and (with [deviceStart]) audio thread.
class PrismCore {
  PrismCore._(this._b, this._core);

  /// Creates an engine. All arguments mirror `prism_config`; zeros mean ABI defaults.
  factory PrismCore.create({
    int tzOffsetMin = 0,
    PrismVertical vertical = PrismVertical.aqademiq,
    int cadenceMs = 0,
    int checkIntervalMs = 0,
    double significantDelta = 0,
    ffi.DynamicLibrary? library,
  }) {
    final b = raw.PrismCoreBindings(library ?? defaultLibrary());
    final config = calloc<raw.prism_config>();
    final out = calloc<ffi.Pointer<raw.prism_core>>();
    try {
      config.ref.tz_offset_min = tzOffsetMin;
      config.ref.vertical = vertical.index;
      config.ref.cadence_ms = cadenceMs;
      config.ref.check_interval_ms = checkIntervalMs;
      config.ref.significant_delta = significantDelta;
      _check(b.prism_create(config, out), 'prism_create');
      return PrismCore._(b, out.value);
    } finally {
      calloc.free(config);
      calloc.free(out);
    }
  }

  /// Resolution order: PRISM_CORE_LIB env override (host tests), then the platform's
  /// conventional location (Android .so packaged by gradle; iOS statically linked —
  /// Task 7; macOS dylib on the loader path).
  static ffi.DynamicLibrary defaultLibrary() {
    final override = Platform.environment['PRISM_CORE_LIB'];
    if (override != null && override.isNotEmpty) {
      return ffi.DynamicLibrary.open(override);
    }
    if (Platform.isAndroid) return ffi.DynamicLibrary.open('libprism_core.so');
    if (Platform.isIOS) {
      // The core ships as an embedded dynamic framework (Task 7); dyld resolves the
      // rpath name. process() covers a statically linked future variant.
      try {
        return ffi.DynamicLibrary.open('prism_core.framework/prism_core');
      } on ArgumentError {
        return ffi.DynamicLibrary.process();
      }
    }
    if (Platform.isMacOS) return ffi.DynamicLibrary.open('libprism_core.dylib');
    throw UnsupportedError('no prism_core library location for this platform');
  }

  final raw.PrismCoreBindings _b;
  ffi.Pointer<raw.prism_core> _core;

  static void _check(raw.prism_result result, String operation) {
    if (result != raw.prism_result.PRISM_OK) {
      throw PrismException(result, operation);
    }
  }

  String get version {
    final b = _b; // version is instance-independent but needs the lookup table
    return b.prism_version().cast<Utf8>().toDartString();
  }

  void loadScene(String scenesJsonPath) {
    final path = scenesJsonPath.toNativeUtf8();
    try {
      _check(_b.prism_load_scene(_core, path.cast()), 'prism_load_scene');
    } finally {
      calloc.free(path);
    }
  }

  void start() => _check(_b.prism_start(_core), 'prism_start');
  void stop() => _check(_b.prism_stop(_core), 'prism_stop');

  void reportAppSwitch(int tMs) =>
      _check(_b.prism_report_app_switch(_core, tMs), 'prism_report_app_switch');

  void reportIdle(int tMs, int idleMs) =>
      _check(_b.prism_report_idle(_core, tMs, idleMs), 'prism_report_idle');

  void reportTaskDeadlines(List<TaskDeadline> tasks) {
    final native = calloc<raw.prism_task_deadline>(tasks.isEmpty ? 1 : tasks.length);
    try {
      for (var i = 0; i < tasks.length; i++) {
        native[i].due_ms = tasks[i].dueMs;
        native[i].priority = tasks[i].priority.index;
      }
      _check(_b.prism_report_task_deadlines(_core, tasks.isEmpty ? ffi.nullptr : native,
              tasks.length),
          'prism_report_task_deadlines');
    } finally {
      calloc.free(native);
    }
  }

  /// The most recently emitted PSV, or null before [start] (INVALID_STATE in the ABI).
  Psv? get psv {
    final out = calloc<raw.prism_psv>();
    try {
      final result = _b.prism_get_psv(_core, out);
      if (result == raw.prism_result.PRISM_ERROR_INVALID_STATE) {
        return null;
      }
      _check(result, 'prism_get_psv');
      final hintChars = <int>[];
      for (var i = 0; i < 24; i++) {
        final c = out.ref.mode_hint[i];
        if (c == 0) break;
        hintChars.add(c);
      }
      return Psv(
        arousal: out.ref.arousal,
        arousalConfidence: out.ref.arousal_confidence,
        valence: out.ref.valence,
        valenceConfidence: out.ref.valence_confidence,
        cognitiveLoad: out.ref.cognitive_load,
        cognitiveLoadConfidence: out.ref.cognitive_load_confidence,
        readiness: out.ref.readiness,
        readinessConfidence: out.ref.readiness_confidence,
        updateTimestampMs: out.ref.update_timestamp_ms,
        sequence: out.ref.sequence,
        modeHint: hintChars.isEmpty ? null : String.fromCharCodes(hintChars),
      );
    } finally {
      calloc.free(out);
    }
  }

  int get sampleRate => _b.prism_sample_rate(_core);

  void deviceStart() => _check(_b.prism_device_start(_core), 'prism_device_start');
  void deviceStop() => _check(_b.prism_device_stop(_core), 'prism_device_stop');

  /// Pull-model render (host tests / custom audio hosts). NOT for the UI isolate while
  /// the built-in device runs — same single-audio-thread contract as the C header.
  Float32List render(int frameCount) {
    final buffer = calloc<ffi.Float>(frameCount);
    try {
      _check(_b.prism_render(_core, buffer, frameCount), 'prism_render');
      return Float32List.fromList(buffer.asTypedList(frameCount));
    } finally {
      calloc.free(buffer);
    }
  }

  /// Stops everything and frees the native handle. The instance is unusable after.
  void dispose() {
    if (_core != ffi.nullptr) {
      _b.prism_destroy(_core);
      _core = ffi.nullptr;
    }
  }
}
