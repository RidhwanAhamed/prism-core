#pragma once

// Deadline-proximity adapter (verbatim port of the probe's adapters/deadline-proximity.ts).
// Reduces the user's task list to a single "deadline pressure" reading. Task titles never
// enter the core: ingress carries due instants and priorities only.

#include "pce/reading.h"

#include <cstdint>
#include <vector>

namespace prism::pce {

enum class Priority { Low, Medium, High };

struct TaskDeadline {
  int64_t due_ms = 0; // epoch ms (platform shells parse whatever transport they use)
  Priority priority = Priority::Medium;
};

// WHY these defaults (copied verbatim from the probe):
// - tau = 48h: pressure ramps meaningfully inside ~2 days, fades for a week+ out.
//   exp(-Δt/48h): 12h→0.78, 24h→0.61, 2d→0.37, 1wk→0.04.
// - priority weights cap how pressing a task can get; missing/unknown priority → medium.
// - base confidence 0.9: a due date is a fact, not a decaying sensor sample — availability,
//   not freshness, drives confidence; keep a little headroom.
struct DeadlineOptions {
  double tau_ms = 48.0 * 60.0 * 60.0 * 1000.0;
  double weight_low = 0.4;
  double weight_medium = 0.7;
  double weight_high = 1.0;
  double base_confidence = 0.9;
};

// pressure = priorityWeight × exp(-max(0, Δt)/tau), MAX across tasks — the single most
// pressing deadline drives the signal. Overdue clamps at Δt = 0 (maximally proximate).
// Empty task list → neutral prior at zero confidence (inert in fusion).
AdapterReading deadline_proximity(const std::vector<TaskDeadline>& tasks, int64_t now_ms,
                                  const DeadlineOptions& options = {});

} // namespace prism::pce
