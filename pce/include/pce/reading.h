#pragma once

// Shared shape for PCE input adapters (port of the probe's adapters/shared.ts). An adapter
// reduces some local signal to a normalized value plus a confidence; it is NOT a PSV and
// emits no audio parameters — readings are fused into PSV dimensions by fusion.h.

#include <algorithm>

namespace prism::pce {

struct AdapterReading {
  double value = 0.5;      // derived signal in [0,1]
  double confidence = 0.0; // trust in the reading, [0,1]; 0 = no usable input
};

inline double clamp01(double x) {
  return std::max(0.0, std::min(1.0, x));
}

} // namespace prism::pce
