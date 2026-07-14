#pragma once

// Real-time-safe PSV representation for the inference → audio handoff (CLAUDE.md
// real-time rule 2). StateVector holds std::string members, so copying it can allocate —
// forbidden on the audio thread. RtStateVector is its trivially copyable mirror: fixed
// dimension arrays in spec §4.1 order and a bounded mode-hint buffer.

#include "psv/psv.h"

#include <cstdint>
#include <type_traits>

namespace prism::psv {

// Dimension indices, spec §4.1 order. Shared by every consumer of the arrays.
enum RtDimension : int {
  kArousal = 0,
  kValence = 1,
  kCognitiveLoad = 2,
  kReadiness = 3,
  kDimensionCount = 4,
};

struct RtStateVector {
  double value[kDimensionCount] = {0.5, 0.5, 0.5, 0.5};
  double confidence[kDimensionCount] = {0, 0, 0, 0};
  int64_t update_timestamp_ms = 0;
  int64_t sequence = -1; // < 0 = absent
  Vertical vertical = Vertical::Aqademiq;
  // NUL-terminated; empty = null hint. Sized for every §9 hint ("venue_energize" = 14);
  // longer unknown hints are truncated — hints are advisory (§9), truncation is lossy but
  // safe, and the wire/JSON path is unaffected.
  char mode_hint[24] = {};

  // Spec §8.1 confidence weighting, same as Dimension::effective().
  double effective(int dim) const { return 0.5 + (value[dim] - 0.5) * confidence[dim]; }
};

static_assert(std::is_trivially_copyable_v<RtStateVector>,
              "the audio-thread handoff requires a trivially copyable snapshot");

// StateVector → snapshot: no allocation (writer side may run on the inference thread).
RtStateVector to_rt(const StateVector& v);

// Snapshot → StateVector: allocates strings; NON-real-time side only (logging, tests).
StateVector from_rt(const RtStateVector& v);

} // namespace prism::psv
