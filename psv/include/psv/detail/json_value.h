#pragma once

// Internal (psv/src, not public surface): minimal strict JSON value parser, shared by the
// PSV transport (json.cpp) and by test tooling that reads golden-trace records. Full value
// grammar so unknown fields of any shape can be skipped (spec §10); no extensions — no
// comments, no trailing commas, no NaN/Infinity literals. Strings are validated as UTF-8.

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// POSIX per-thread locales: number conversion must not follow the host's process locale
// (see json.cpp). Shared here because both the parser (strtod) and the emitter (snprintf)
// need it.
#include <locale.h>
#ifdef __APPLE__
#include <xlocale.h>
#endif

namespace prism::psv::detail {

inline locale_t c_locale() {
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

// Parses one complete JSON document (no trailing content). Returns false and sets `error`
// (with a byte offset) on failure.
bool parse_json(std::string_view input, JValue& out, std::string& error);

} // namespace prism::psv::detail
