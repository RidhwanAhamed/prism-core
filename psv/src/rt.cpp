#include "psv/rt.h"

#include <cstring>

namespace prism::psv {

RtStateVector to_rt(const StateVector& v) {
  RtStateVector rt;
  const Dimension* dims[kDimensionCount] = {&v.arousal, &v.valence, &v.cognitive_load,
                                            &v.readiness};
  for (int i = 0; i < kDimensionCount; ++i) {
    rt.value[i] = dims[i]->value;
    rt.confidence[i] = dims[i]->confidence;
  }
  rt.update_timestamp_ms = v.update_timestamp_ms;
  rt.sequence = v.sequence ? *v.sequence : -1;
  rt.vertical = v.vertical;
  if (v.mode_hint) {
    // Bounded copy, always NUL-terminated; overlong hints truncate (see rt.h).
    std::strncpy(rt.mode_hint, v.mode_hint->c_str(), sizeof rt.mode_hint - 1);
    rt.mode_hint[sizeof rt.mode_hint - 1] = '\0';
  }
  return rt;
}

StateVector from_rt(const RtStateVector& rt) {
  StateVector v;
  v.arousal = {rt.value[kArousal], rt.confidence[kArousal]};
  v.valence = {rt.value[kValence], rt.confidence[kValence]};
  v.cognitive_load = {rt.value[kCognitiveLoad], rt.confidence[kCognitiveLoad]};
  v.readiness = {rt.value[kReadiness], rt.confidence[kReadiness]};
  v.update_timestamp_ms = rt.update_timestamp_ms;
  if (rt.sequence >= 0) {
    v.sequence = rt.sequence;
  }
  v.vertical = rt.vertical;
  if (rt.mode_hint[0] != '\0') {
    v.mode_hint = std::string(rt.mode_hint);
  }
  return v;
}

} // namespace prism::psv
