#pragma once

// Validation of constructed vectors against the producer guarantees the PCE
// must uphold (spec §3 invariants + the §5.2 schema constraints).

#include "psv/psv.h"

#include <string>
#include <vector>

namespace prism::psv {

// Returns one human-readable message per violated rule; empty == valid.
// Checks: semver-shaped schema_version, all dimension values/confidences
// finite and within [0,1], non-negative timestamp, non-negative sequence,
// UTF-8 well-formed mode_hint (the JSON transport is UTF-8 per RFC 8259).
std::vector<std::string> validate(const StateVector& v);

inline bool is_valid(const StateVector& v) {
  return validate(v).empty();
}

} // namespace prism::psv
