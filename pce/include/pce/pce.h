#pragma once

// The Prism Context Engine (verbatim port of the probe's engine.ts, plus the event
// ingress the platform shells call). Gathers inputs, fuses them into a PSV, and emits at
// the spec cadence (~30s for aqademiq, PSV §7.1) plus event-driven on significant change.
// It produces ONLY a PSV — the firewall.
//
// Threading: this class is single-threaded by design; the host owns the timers and (from
// Task 4) the inference thread. All state lives in memory; raw events are never persisted.

#include "pce/attention.h"
#include "pce/deadline.h"
#include "pce/fusion.h"
#include "psv/psv.h"

#include <cstdint>
#include <limits>
#include <optional>
#include <vector>

namespace prism::pce {

struct PceOptions {
  int64_t cadence_ms = 30'000;    // nominal emit cadence (PSV §7.1: aqademiq ~30s)
  double significant_delta = 0.1; // min |Δvalue| in any dimension forcing an early emit
  psv::Vertical vertical = psv::Vertical::Aqademiq;
  int tz_offset_min = 0; // host-supplied (UTC − local, minutes); circadian is local-hours
  AttentionOptions attention{};
  DeadlineOptions deadline{};
};

class Pce {
public:
  explicit Pce(PceOptions options = {}) : opts_(options), attention_(options.attention) {}

  // --- Event ingress (platform shells capture raw events and forward them; no inference
  // --- logic lives outside this module). App identity never crosses: a switch is a bare
  // --- timestamp, idle is a duration, tasks are due instants + priorities.
  void report_app_switch(int64_t t_ms) { attention_.record_switch(t_ms); }
  void report_idle(int64_t t_ms, int64_t idle_ms) { attention_.record_idle(t_ms, idle_ms); }
  void report_task_deadlines(std::vector<TaskDeadline> tasks) { tasks_ = std::move(tasks); }

  // --- Lifecycle. The host drives the clock: start() emits the cold-start neutral vector
  // --- (PSV §6); evaluate() is one tick — it emits when the cadence has elapsed OR a
  // --- dimension moved by significant_delta, and returns the emission (if any).
  psv::StateVector start(int64_t now_ms);
  std::optional<psv::StateVector> evaluate(int64_t now_ms);

  // Claim the next sequence number for a vector this PCE did not author — the host mood
  // override publishes out of band, and the C ABI promises a strictly monotonic sequence
  // that is safe to dedup by. Reserving here keeps the PCE the single owner of the
  // counter, so a later emission can never hand back a number the host already used.
  // Does not count as an emission: the cadence and last_psv_ are untouched.
  int64_t reserve_sequence() { return ++seq_; }

private:
  FusionInputs read_inputs(int64_t now_ms);
  bool significantly_changed(const psv::StateVector& candidate) const;
  void note_emit(const psv::StateVector& v, int64_t now_ms);

  PceOptions opts_;
  AttentionMonitor attention_;
  std::vector<TaskDeadline> tasks_;
  int64_t seq_ = 0;
  // Matches the probe's Number.NEGATIVE_INFINITY sentinel: first evaluate always passes
  // the cadence check. Epoch-ms values are exact in double (< 2^53).
  double last_emit_ms_ = -std::numeric_limits<double>::infinity();
  std::optional<psv::StateVector> last_psv_;
};

} // namespace prism::pce
