#pragma once

// Heuristic fusion (verbatim port of the probe's fusion.ts) — the hand-written mapping
// from PCE inputs to a Prism State Vector. Every coefficient is a validated dogfooding
// finding from Slice 01; golden-trace parity (1e-6) is the acceptance bar, so DO NOT
// "improve" anything here in this slice.
//
// Firewall: produces ONLY a PSV (values + confidences). It knows nothing about audio, and
// it does NOT apply the consumer's confidence weighting (psv::Dimension::effective) — that
// is the PGAE's job. Heuristic only — no ML.

#include "pce/attention.h"
#include "pce/circadian.h"
#include "pce/reading.h"
#include "psv/psv.h"

#include <cstdint>
#include <optional>

namespace prism::pce {

struct FusionInputs {
  AdapterReading deadline;    // deadline pressure
  AttentionDrivers attention; // how present (activity) and how fragmented (scatter)
  CircadianReading circadian; // alertness prior
};

struct FusionMeta {
  int64_t timestamp_ms = 0;
  std::optional<int64_t> sequence;
  psv::Vertical vertical = psv::Vertical::Aqademiq;
};

psv::StateVector fuse_to_psv(const FusionInputs& inputs, const FusionMeta& meta);

} // namespace prism::pce
