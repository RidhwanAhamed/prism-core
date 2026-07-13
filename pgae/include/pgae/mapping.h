#pragma once

// PSV → audio parameter mapping (verbatim port of the probe's audio/mapping.ts; PGAE §5,
// §11). This is the ONLY place a state vector becomes audio intent — the audio side of the
// firewall. Pure and DSP-free so it is unit-testable; the engine applies these targets
// with smoothing/slew and equal-power crossfades. Coefficients are validated dogfooding
// findings — parity with the probe is the bar, do not retune in this slice.

#include "psv/psv.h"

#include <array>
#include <cstddef>

namespace prism::pgae {

// Stem roles in activation order. Optional layers activate by increasing salience as
// density rises: pulse (subtle rhythm), then air (texture), then lead (melodic foreground).
// Under high load density falls and they drop out, leaving bed + sub — the most
// non-distracting core.
enum class StemRole : size_t { Bed = 0, Sub = 1, Pulse = 2, Lead = 3, Air = 4 };
inline constexpr size_t kStemRoleCount = 5;
inline constexpr std::array<const char*, kStemRoleCount> kStemRoleNames = {"bed", "sub", "pulse",
                                                                           "lead", "air"};

struct AudioParams {
  double cutoff_hz = 0.0;                     // master low-pass cutoff (brightness)
  std::array<double, kStemRoleCount> gains{}; // per-stem target loudness, [0,1]
  std::array<bool, kStemRoleCount> active{};  // density gate; bed + sub always on
  double density = 0.0;                       // raw density scalar (logging/tests)
};

// Map [0,1] brightness → Hz on a log scale (perceptually even). 300 Hz – 12 kHz.
double brightness_to_hz(double brightness);

AudioParams psv_to_audio_params(const psv::StateVector& v);

inline double gain_of(const AudioParams& p, StemRole role) {
  return p.gains[static_cast<size_t>(role)];
}
inline bool active_of(const AudioParams& p, StemRole role) {
  return p.active[static_cast<size_t>(role)];
}

} // namespace prism::pgae
