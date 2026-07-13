#include "pce/circadian.h"

#include "pce/reading.h"

#include <cmath>

namespace prism::pce {

namespace {
// Always available, but a generic prior rather than a measured signal → mid confidence.
constexpr double kCircadianConfidence = 0.5;
// JS Math.PI, so the cosine argument matches the probe exactly.
constexpr double kPi = 3.141592653589793;
} // namespace

double circadian_alertness(double hour) {
  const double h = std::fmod(std::fmod(hour, 24.0) + 24.0, 24.0);
  const double daily = 0.5 + 0.4 * std::cos((2.0 * kPi * (h - 16.0)) / 24.0);
  const double post_lunch_dip = 0.1 * std::exp(-((h - 14.0) * (h - 14.0)) / (2.0 * 1.5 * 1.5));
  return clamp01(daily - post_lunch_dip);
}

CircadianReading circadian_from_epoch(int64_t now_ms, int tz_offset_min) {
  // Same arithmetic as the probe's replay path (circadianFromEpoch): minute-quantized
  // local time, hour = wholeHours + minutes/60 — NOT minutesOfDay/60, which differs by an
  // ulp for some instants.
  const int64_t local_minutes = static_cast<int64_t>(std::floor(
      static_cast<double>(now_ms - static_cast<int64_t>(tz_offset_min) * 60'000) / 60'000.0));
  const int64_t minutes_of_day = ((local_minutes % 1440) + 1440) % 1440;
  const double hour =
      static_cast<double>(minutes_of_day / 60) + static_cast<double>(minutes_of_day % 60) / 60.0;
  return {circadian_alertness(hour), kCircadianConfidence};
}

} // namespace prism::pce
