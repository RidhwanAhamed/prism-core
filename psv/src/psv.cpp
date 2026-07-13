#include "psv/psv.h"

namespace prism::psv {

const char* to_string(Vertical v) {
  switch (v) {
  case Vertical::Aqademiq:
    return "aqademiq";
  case Vertical::Venues:
    return "venues";
  case Vertical::Automotive:
    return "automotive";
  }
  return "aqademiq"; // unreachable; keeps -Werror happy across compilers
}

std::optional<Vertical> vertical_from_string(std::string_view s) {
  if (s == "aqademiq") {
    return Vertical::Aqademiq;
  }
  if (s == "venues") {
    return Vertical::Venues;
  }
  if (s == "automotive") {
    return Vertical::Automotive;
  }
  return std::nullopt;
}

bool operator==(const StateVector& a, const StateVector& b) {
  auto dim_eq = [](const Dimension& x, const Dimension& y) {
    return x.value == y.value && x.confidence == y.confidence;
  };
  return a.schema_version == b.schema_version && a.vertical == b.vertical &&
         a.sequence == b.sequence && a.update_timestamp_ms == b.update_timestamp_ms &&
         a.mode_hint == b.mode_hint && dim_eq(a.arousal, b.arousal) &&
         dim_eq(a.valence, b.valence) && dim_eq(a.cognitive_load, b.cognitive_load) &&
         dim_eq(a.readiness, b.readiness);
}

StateVector neutral(Vertical vertical, int64_t update_timestamp_ms) {
  StateVector v;
  v.vertical = vertical;
  v.update_timestamp_ms = update_timestamp_ms;
  // Dimensions default to {0.5, 0.0} and mode_hint to null — exactly §6.
  return v;
}

} // namespace prism::psv
