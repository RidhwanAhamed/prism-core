#include "pce/fusion.h"

#include <initializer_list>

namespace prism::pce {

namespace {

struct Contribution {
  double target;     // this signal's best estimate of the dimension's value, [0,1]
  double confidence; // trust in that estimate right now, [0,1]
  double importance; // how much this signal should matter (relative weight)
};

// Confidence-weighted blend of independent estimates (verbatim from the probe).
// value = Σ(conf·imp·target)/Σ(conf·imp); confidence = Σ(conf·imp)/Σ(imp) — how much of
// the dimension's expected input is actually present and trusted ("confidence from input
// availability and freshness"). With no trusted input: neutral prior at zero confidence.
// Accumulation order matches the probe's loop for floating-point faithfulness.
psv::Dimension blend(std::initializer_list<Contribution> contributions) {
  double weight = 0.0;
  double weighted_target = 0.0;
  double importance = 0.0;
  double confidence_weight = 0.0;
  for (const Contribution& c : contributions) {
    const double w = c.confidence * c.importance;
    weight += w;
    weighted_target += w * c.target;
    importance += c.importance;
    confidence_weight += c.confidence * c.importance;
  }
  if (weight == 0.0 || importance == 0.0) {
    return {0.5, 0.0};
  }
  return {clamp01(weighted_target / weight), clamp01(confidence_weight / importance)};
}

} // namespace

psv::StateVector fuse_to_psv(const FusionInputs& inputs, const FusionMeta& meta) {
  const AdapterReading& d = inputs.deadline;
  const AttentionDrivers& a = inputs.attention;
  const CircadianReading& c = inputs.circadian;

  // Behavioral arousal estimate: away → low; active+settled → calm-alert; active+scattered
  // → keyed-up. (Idle suppresses arousal regardless; scatter only adds energy when present.)
  const double behavioral_arousal = clamp01(a.activity * (0.55 + 0.45 * a.scatter));

  // arousal — activation. Deadline pressure keys you up; circadian sets the daily
  // baseline; behavior adds moment-to-moment activation (PSV §4.1 drivers).
  const psv::Dimension arousal = blend({
      {d.value, d.confidence, 1.0},
      {c.alertness, c.confidence, 0.7},
      {behavioral_arousal, a.confidence, 0.7},
  });

  // cognitive_load — deadline pressure and fragmentation both raise load. Intended
  // property: deep focus under pressure (high D, low scatter) lands at MODERATE load —
  // flow, not overwhelm — whereas scattered-under-pressure lands HIGH, so the audio
  // recedes hardest exactly then (the core focus-protection case, PGAE §5).
  const psv::Dimension cognitive_load = blend({
      {d.value, d.confidence, 1.0},
      {a.scatter, a.confidence, 0.9},
  });

  // readiness — circadian capacity, eroded by sustained deadline pressure. Low readiness
  // with high load is the "loaded and fatigued → recede AND ground" case (PGAE §5).
  const psv::Dimension readiness = blend({
      {c.alertness, c.confidence, 1.0},
      {1.0 - d.value, d.confidence, 0.4},
  });

  // valence — reserved in v1; ships inert (neutral slot at confidence 0, PSV §4.1 note).
  const psv::Dimension valence = {0.5, 0.0};

  psv::StateVector v;
  v.schema_version = psv::kSchemaVersion;
  v.vertical = meta.vertical;
  v.update_timestamp_ms = meta.timestamp_ms;
  v.sequence = meta.sequence;
  v.mode_hint = std::nullopt; // v1 drives audio purely from the continuous vector
  v.arousal = arousal;
  v.valence = valence;
  v.cognitive_load = cognitive_load;
  v.readiness = readiness;
  return v;
}

} // namespace prism::pce
