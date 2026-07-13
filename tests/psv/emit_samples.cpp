// Prints representative PSVs as JSONL for validation against the spec's own
// JSON Schema (tests/psv/psv.schema.json) by an independent implementation.
// Wired up as the psv_schema_conformance ctest when python3 + jsonschema are
// available; see run_schema_check.cmake.

#include "psv/json.h"
#include "psv/psv.h"

#include <cstdio>

namespace psv = prism::psv;

int main() {
  // Cold start, one per vertical.
  std::printf("%s\n", psv::to_json(psv::neutral(psv::Vertical::Aqademiq, 0)).c_str());
  std::printf("%s\n", psv::to_json(psv::neutral(psv::Vertical::Venues, 1750000000000)).c_str());
  std::printf("%s\n", psv::to_json(psv::neutral(psv::Vertical::Automotive, 1)).c_str());

  // The spec §5.1 reference vector.
  psv::StateVector ref;
  ref.sequence = 4821;
  ref.update_timestamp_ms = 1750000000000;
  ref.mode_hint = psv::mode_hint::kDeepWork;
  ref.arousal = {0.72, 0.81};
  ref.valence = {0.55, 0.30};
  ref.cognitive_load = {0.85, 0.77};
  ref.readiness = {0.31, 0.68};
  std::printf("%s\n", psv::to_json(ref).c_str());

  // Boundary values and awkward doubles.
  auto edge = psv::neutral(psv::Vertical::Venues, 9007199254740993);
  edge.sequence = 0;
  edge.mode_hint = psv::mode_hint::kVenueSettle;
  edge.arousal = {0.0, 1.0};
  edge.valence = {1.0, 0.0};
  edge.cognitive_load = {0.1 + 0.2, 1.0 / 3.0};
  edge.readiness = {1e-17, 0.9999999999999999};
  std::printf("%s\n", psv::to_json(edge).c_str());

  return 0;
}
