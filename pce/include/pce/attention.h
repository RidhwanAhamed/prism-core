#pragma once

// Behavioral / attention adapter (verbatim port of the probe's adapters/attention.ts).
// Focus/engagement drivers from active-window switch frequency and idle gaps over a
// ROLLING IN-MEMORY WINDOW. Events are bare timestamps (and idle durations); no app
// identity ever reaches this class (hashing and discarding happen in the platform shell's
// capture layer). Nothing here persists to disk — CLAUDE.md privacy invariant 2.

#include <cstdint>
#include <vector>

namespace prism::pce {

// The two distinct behavioral drivers the PSV spec names (§4.1): "behavioural intensity"
// (activity) and "app-switching" (scatter). Fusion reads both because they push different
// PSV dimensions.
struct AttentionDrivers {
  double activity = 0.5;   // 1 = fully active, 0 = away (i.e. 1 − idle fraction)
  double scatter = 0.0;    // 0 = settled, 1 = constant context-switching
  double confidence = 0.0; // 0 with no idle samples (no usable behavioral input)
};

// WHY these defaults (copied verbatim from the probe — dogfooding findings, not magic):
// - window_ms 120s: longer than the ~30s PSV cadence so the signal is stable, not twitchy.
// - sample_interval_ms 5s: the poll heartbeat; 24 samples = a full 2-min window.
// - idle_threshold_ms 15s: "no input for 15s+" reads as away for that sample.
// - switch_scale_per_min 6: 6 switches/min → scatter ~0.63, 12/min → ~0.86.
// - base_confidence 0.7: behavior is a noisier proxy than a hard deadline date, so even a
//   full window tops out below 1.0.
struct AttentionOptions {
  int64_t window_ms = 120'000;
  int64_t sample_interval_ms = 5'000;
  int64_t idle_threshold_ms = 15'000;
  double switch_scale_per_min = 6.0;
  double base_confidence = 0.7;
};

class AttentionMonitor {
public:
  explicit AttentionMonitor(AttentionOptions options = {}) : opts_(options) {}

  void record_switch(int64_t t_ms) { switches_.push_back(t_ms); }
  void record_idle(int64_t t_ms, int64_t idle_ms) { idles_.push_back({t_ms, idle_ms}); }

  // The behavioral drivers (activity, scatter) plus confidence — what fusion consumes.
  // Evicts events older than the window, exactly like the probe (keep t >= now − window).
  AttentionDrivers read_drivers(int64_t now_ms);

private:
  struct IdleSample {
    int64_t t;
    int64_t idle_ms;
  };

  AttentionOptions opts_;
  std::vector<int64_t> switches_;
  std::vector<IdleSample> idles_;
};

} // namespace prism::pce
