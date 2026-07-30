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

/// A named venue mood: a point in PSV space plus the hint naming it.
///
/// These are DERIVED from the engine's own mapping (pgae/src/mapping.cpp), not dialled by
/// ear, because that mapping is what decides whether two moods differ at all. Density
/// gates which stems open (pulse ≥ 0.35, air ≥ 0.55, lead ≥ 0.72), brightness drives the
/// master low-pass, and the sub gain moves on readiness independently of arousal. Each
/// preset lands on a deliberate side of those gates, so the moods differ in how many
/// layers play — not merely in volume.
///
/// [valence] is carried for completeness but is inert in v1 (PGAE §11).
///
/// The [id] values match the mood ids the Prism Venues app and backend already use, so a
/// mood coming off the API maps straight through [byId].
class VenueMood {
  const VenueMood._(this.id, this.modeHint, this.arousal, this.valence, this.cognitiveLoad,
      this.readiness);

  final String id;
  final String modeHint;
  final double arousal;
  final double valence;
  final double cognitiveLoad;
  final double readiness;

  /// Sparse and dark — bed and sub only, the "duo texture" the mood spec asks for, with
  /// low readiness lifting the sub into a drone.
  static const windDown =
      VenueMood._('wind-down', 'venue_wind_down', 0.12, 0.40, 0.50, 0.20);

  /// Equally sparse but the opposite character: low load lets the air layer through and
  /// keeps the filter up, high readiness keeps the bottom light.
  static const morningCalm =
      VenueMood._('morning-calm', 'venue_morning_calm', 0.32, 0.70, 0.22, 0.62);

  /// Just past the pulse gate — the groove is present without being forward.
  static const daytimeFlow =
      VenueMood._('daytime-flow', 'venue_daytime_flow', 0.52, 0.65, 0.42, 0.50);

  /// Just past the lead gate, with load held high enough to keep the filter down: all
  /// five layers, rich rather than bright, and a deep bottom from low readiness.
  static const eveningWarmth =
      VenueMood._('evening-warmth', 'venue_evening_warmth', 0.62, 0.60, 0.38, 0.35);

  /// A step up, not a peak: brighter and more forward than daytime, lead still shut.
  static const afternoonLift =
      VenueMood._('afternoon-lift', 'venue_afternoon_lift', 0.70, 0.72, 0.40, 0.55);

  /// Everything open, filter wide, pulse loudest of the six.
  static const peak = VenueMood._('peak', 'venue_peak', 0.92, 0.70, 0.35, 0.58);

  /// In the order a day runs, which is also sparsest to densest.
  static const all = <VenueMood>[
    morningCalm,
    daytimeFlow,
    afternoonLift,
    eveningWarmth,
    peak,
    windDown,
  ];

  /// Looks up a mood by the id the app and backend use. Null for an unknown id — callers
  /// decide whether to fall back or surface it, rather than getting a silent default.
  static VenueMood? byId(String id) {
    for (final m in all) {
      if (m.id == id) return m;
    }
    return null;
  }

  @override
  String toString() => 'VenueMood($id)';
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
    // Desktop hosts. Windows is where the venues app is developed before it reaches a
    // Mac, and Linux is the Prism Venues embedded target; both load a plain shared
    // object next to the executable.
    if (Platform.isWindows) return ffi.DynamicLibrary.open('prism_core.dll');
    if (Platform.isLinux) return ffi.DynamicLibrary.open('libprism_core.so');
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

  /// Pins the PSV so the engine holds one mood until [clearMoodOverride].
  ///
  /// Takes effect immediately — the vector is published before this returns, so a tap is
  /// heard without waiting out the inference cadence. The engine ramps into it; nothing
  /// steps.
  ///
  /// [confidence] defaults to 1.0 and should stay there for a manual choice. Consumers
  /// blend toward neutral as confidence falls (PSV spec §8.1), so a mood pinned at low
  /// confidence is pulled back to the middle and barely changes the sound.
  void setMoodOverride(VenueMood mood, {double confidence = 1.0}) {
    final o = calloc<raw.prism_mood_override>();
    final hint = mood.modeHint.toNativeUtf8();
    try {
      o.ref.mode_hint = hint.cast();
      o.ref.arousal = mood.arousal;
      o.ref.valence = mood.valence;
      o.ref.cognitive_load = mood.cognitiveLoad;
      o.ref.readiness = mood.readiness;
      o.ref.confidence = confidence;
      _check(_b.prism_set_mood_override(_core, o), 'prism_set_mood_override');
    } finally {
      calloc.free(hint);
      calloc.free(o);
    }
  }

  /// Hands control back to the context engine, which resumes at its next tick. The
  /// pinned mood stays audible until then rather than dropping to neutral. Idempotent.
  void clearMoodOverride() =>
      _check(_b.prism_clear_mood_override(_core), 'prism_clear_mood_override');

  void deviceStart() => _check(_b.prism_device_start(_core), 'prism_device_start');

  /// Stops the built-in device. This is what Takeover calls: the engine goes silent so
  /// venue staff can drive the speakers from their own source. Idempotent.
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
