#pragma once

// Circadian phase adapter (verbatim port of the probe's adapters/circadian.ts). Time of
// day is always available, so this provides a baseline alertness/readiness prior. It is a
// population-level prior, not personalized → modest fixed confidence.
//
// Works in LOCAL hours: callers supply the timezone offset (minutes, JS
// Date#getTimezoneOffset convention: UTC − local). Hours are quantized to whole minutes,
// reproducing the probe's getHours() + getMinutes()/60 arithmetic op-for-op — golden-trace
// parity depends on it.

#include <cstdint>

namespace prism::pce {

struct CircadianReading {
  double alertness = 0.5; // [0,1]; 0 = deep-night trough, 1 = late-afternoon peak
  double confidence = 0.0;
};

// Pure curve: local hour (0–24, fractional) → alertness. 24h cosine with trough ~04:00 and
// peak ~16:00, minus a small post-lunch dip around 14:00 (constants verbatim).
double circadian_alertness(double hour);

CircadianReading circadian_from_epoch(int64_t now_ms, int tz_offset_min);

} // namespace prism::pce
