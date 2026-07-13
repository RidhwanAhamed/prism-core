#include "pgae/mapping.h"

#include <algorithm>
#include <cmath>

namespace prism::pgae {

namespace {

inline double clamp01(double x) {
  return std::max(0.0, std::min(1.0, x));
}

constexpr double kCutoffMinHz = 300.0;
constexpr double kCutoffMaxHz = 12'000.0;

} // namespace

double brightness_to_hz(double brightness) {
  return kCutoffMinHz * std::pow(kCutoffMaxHz / kCutoffMinHz, clamp01(brightness));
}

namespace {
AudioParams map_from_effectives(double arousal_eff, double load_eff, double readiness_eff);
} // namespace

AudioParams psv_to_audio_params(const psv::StateVector& v) {
  return map_from_effectives(v.arousal.effective(), v.cognitive_load.effective(),
                             v.readiness.effective());
}

AudioParams psv_to_audio_params(const psv::RtStateVector& v) {
  return map_from_effectives(v.effective(psv::kArousal), v.effective(psv::kCognitiveLoad),
                             v.effective(psv::kReadiness));
}

namespace {

AudioParams map_from_effectives(double arousal_eff, double load_eff, double readiness_eff) {
  // Confidence-weighted (PGAE §3 / PSV §8.1), centered on the neutral baseline. valence is
  // inert in v1 (PGAE §11).
  const double a = arousal_eff - 0.5;
  const double l = load_eff - 0.5;
  const double r = readiness_eff - 0.5;

  // Brightness: arousal opens the filter, load closes it (recede). Neutral (a=l=0) → 0.55.
  const double brightness = clamp01(0.55 + 0.9 * a - 0.8 * l);

  // Density: arousal adds layers, load strips them (focus protection, PGAE §5). Neutral → 0.5.
  const double density = clamp01(0.5 + 0.9 * a - 1.0 * l);

  AudioParams p;
  p.cutoff_hz = brightness_to_hz(brightness);
  p.density = density;

  // Per-stem target loudness (PGAE §5 directions; coefficients verbatim from the probe):
  p.gains[static_cast<size_t>(StemRole::Bed)] =
      clamp01(0.6 + 0.6 * l); // steady bed rises under load — something calm to hold onto
  p.gains[static_cast<size_t>(StemRole::Sub)] =
      clamp01(0.5 - 0.6 * r); // low grounding rises as readiness falls (support the fatigued)
  p.gains[static_cast<size_t>(StemRole::Pulse)] =
      clamp01(0.5 + 0.7 * a - 0.6 * l); // rhythmic energy up with arousal, stripped under load
  p.gains[static_cast<size_t>(StemRole::Lead)] =
      clamp01(0.5 + 0.4 * a - 0.9 * l); // melodic foreground recedes hard under load
  p.gains[static_cast<size_t>(StemRole::Air)] =
      clamp01(0.5 + 0.5 * a - 0.6 * l); // high texture up with arousal, thinned under load

  // Density gate thresholds (verbatim): bed + sub always on.
  p.active[static_cast<size_t>(StemRole::Bed)] = true;
  p.active[static_cast<size_t>(StemRole::Sub)] = true;
  p.active[static_cast<size_t>(StemRole::Pulse)] = density >= 0.35;
  p.active[static_cast<size_t>(StemRole::Air)] = density >= 0.55;
  p.active[static_cast<size_t>(StemRole::Lead)] = density >= 0.72;

  return p;
}

} // namespace

} // namespace prism::pgae
