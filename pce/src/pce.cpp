#include "pce/pce.h"

#include "pce/circadian.h"

#include <cmath>

namespace prism::pce {

psv::StateVector Pce::start(int64_t now_ms) {
  psv::StateVector neutral = psv::neutral(opts_.vertical, now_ms);
  neutral.sequence = seq_; // the probe stamps sequence 0 on the cold-start vector
  note_emit(neutral, now_ms);
  return neutral;
}

std::optional<psv::StateVector> Pce::evaluate(int64_t now_ms) {
  FusionMeta meta;
  meta.timestamp_ms = now_ms;
  meta.sequence = seq_ + 1; // candidate sequence; seq_ only advances on emission
  meta.vertical = opts_.vertical;
  const psv::StateVector candidate = fuse_to_psv(read_inputs(now_ms), meta);

  const bool cadence_elapsed =
      static_cast<double>(now_ms) - last_emit_ms_ >= static_cast<double>(opts_.cadence_ms);
  if (cadence_elapsed || significantly_changed(candidate)) {
    note_emit(candidate, now_ms);
    return candidate;
  }
  return std::nullopt;
}

FusionInputs Pce::read_inputs(int64_t now_ms) {
  FusionInputs inputs;
  inputs.deadline = deadline_proximity(tasks_, now_ms, opts_.deadline);
  inputs.attention = attention_.read_drivers(now_ms);
  inputs.circadian = circadian_from_epoch(now_ms, opts_.tz_offset_min);
  return inputs;
}

bool Pce::significantly_changed(const psv::StateVector& candidate) const {
  if (!last_psv_) {
    return true; // first evaluation always emits
  }
  const psv::StateVector& prev = *last_psv_;
  const psv::Dimension* candidate_dims[] = {&candidate.arousal, &candidate.valence,
                                            &candidate.cognitive_load, &candidate.readiness};
  const psv::Dimension* prev_dims[] = {&prev.arousal, &prev.valence, &prev.cognitive_load,
                                       &prev.readiness};
  for (int i = 0; i < 4; ++i) {
    if (std::fabs(candidate_dims[i]->value - prev_dims[i]->value) >= opts_.significant_delta) {
      return true;
    }
  }
  return false;
}

void Pce::note_emit(const psv::StateVector& v, int64_t now_ms) {
  if (v.sequence) {
    seq_ = *v.sequence;
  }
  last_emit_ms_ = static_cast<double>(now_ms);
  last_psv_ = v;
}

} // namespace prism::pce
