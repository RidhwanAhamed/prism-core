#include "pce/attention.h"

#include "pce/reading.h"

#include <algorithm>
#include <cmath>

namespace prism::pce {

AttentionDrivers AttentionMonitor::read_drivers(int64_t now_ms) {
  const int64_t cutoff = now_ms - opts_.window_ms;
  switches_.erase(std::remove_if(switches_.begin(), switches_.end(),
                                 [cutoff](int64_t t) { return t < cutoff; }),
                  switches_.end());
  idles_.erase(std::remove_if(idles_.begin(), idles_.end(),
                              [cutoff](const IdleSample& s) { return s.t < cutoff; }),
               idles_.end());

  // The idle heartbeat (one sample per poll) is what establishes coverage/confidence. With
  // no samples we have no usable behavioral input → neutral prior, zero confidence.
  if (idles_.empty()) {
    return {0.5, 0.0, 0.0};
  }

  const double switches_per_min =
      static_cast<double>(switches_.size()) / (static_cast<double>(opts_.window_ms) / 60'000.0);
  const double scatter = 1.0 - std::exp(-switches_per_min / opts_.switch_scale_per_min);

  int64_t away = 0;
  for (const IdleSample& s : idles_) {
    if (s.idle_ms > opts_.idle_threshold_ms) {
      ++away;
    }
  }
  const double activity = 1.0 - static_cast<double>(away) / static_cast<double>(idles_.size());

  const double expected_samples =
      static_cast<double>(opts_.window_ms) / static_cast<double>(opts_.sample_interval_ms);
  const double coverage = clamp01(static_cast<double>(idles_.size()) / expected_samples);

  return {activity, scatter, clamp01(coverage * opts_.base_confidence)};
}

} // namespace prism::pce
