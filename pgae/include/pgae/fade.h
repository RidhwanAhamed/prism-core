#pragma once

// Pure scheduling/shape helpers for the render path, public so the acceptance tests can
// pin their properties directly (equal-power, sample accuracy).

#include <cmath>
#include <cstdint>

namespace prism::pgae {

// Equal-power fade shape (PGAE §6.2): sin for in, cos for out; in²(t) + out²(t) == 1 for
// all t in [0,1], so a complementary pair holds constant acoustic power.
inline double equal_power_fade(double t, bool fade_in) {
  const double clamped = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
  const double phase = clamped * 1.5707963267948966; // π/2
  return fade_in ? std::sin(phase) : std::cos(phase);
}

// Next loop boundary strictly after `sample_clock`, for a stem of `loop_frames` that
// started at sample 0 (all stems start together; PGAE §6.2 schedules density swaps here).
// Mirrors the probe: floor(elapsed / loop) + 1 whole loops from the start.
inline uint64_t next_loop_boundary(uint64_t sample_clock, uint64_t loop_frames) {
  return (sample_clock / loop_frames + 1) * loop_frames;
}

} // namespace prism::pgae
