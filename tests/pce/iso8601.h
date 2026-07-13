#pragma once

// Test-only ISO-8601 → epoch-ms parser for golden-trace task due strings
// ("2026-07-14T18:00:00+04:00", "2026-07-14T10:30:00.000Z"). Matches JS Date.parse for
// this strict subset. Transport parsing is a platform-shell concern, so this lives in
// test support, not in the core.

#include <cstdint>
#include <optional>
#include <string_view>

namespace prism::test {

// Howard Hinnant's days-from-civil: days since 1970-01-01, proleptic Gregorian.
inline int64_t days_from_civil(int64_t y, int64_t m, int64_t d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const int64_t yoe = y - era * 400;
  const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

// Strict subset: YYYY-MM-DDTHH:MM:SS(.mmm)?(Z|±HH:MM)
inline std::optional<int64_t> parse_iso8601_ms(std::string_view s) {
  const auto digits = [&s](size_t pos, int n) -> std::optional<int64_t> {
    if (pos + static_cast<size_t>(n) > s.size()) {
      return std::nullopt;
    }
    int64_t v = 0;
    for (int i = 0; i < n; ++i) {
      const char c = s[pos + static_cast<size_t>(i)];
      if (c < '0' || c > '9') {
        return std::nullopt;
      }
      v = v * 10 + (c - '0');
    }
    return v;
  };
  const auto lit = [&s](size_t pos, char c) { return pos < s.size() && s[pos] == c; };

  const auto y = digits(0, 4);
  const auto mo = digits(5, 2);
  const auto d = digits(8, 2);
  const auto h = digits(11, 2);
  const auto mi = digits(14, 2);
  const auto se = digits(17, 2);
  if (!y || !mo || !d || !h || !mi || !se || !lit(4, '-') || !lit(7, '-') || !lit(10, 'T') ||
      !lit(13, ':') || !lit(16, ':')) {
    return std::nullopt;
  }
  size_t pos = 19;

  int64_t ms = 0;
  if (lit(pos, '.')) {
    const auto frac = digits(pos + 1, 3);
    if (!frac) {
      return std::nullopt;
    }
    ms = *frac;
    pos += 4;
  }

  int64_t offset_min = 0;
  if (lit(pos, 'Z')) {
    pos += 1;
  } else if (lit(pos, '+') || lit(pos, '-')) {
    const int64_t sign = s[pos] == '-' ? -1 : 1;
    const auto oh = digits(pos + 1, 2);
    const auto om = lit(pos + 3, ':') ? digits(pos + 4, 2) : std::nullopt;
    if (!oh || !om) {
      return std::nullopt;
    }
    offset_min = sign * (*oh * 60 + *om);
    pos += 6;
  } else {
    return std::nullopt;
  }
  if (pos != s.size()) {
    return std::nullopt;
  }

  const int64_t days = days_from_civil(*y, *mo, *d);
  return (days * 86'400 + *h * 3'600 + *mi * 60 + *se) * 1'000 + ms - offset_min * 60'000;
}

} // namespace prism::test
