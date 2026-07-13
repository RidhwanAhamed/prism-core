#pragma once

// Internal (not part of the psv public surface): UTF-8 well-formedness, used
// to keep emitted JSON RFC 8259-conformant. Rejects overlong encodings,
// surrogate code points, and anything above U+10FFFF.

#include <string_view>

namespace prism::psv::detail {

inline bool is_valid_utf8(std::string_view s) {
  size_t i = 0;
  const auto byte = [&](size_t k) { return static_cast<unsigned char>(s[i + k]); };
  const auto cont = [&](size_t k) { return i + k < s.size() && (byte(k) & 0xC0) == 0x80; };
  while (i < s.size()) {
    const unsigned char c = byte(0);
    if (c < 0x80) {
      i += 1;
    } else if ((c & 0xE0) == 0xC0) {
      if (c < 0xC2 || !cont(1)) { // C0/C1 are always overlong
        return false;
      }
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      if (!cont(1) || !cont(2)) {
        return false;
      }
      if (c == 0xE0 && byte(1) < 0xA0) { // overlong
        return false;
      }
      if (c == 0xED && byte(1) > 0x9F) { // UTF-16 surrogate range
        return false;
      }
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      if (c > 0xF4 || !cont(1) || !cont(2) || !cont(3)) {
        return false;
      }
      if (c == 0xF0 && byte(1) < 0x90) { // overlong
        return false;
      }
      if (c == 0xF4 && byte(1) > 0x8F) { // above U+10FFFF
        return false;
      }
      i += 4;
    } else {
      return false;
    }
  }
  return true;
}

} // namespace prism::psv::detail
