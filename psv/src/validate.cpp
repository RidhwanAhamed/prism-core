#include "psv/validate.h"

#include <cctype>

namespace prism::psv {

namespace {

// Spec §5.2: ^\d+\.\d+\.\d+$
bool is_semver_shaped(const std::string& s) {
  int part = 0;
  size_t i = 0;
  while (part < 3) {
    size_t digits = 0;
    while (i < s.size() && std::isdigit(static_cast<unsigned char>(s[i]))) {
      ++i;
      ++digits;
    }
    if (digits == 0) {
      return false;
    }
    ++part;
    if (part < 3) {
      if (i >= s.size() || s[i] != '.') {
        return false;
      }
      ++i;
    }
  }
  return i == s.size();
}

// Written so NaN fails (any comparison with NaN is false).
bool in_unit_range(double x) {
  return x >= 0.0 && x <= 1.0;
}

void check_dimension(const char* name, const Dimension& d, std::vector<std::string>& errors) {
  if (!in_unit_range(d.value)) {
    errors.push_back(std::string(name) + ".value out of [0,1]");
  }
  if (!in_unit_range(d.confidence)) {
    errors.push_back(std::string(name) + ".confidence out of [0,1]");
  }
}

} // namespace

std::vector<std::string> validate(const StateVector& v) {
  std::vector<std::string> errors;
  if (!is_semver_shaped(v.schema_version)) {
    errors.push_back("schema_version is not MAJOR.MINOR.PATCH: \"" + v.schema_version + "\"");
  }
  if (v.update_timestamp_ms < 0) {
    errors.push_back("update_timestamp is negative");
  }
  if (v.sequence && *v.sequence < 0) {
    errors.push_back("sequence is negative");
  }
  check_dimension("arousal", v.arousal, errors);
  check_dimension("valence", v.valence, errors);
  check_dimension("cognitive_load", v.cognitive_load, errors);
  check_dimension("readiness", v.readiness, errors);
  return errors;
}

} // namespace prism::psv
