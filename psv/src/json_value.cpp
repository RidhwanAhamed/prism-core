#include "psv/detail/json_value.h"

#include "utf8.h"

#include <charconv>
#include <cstdlib>

namespace prism::psv::detail {

namespace {

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
        // RFC 8259 §8.1: interchange JSON is UTF-8. Escape-decoded sequences are valid by
        // construction; this catches raw invalid bytes.
        if (!is_valid_utf8(out)) {
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

} // namespace

bool parse_json(std::string_view input, JValue& out, std::string& error) {
  Parser parser(input);
  if (!parser.parse_document(out)) {
    error = parser.error();
    return false;
  }
  return true;
}

} // namespace prism::psv::detail
