/*
 * prism_core.h — the Prism Engine core C ABI.
 *
 * THE ONLY PUBLIC HEADER. Everything else in this repository is internal. This surface
 * is semver'd (PRISM_ABI_VERSION_*): breaking any declaration here is a major version.
 * It is the binding reference for dart:ffi (ffigen), embedded hosts, and OEM SDKs.
 *
 * Architecture behind the handle: raw events go in (prism_report_*), the Prism Context
 * Engine fuses them into a Prism State Vector on an internal inference thread, the PSV
 * crosses to the audio path through a wait-free atomic exchange, and the Prism
 * Generative Audio Engine renders adaptive audio. The PSV is the only thing that crosses
 * between inference and audio — the structural firewall.
 *
 * Threading contract:
 *   - prism_create / prism_destroy / prism_load_scene / prism_start / prism_stop /
 *     prism_device_*: host "control" thread. Not thread-safe against each other.
 *   - prism_report_*: any thread (internally synchronized). Never call from the render
 *     callback.
 *   - prism_get_psv: any thread (internally synchronized). UI-friendly.
 *   - prism_render: exactly ONE audio thread (single-reader contract). Wait-free,
 *     allocation-free, lock-free — safe inside a real-time audio callback. Unnecessary
 *     when the built-in device is running (it renders internally).
 *   - QUIESCENCE BEFORE DESTROY: no library can stop a thread it does not own. The host
 *     MUST guarantee that no thread is inside ANY prism_* call — above all a pull-model
 *     audio thread inside prism_render — when prism_destroy runs, or behavior is
 *     undefined (use-after-free). Stop your audio callback first, then destroy. The
 *     built-in device and the internal inference thread ARE owned by the library and are
 *     shut down by prism_destroy automatically.
 *
 * Fully local by design: nothing in this library performs network I/O, and raw input
 * events are processed in memory only — never persisted.
 */

#ifndef PRISM_CORE_H
#define PRISM_CORE_H

#include <stddef.h>
#include <stdint.h>

/* Export annotation: when the core is built as a shared library, ONLY prism_* symbols
 * are visible (internal targets compile with hidden visibility). Windows dllexport/
 * dllimport plumbing lands with the first Windows target. */
#ifndef PRISM_API
#if defined(_WIN32)
#define PRISM_API
#else
#define PRISM_API __attribute__((visibility("default")))
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define PRISM_ABI_VERSION_MAJOR 0
#define PRISM_ABI_VERSION_MINOR 1
#define PRISM_ABI_VERSION_PATCH 0

/* "MAJOR.MINOR.PATCH" of the linked library. Compare against the macros above. */
PRISM_API const char* prism_version(void);

typedef enum prism_result {
  PRISM_OK = 0,
  PRISM_ERROR_INVALID_ARGUMENT = 1, /* null/malformed parameter */
  PRISM_ERROR_INVALID_STATE = 2,    /* call violates the lifecycle (see per-function docs) */
  PRISM_ERROR_IO = 3,               /* scene manifest or stem file unreadable/invalid */
  PRISM_ERROR_DEVICE = 4,           /* audio device could not be opened or started */
  PRISM_ERROR_OUT_OF_MEMORY = 5
} prism_result;

/* Static, human-readable description; never NULL. */
PRISM_API const char* prism_result_description(prism_result result);

/* Opaque engine handle. One handle = one PCE + one PGAE + their PSV exchange. */
typedef struct prism_core prism_core;

typedef enum prism_vertical {
  PRISM_VERTICAL_AQADEMIQ = 0,
  PRISM_VERTICAL_VENUES = 1,
  PRISM_VERTICAL_AUTOMOTIVE = 2
} prism_vertical;

typedef struct prism_config {
  /* Timezone offset in minutes, UTC minus local (JS Date#getTimezoneOffset convention).
   * The circadian prior works in local hours; hosts refresh it by re-creating the core
   * (sessions should not span timezone changes). */
  int32_t tz_offset_min;
  /* Adapter profile / PSV `vertical` field. v1 implements the aqademiq profile. */
  int32_t vertical;
  /* Nominal PSV emit cadence in ms; 0 = spec default (30000 for aqademiq, PSV §7.1). */
  int64_t cadence_ms;
  /* Inference wake interval in ms; 0 = default (5000). Mainly for tests. */
  int64_t check_interval_ms;
  /* Min |value change| in any PSV dimension forcing an early emit; 0 = default (0.1). */
  double significant_delta;
} prism_config;

/* All defaults (aqademiq profile, UTC, spec cadence). */
PRISM_API prism_config prism_config_default(void);

/* Create an engine. `config` may be NULL for all defaults. On success writes the new
 * handle to *out_core. The handle must be released with prism_destroy. */
PRISM_API prism_result prism_create(const prism_config* config, prism_core** out_core);

/* Stops everything still running (inference thread, built-in device) and frees the
 * handle. NULL is a no-op. */
PRISM_API void prism_destroy(prism_core* core);

/* Load a scene manifest (scenes.json; stem paths resolve relative to it). Decodes and
 * preloads every stem of the default scene — file I/O happens HERE, never at render
 * time. Call once, before prism_start / any rendering. INVALID_STATE after start. */
PRISM_API prism_result prism_load_scene(prism_core* core, const char* scenes_json_path);

/* Start the engine: emits the neutral PSV (spec §6) and launches the internal inference
 * thread (evaluates on the cadence + significant-change rule). Requires a loaded scene.
 * INVALID_STATE if already started. Restart after prism_stop is allowed; the restart's
 * neutral vector continues the sequence (see prism_psv.sequence). */
PRISM_API prism_result prism_start(prism_core* core);

/* Stop the inference thread (and the built-in device if running). Idempotent. */
PRISM_API prism_result prism_stop(prism_core* core);

/* --- Event ingress -------------------------------------------------------------------
 * Platform shells capture raw events and forward them; no inference logic lives on the
 * host side. Timestamps are epoch milliseconds from the host clock. App identity never
 * crosses this boundary: a switch is a bare timestamp, idle is a duration, tasks are
 * due instants plus priorities. Thread-safe; never call from the render callback. */

PRISM_API prism_result prism_report_app_switch(prism_core* core, int64_t t_ms);

PRISM_API prism_result prism_report_idle(prism_core* core, int64_t t_ms, int64_t idle_ms);

typedef struct prism_task_deadline {
  int64_t due_ms;   /* epoch ms */
  int32_t priority; /* 0 = low, 1 = medium, 2 = high; out of range reads as medium */
} prism_task_deadline;

/* Replace the full task-deadline snapshot (matches how hosts observe task lists).
 * `tasks` may be NULL when `count` is 0 (an empty list is a valid state). Task lists are
 * human-scale: count > 4096 is rejected as INVALID_ARGUMENT (this also shields the
 * boundary from negative lengths cast to size_t). No C++ exception ever crosses this
 * ABI; internal failures surface as error codes. */
PRISM_API prism_result prism_report_task_deadlines(prism_core* core,
                                                   const prism_task_deadline* tasks, size_t count);

/* --- PSV read-out (for UI) ----------------------------------------------------------- */

typedef struct prism_psv {
  /* Spec §4.1 dimensions, each value/confidence in [0,1]. */
  double arousal, arousal_confidence;
  double valence, valence_confidence;
  double cognitive_load, cognitive_load_confidence;
  double readiness, readiness_confidence;
  int64_t update_timestamp_ms;
  /* Strictly monotonic across the handle's lifetime: the first start's cold-start vector
   * is 0; a restart's neutral vector CONTINUES the sequence (safe to dedup by it). */
  int64_t sequence;
  char mode_hint[24]; /* NUL-terminated; empty string = null hint (spec §9) */
} prism_psv;

/* Copy the most recently emitted PSV into *out. INVALID_STATE before prism_start (the
 * cold-start vector exists from start onward). Thread-safe. */
PRISM_API prism_result prism_get_psv(prism_core* core, prism_psv* out);

/* --- Audio out ------------------------------------------------------------------------
 * Two ways to get sound, mutually exclusive per handle:
 *   pull model — the host owns the audio device and calls prism_render from its audio
 *                callback (this is what dart:ffi / OEM hosts do);
 *   built-in device — the core opens a default playback device via its own backend
 *                     (desktop harness, quick starts). */

/* Sample rate of the loaded scene's stems (0 before a scene is loaded). Mono float32. */
PRISM_API uint32_t prism_sample_rate(const prism_core* core);

/* Render `frame_count` mono float32 frames into out_frames. Real-time safe: wait-free,
 * no allocation, no locks, no I/O, no logging. Single audio thread only. Before a scene
 * is loaded, fills silence and returns INVALID_STATE. */
PRISM_API prism_result prism_render(prism_core* core, float* out_frames, uint32_t frame_count);

/* Open and start the built-in playback device (renders internally; do not also call
 * prism_render). Requires a loaded scene. INVALID_STATE if already running. */
PRISM_API prism_result prism_device_start(prism_core* core);

/* Stop and close the built-in device. Idempotent. */
PRISM_API prism_result prism_device_stop(prism_core* core);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* PRISM_CORE_H */
