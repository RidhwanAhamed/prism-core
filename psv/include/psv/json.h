#pragma once

// JSON transport (spec §5) — the reference serialization for aqademiq and
// venues. Hand-rolled against the fixed PSV shape: the stack is locked to
// two dependencies, and this schema is small enough to own outright.

#include "psv/psv.h"

#include <optional>
#include <string>
#include <string_view>

namespace prism::psv {

// Emits the §5.1 reference shape, compact (no whitespace), fields in spec
// order; `sequence` omitted when absent, `mode_hint` emitted as null when
// absent. Doubles are printed at round-trip precision, so
// from_json(to_json(v)) == v exactly. Number formatting is locale-independent
// (the embedding host owns the process locale; wire format must not follow
// it). Assumes an is_valid() vector: NaN or infinity in a dimension has no
// JSON representation, and a non-UTF-8 mode_hint has no RFC 8259 encoding.
std::string to_json(const StateVector& v);

// Parses and validates one PSV document against the §5.2 schema rules:
// required fields present and correctly typed, vertical in its enum,
// values/confidences within [0,1], plus the v1 contract that all four §4.1
// dimensions are present. Forward-compatible per §10: unknown top-level
// fields, unknown dimensions in `state`, unknown fields inside a dimension,
// and unknown mode_hint strings are accepted (ignored or preserved as-is).
// Returns nullopt on any violation; if `error` is non-null it receives a
// human-readable reason.
std::optional<StateVector> from_json(std::string_view json, std::string* error = nullptr);

} // namespace prism::psv
