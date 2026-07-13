#include "psv/json.h"

#include "psv/detail/json_value.h"

#include <cmath>
#include <cstdio>

namespace prism::psv {

using detail::CLocaleScope;
using detail::JValue;

// ---------------------------------------------------------------------------
// Emitter (spec §5.1)
// ---------------------------------------------------------------------------

namespace {

void append_escaped(std::string& out, std::string_view s) {
  out += '"';
  for (char c : s) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "\\u%04x", c);
        out += buf;
      } else {
        out += c; // UTF-8 bytes pass through untouched
      }
    }
  }
  out += '"';
}

// %.17g guarantees a double survives text round-trip exactly.
void append_double(std::string& out, double d) {
  CLocaleScope c_locale_scope; // '.' decimal point regardless of host locale
  char buf[32];
  std::snprintf(buf, sizeof buf, "%.17g", d);
  out += buf;
}

void append_int(std::string& out, int64_t i) {
  char buf[24];
  std::snprintf(buf, sizeof buf, "%lld", static_cast<long long>(i));
  out += buf;
}

void append_dimension(std::string& out, const char* name, const Dimension& d) {
  out += '"';
  out += name;
  out += "\":{\"value\":";
  append_double(out, d.value);
  out += ",\"confidence\":";
  append_double(out, d.confidence);
  out += '}';
}

} // namespace

std::string to_json(const StateVector& v) {
  std::string out;
  out.reserve(320);
  out += "{\"schema_version\":";
  append_escaped(out, v.schema_version);
  out += ",\"vertical\":\"";
  out += to_string(v.vertical);
  out += '"';
  if (v.sequence) {
    out += ",\"sequence\":";
    append_int(out, *v.sequence);
  }
  out += ",\"update_timestamp\":";
  append_int(out, v.update_timestamp_ms);
  out += ",\"mode_hint\":";
  if (v.mode_hint) {
    append_escaped(out, *v.mode_hint);
  } else {
    out += "null";
  }
  out += ",\"state\":{";
  append_dimension(out, "arousal", v.arousal);
  out += ',';
  append_dimension(out, "valence", v.valence);
  out += ',';
  append_dimension(out, "cognitive_load", v.cognitive_load);
  out += ',';
  append_dimension(out, "readiness", v.readiness);
  out += "}}";
  return out;
}

// ---------------------------------------------------------------------------
// Parsing + schema-shape validation (spec §5.2). The JSON value grammar lives
// in json_value.{h,cpp}; this file extracts and validates the PSV shape.
// ---------------------------------------------------------------------------

namespace {

// JSON Schema "integer" accepts integral-valued floats (1e3 == 1000). Mirror
// that: exact int64 token, or a double with zero fractional part inside the
// exactly-representable range.
bool as_int64(const JValue& v, int64_t& out) {
  if (v.type != JValue::Type::Number) {
    return false;
  }
  if (v.number_is_integer) {
    out = v.integer;
    return true;
  }
  if (std::floor(v.number) == v.number && std::fabs(v.number) <= 9007199254740992.0) {
    out = static_cast<int64_t>(v.number);
    return true;
  }
  return false;
}

bool parse_dimension(const JValue& state, const char* name, Dimension& out, std::string& why) {
  const JValue* d = state.find(name);
  if (!d) {
    why = std::string("state.") + name + " is missing (required in v1, spec §4.1)";
    return false;
  }
  if (d->type != JValue::Type::Object) {
    why = std::string("state.") + name + " is not an object";
    return false;
  }
  const JValue* value = d->find("value");
  const JValue* confidence = d->find("confidence");
  if (!value || value->type != JValue::Type::Number) {
    why = std::string("state.") + name + ".value is missing or not a number";
    return false;
  }
  if (!confidence || confidence->type != JValue::Type::Number) {
    why = std::string("state.") + name + ".confidence is missing or not a number";
    return false;
  }
  if (!(value->number >= 0.0 && value->number <= 1.0)) {
    why = std::string("state.") + name + ".value out of [0,1]";
    return false;
  }
  if (!(confidence->number >= 0.0 && confidence->number <= 1.0)) {
    why = std::string("state.") + name + ".confidence out of [0,1]";
    return false;
  }
  out.value = value->number;
  out.confidence = confidence->number;
  return true;
}

bool is_semver_shaped_ref(const std::string& s) {
  // Same rule as validate.cpp (spec §5.2 pattern); duplicated locally to keep
  // the parser self-contained.
  int part = 0;
  size_t i = 0;
  while (part < 3) {
    size_t digits = 0;
    while (i < s.size() && s[i] >= '0' && s[i] <= '9') {
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

} // namespace

std::optional<StateVector> from_json(std::string_view json, std::string* error) {
  auto fail = [&](const std::string& why) -> std::optional<StateVector> {
    if (error) {
      *error = why;
    }
    return std::nullopt;
  };

  JValue root;
  std::string parse_error;
  if (!detail::parse_json(json, root, parse_error)) {
    return fail(parse_error);
  }
  if (root.type != JValue::Type::Object) {
    return fail("document is not a JSON object");
  }

  StateVector v;

  const JValue* schema_version = root.find("schema_version");
  if (!schema_version || schema_version->type != JValue::Type::String) {
    return fail("schema_version is missing or not a string");
  }
  if (!is_semver_shaped_ref(schema_version->string)) {
    return fail("schema_version is not MAJOR.MINOR.PATCH: \"" + schema_version->string + "\"");
  }
  v.schema_version = schema_version->string;

  const JValue* vertical = root.find("vertical");
  if (!vertical || vertical->type != JValue::Type::String) {
    return fail("vertical is missing or not a string");
  }
  auto vert = vertical_from_string(vertical->string);
  if (!vert) {
    return fail("vertical is not one of aqademiq|venues|automotive: \"" + vertical->string + "\"");
  }
  v.vertical = *vert;

  const JValue* timestamp = root.find("update_timestamp");
  int64_t ts = 0;
  if (!timestamp || !as_int64(*timestamp, ts)) {
    return fail("update_timestamp is missing or not an integer");
  }
  if (ts < 0) {
    return fail("update_timestamp is negative");
  }
  v.update_timestamp_ms = ts;

  if (const JValue* sequence = root.find("sequence")) {
    int64_t seq = 0;
    if (!as_int64(*sequence, seq)) {
      return fail("sequence is not an integer");
    }
    if (seq < 0) {
      return fail("sequence is negative");
    }
    v.sequence = seq;
  }

  if (const JValue* hint = root.find("mode_hint")) {
    if (hint->type == JValue::Type::String) {
      v.mode_hint = hint->string; // unknown hints preserved (spec §10)
    } else if (hint->type != JValue::Type::Null) {
      return fail("mode_hint is neither a string nor null");
    }
  }

  const JValue* state = root.find("state");
  if (!state || state->type != JValue::Type::Object) {
    return fail("state is missing or not an object");
  }
  std::string why;
  if (!parse_dimension(*state, "arousal", v.arousal, why) ||
      !parse_dimension(*state, "valence", v.valence, why) ||
      !parse_dimension(*state, "cognitive_load", v.cognitive_load, why) ||
      !parse_dimension(*state, "readiness", v.readiness, why)) {
    return fail(why);
  }
  // Unknown dimensions in state and unknown top-level fields: ignored (§10).

  return v;
}

} // namespace prism::psv
