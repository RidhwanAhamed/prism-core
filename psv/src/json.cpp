#include "psv/json.h"

#include "utf8.h"

#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

// POSIX per-thread locales: JSON's number grammar is locale-independent, but
// strtod and snprintf follow the process LC_NUMERIC — which the embedding
// HOST owns, not this library (GTK/Flutter hosts commonly call
// setlocale(LC_ALL, "")). Every number conversion below runs under a cached
// "C" locale so wire format never depends on host locale. uselocale is
// POSIX-2008: present on macOS, glibc, Android (21+), iOS. Windows is not a
// slice-02 target; it will need _create_locale/_snprintf_l when it arrives.
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace prism::psv {

// ---------------------------------------------------------------------------
// Emitter (spec §5.1)
// ---------------------------------------------------------------------------

namespace {

locale_t c_locale() {
  static locale_t loc = newlocale(LC_ALL_MASK, "C", static_cast<locale_t>(nullptr));
  return loc;
}

// RAII: switch this thread to the C locale for the enclosed conversions.
class CLocaleScope {
public:
  CLocaleScope() : old_(uselocale(c_locale())) {}
  ~CLocaleScope() { uselocale(old_); }
  CLocaleScope(const CLocaleScope&) = delete;
  CLocaleScope& operator=(const CLocaleScope&) = delete;

private:
  locale_t old_;
};

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
// Parser — minimal, strict JSON for the PSV transport. Full value grammar
// (so unknown fields of any shape can be skipped per §10), no extensions:
// no comments, no trailing commas, no NaN/Infinity literals.
// ---------------------------------------------------------------------------

namespace {

struct JValue {
  enum class Type { Null, Bool, Number, String, Object, Array };
  Type type = Type::Null;
  bool boolean = false;
  double number = 0.0;
  bool number_is_integer = false; // token had no '.', 'e' and fit int64
  int64_t integer = 0;
  std::string string;
  std::vector<std::pair<std::string, JValue>> object; // duplicate keys: last wins
  std::vector<JValue> array;

  const JValue* find(std::string_view key) const {
    const JValue* hit = nullptr;
    for (const auto& [k, val] : object) {
      if (k == key) {
        hit = &val;
      }
    }
    return hit;
  }
};

class Parser {
public:
  explicit Parser(std::string_view input) : s_(input) {}

  bool parse_document(JValue& out) {
    skip_ws();
    if (!parse_value(out, 0)) {
      return false;
    }
    skip_ws();
    if (pos_ != s_.size()) {
      return fail("trailing content after JSON document");
    }
    return true;
  }

  const std::string& error() const { return error_; }

private:
  static constexpr int kMaxDepth = 64;

  std::string_view s_;
  size_t pos_ = 0;
  std::string error_;

  bool fail(const std::string& why) {
    if (error_.empty()) {
      error_ = why + " (at byte " + std::to_string(pos_) + ")";
    }
    return false;
  }

  void skip_ws() {
    while (pos_ < s_.size() &&
           (s_[pos_] == ' ' || s_[pos_] == '\t' || s_[pos_] == '\n' || s_[pos_] == '\r')) {
      ++pos_;
    }
  }

  bool consume(char c) {
    if (pos_ < s_.size() && s_[pos_] == c) {
      ++pos_;
      return true;
    }
    return false;
  }

  bool parse_value(JValue& out, int depth) {
    if (depth > kMaxDepth) {
      return fail("nesting too deep");
    }
    if (pos_ >= s_.size()) {
      return fail("unexpected end of input");
    }
    switch (s_[pos_]) {
    case '{':
      return parse_object(out, depth);
    case '[':
      return parse_array(out, depth);
    case '"':
      out.type = JValue::Type::String;
      return parse_string(out.string);
    case 't':
      return parse_literal("true", out, JValue::Type::Bool, true);
    case 'f':
      return parse_literal("false", out, JValue::Type::Bool, false);
    case 'n':
      return parse_literal("null", out, JValue::Type::Null, false);
    default:
      return parse_number(out);
    }
  }

  bool parse_literal(std::string_view word, JValue& out, JValue::Type type, bool boolean) {
    if (s_.substr(pos_, word.size()) != word) {
      return fail("invalid literal");
    }
    pos_ += word.size();
    out.type = type;
    out.boolean = boolean;
    return true;
  }

  bool parse_object(JValue& out, int depth) {
    out.type = JValue::Type::Object;
    ++pos_; // '{'
    skip_ws();
    if (consume('}')) {
      return true;
    }
    for (;;) {
      skip_ws();
      if (pos_ >= s_.size() || s_[pos_] != '"') {
        return fail("expected object key");
      }
      std::string key;
      if (!parse_string(key)) {
        return false;
      }
      skip_ws();
      if (!consume(':')) {
        return fail("expected ':' after object key");
      }
      skip_ws();
      JValue value;
      if (!parse_value(value, depth + 1)) {
        return false;
      }
      out.object.emplace_back(std::move(key), std::move(value));
      skip_ws();
      if (consume(',')) {
        continue;
      }
      if (consume('}')) {
        return true;
      }
      return fail("expected ',' or '}' in object");
    }
  }

  bool parse_array(JValue& out, int depth) {
    out.type = JValue::Type::Array;
    ++pos_; // '['
    skip_ws();
    if (consume(']')) {
      return true;
    }
    for (;;) {
      skip_ws();
      JValue value;
      if (!parse_value(value, depth + 1)) {
        return false;
      }
      out.array.push_back(std::move(value));
      skip_ws();
      if (consume(',')) {
        continue;
      }
      if (consume(']')) {
        return true;
      }
      return fail("expected ',' or ']' in array");
    }
  }

  static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    return -1;
  }

  bool parse_hex4(uint32_t& out) {
    if (pos_ + 4 > s_.size()) {
      return fail("truncated \\u escape");
    }
    out = 0;
    for (int i = 0; i < 4; ++i) {
      int d = hex_digit(s_[pos_ + static_cast<size_t>(i)]);
      if (d < 0) {
        return fail("invalid \\u escape");
      }
      out = out * 16 + static_cast<uint32_t>(d);
    }
    pos_ += 4;
    return true;
  }

  static void append_utf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
      out += static_cast<char>(cp);
    } else if (cp < 0x800) {
      out += static_cast<char>(0xC0 | (cp >> 6));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += static_cast<char>(0xE0 | (cp >> 12));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
      out += static_cast<char>(0xF0 | (cp >> 18));
      out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
      out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out += static_cast<char>(0x80 | (cp & 0x3F));
    }
  }

  bool parse_string(std::string& out) {
    ++pos_; // opening '"'
    out.clear();
    for (;;) {
      if (pos_ >= s_.size()) {
        return fail("unterminated string");
      }
      char c = s_[pos_];
      if (c == '"') {
        ++pos_;
        // RFC 8259 §8.1: interchange JSON is UTF-8. Escape-decoded sequences
        // are valid by construction; this catches raw invalid bytes.
        if (!detail::is_valid_utf8(out)) {
          return fail("invalid UTF-8 in string");
        }
        return true;
      }
      if (static_cast<unsigned char>(c) < 0x20) {
        return fail("raw control character in string");
      }
      if (c != '\\') {
        out += c;
        ++pos_;
        continue;
      }
      ++pos_; // '\'
      if (pos_ >= s_.size()) {
        return fail("unterminated escape");
      }
      char e = s_[pos_++];
      switch (e) {
      case '"':
        out += '"';
        break;
      case '\\':
        out += '\\';
        break;
      case '/':
        out += '/';
        break;
      case 'b':
        out += '\b';
        break;
      case 'f':
        out += '\f';
        break;
      case 'n':
        out += '\n';
        break;
      case 'r':
        out += '\r';
        break;
      case 't':
        out += '\t';
        break;
      case 'u': {
        uint32_t cp = 0;
        if (!parse_hex4(cp)) {
          return false;
        }
        if (cp >= 0xD800 && cp <= 0xDBFF) { // high surrogate: pair required
          if (pos_ + 1 >= s_.size() || s_[pos_] != '\\' || s_[pos_ + 1] != 'u') {
            return fail("lone high surrogate");
          }
          pos_ += 2;
          uint32_t low = 0;
          if (!parse_hex4(low)) {
            return false;
          }
          if (low < 0xDC00 || low > 0xDFFF) {
            return fail("invalid low surrogate");
          }
          cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
          return fail("lone low surrogate");
        }
        append_utf8(out, cp);
        break;
      }
      default:
        return fail("invalid escape character");
      }
    }
  }

  bool parse_number(JValue& out) {
    const size_t start = pos_;
    bool is_integer = true;

    if (consume('-')) {
    }
    if (consume('0')) {
      // leading zero: no further int digits allowed
    } else {
      size_t digits = 0;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        ++pos_;
        ++digits;
      }
      if (digits == 0) {
        return fail("invalid number");
      }
    }
    if (consume('.')) {
      is_integer = false;
      size_t digits = 0;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        ++pos_;
        ++digits;
      }
      if (digits == 0) {
        return fail("digits required after decimal point");
      }
    }
    if (pos_ < s_.size() && (s_[pos_] == 'e' || s_[pos_] == 'E')) {
      is_integer = false;
      ++pos_;
      if (pos_ < s_.size() && (s_[pos_] == '+' || s_[pos_] == '-')) {
        ++pos_;
      }
      size_t digits = 0;
      while (pos_ < s_.size() && s_[pos_] >= '0' && s_[pos_] <= '9') {
        ++pos_;
        ++digits;
      }
      if (digits == 0) {
        return fail("digits required in exponent");
      }
    }

    const std::string token(s_.substr(start, pos_ - start));
    out.type = JValue::Type::Number;

    if (is_integer) {
      int64_t i = 0;
      auto [ptr, ec] = std::from_chars(token.data(), token.data() + token.size(), i);
      if (ec == std::errc() && ptr == token.data() + token.size()) {
        out.number_is_integer = true;
        out.integer = i;
        out.number = static_cast<double>(i);
        return true;
      }
      // fell out of int64 range: keep as double below
    }
    {
      CLocaleScope c_locale_scope; // host may run a comma-decimal locale
      char* end = nullptr;
      out.number = std::strtod(token.c_str(), &end);
      if (end != token.c_str() + token.size()) {
        return fail("number token not fully parseable"); // defense in depth
      }
    }
    out.number_is_integer = false;
    return true;
  }
};

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
  Parser parser(json);
  if (!parser.parse_document(root)) {
    return fail(parser.error());
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
