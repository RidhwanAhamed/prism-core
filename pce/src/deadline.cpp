#include "pce/deadline.h"

#include <algorithm>
#include <cmath>

namespace prism::pce {

namespace {
double priority_weight(Priority p, const DeadlineOptions& o) {
  switch (p) {
  case Priority::Low:
    return o.weight_low;
  case Priority::High:
    return o.weight_high;
  case Priority::Medium:
    break;
  }
  return o.weight_medium;
}
} // namespace

AdapterReading deadline_proximity(const std::vector<TaskDeadline>& tasks, int64_t now_ms,
                                  const DeadlineOptions& options) {
  // The probe additionally skips tasks with unparseable due strings; ingress here is
  // already-parsed epoch ms, so every task is valid by construction.
  double max_pressure = 0.0;
  for (const TaskDeadline& task : tasks) {
    const double weight = priority_weight(task.priority, options);
    const double delta = std::max(0.0, static_cast<double>(task.due_ms - now_ms));
    const double pressure = weight * std::exp(-delta / options.tau_ms);
    if (pressure > max_pressure) {
      max_pressure = pressure;
    }
  }

  if (tasks.empty()) {
    return {0.5, 0.0}; // no usable deadline: neutral prior at zero confidence
  }
  return {clamp01(max_pressure), clamp01(options.base_confidence)};
}

} // namespace prism::pce
