#include <gtest/gtest.h>

#include "psv/json.h"
#include "psv/psv.h"

#include <clocale>
#include <cmath>
#include <cstdio>
#include <string>

namespace psv = prism::psv;

namespace {

psv::StateVector sample() {
  psv::StateVector v;
  v.vertical = psv::Vertical::Aqademiq;
  v.sequence = 4821;
  v.update_timestamp_ms = 1750000000000;
  v.mode_hint = psv::mode_hint::kDeepWork;
  v.arousal = {0.72, 0.81};
  v.valence = {0.55, 0.30};
  v.cognitive_load = {0.85, 0.77};
  v.readiness = {0.31, 0.68};
  return v;
}

} // namespace

// --- The spec's own reference document (spec §5.1, verbatim) -------------------

TEST(PsvJson, ParsesTheSpecReferenceDocument) {
  const char* doc = R"({
  "schema_version": "1.0.0",
  "vertical": "aqademiq",
  "sequence": 4821,
  "update_timestamp": 1750000000000,
  "mode_hint": "deep_work",
  "state": {
    "arousal":        { "value": 0.72, "confidence": 0.81 },
    "valence":        { "value": 0.55, "confidence": 0.30 },
    "cognitive_load": { "value": 0.85, "confidence": 0.77 },
    "readiness":      { "value": 0.31, "confidence": 0.68 }
  }
})";
  std::string error;
  auto v = psv::from_json(doc, &error);
  ASSERT_TRUE(v.has_value()) << error;
  EXPECT_EQ(*v, sample());
}

// --- Round trip -----------------------------------------------------------------

TEST(PsvJson, RoundTripIsExact) {
  auto v = sample();
  // Deliberately awkward doubles (still in [0,1]).
  v.arousal = {0.1 + 0.2, 1.0 / 3.0};
  v.valence = {std::nextafter(0.0, 1.0), std::nextafter(1.0, 0.0)};
  std::string error;
  auto back = psv::from_json(psv::to_json(v), &error);
  ASSERT_TRUE(back.has_value()) << error;
  EXPECT_EQ(*back, v);
}

TEST(PsvJson, RoundTripNeutralVector) {
  const auto v = psv::neutral(psv::Vertical::Venues, 0);
  const std::string json = psv::to_json(v);
  EXPECT_NE(json.find("\"mode_hint\":null"), std::string::npos);
  EXPECT_EQ(json.find("\"sequence\""), std::string::npos); // omitted when absent
  auto back = psv::from_json(json);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, v);
}

TEST(PsvJson, RoundTripLargeTimestampExactly) {
  auto v = psv::neutral(psv::Vertical::Automotive, 9007199254740993); // 2^53 + 1
  v.sequence = (int64_t{1} << 62) + 7;
  auto back = psv::from_json(psv::to_json(v));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->update_timestamp_ms, 9007199254740993);
  EXPECT_EQ(back->sequence, v.sequence);
}

TEST(PsvJson, RoundTripEscapedModeHint) {
  auto v = psv::neutral(psv::Vertical::Aqademiq, 1);
  v.mode_hint = "we\"ird\\hi\nnt\t\xC3\xA9"; // quotes, backslash, control, UTF-8
  auto back = psv::from_json(psv::to_json(v));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, v);
}

// --- Forward compatibility (spec §10) ---------------------------------------------

TEST(PsvJson, IgnoresUnknownTopLevelFieldsAndDimensions) {
  const char* doc = R"({
    "schema_version": "1.1.0",
    "vertical": "venues",
    "update_timestamp": 5,
    "future_field": [1, {"nested": true}, null],
    "state": {
      "arousal":        {"value": 0.5, "confidence": 0.0},
      "valence":        {"value": 0.5, "confidence": 0.0},
      "cognitive_load": {"value": 0.5, "confidence": 0.0},
      "readiness":      {"value": 0.5, "confidence": 0.0},
      "social_density": {"value": 0.9, "confidence": 0.4}
    }
  })";
  std::string error;
  auto v = psv::from_json(doc, &error);
  ASSERT_TRUE(v.has_value()) << error;
  EXPECT_EQ(v->schema_version, "1.1.0");
}

TEST(PsvJson, PreservesUnknownModeHint) {
  auto doc = psv::to_json(psv::neutral(psv::Vertical::Aqademiq, 1));
  const std::string needle = "\"mode_hint\":null";
  doc.replace(doc.find(needle), needle.size(), "\"mode_hint\":\"hyper_focus_v3\"");
  auto v = psv::from_json(doc);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->mode_hint, "hyper_focus_v3");
}

TEST(PsvJson, MissingModeHintFieldReadsAsNull) {
  const char* doc = R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":1,
    "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
    "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})";
  auto v = psv::from_json(doc);
  ASSERT_TRUE(v.has_value());
  EXPECT_FALSE(v->mode_hint.has_value());
}

// --- Schema enforcement (spec §5.2 + v1 contract §4.1) ------------------------------

TEST(PsvJson, RejectsMissingRequiredFields) {
  const std::string full = psv::to_json(sample());
  for (const char* field : {"\"schema_version\"", "\"vertical\"", "\"update_timestamp\"",
                            "\"state\"", "\"readiness\""}) {
    // Cheap field removal: rename the key so lookup fails.
    std::string broken = full;
    const auto at = broken.find(field);
    ASSERT_NE(at, std::string::npos);
    broken.replace(at, 2, "\"x");
    std::string error;
    EXPECT_FALSE(psv::from_json(broken, &error).has_value()) << "field: " << field;
    EXPECT_FALSE(error.empty());
  }
}

TEST(PsvJson, RejectsSchemaViolations) {
  const struct {
    const char* label;
    const char* doc;
  } cases[] = {
      {"bad vertical", R"({"schema_version":"1.0.0","vertical":"desktop","update_timestamp":1,
        "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
      {"bad semver", R"({"schema_version":"1.0","vertical":"aqademiq","update_timestamp":1,
        "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
      {"value out of range",
       R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":1,
        "state":{"arousal":{"value":1.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
      {"negative timestamp",
       R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":-1,
        "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
      {"fractional timestamp",
       R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":1.5,
        "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
      {"mode_hint wrong type",
       R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":1,
        "mode_hint":7,
        "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
      {"dimension not object",
       R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":1,
        "state":{"arousal":0.5,"valence":{"value":0.5,"confidence":0},
        "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})"},
  };
  for (const auto& c : cases) {
    std::string error;
    EXPECT_FALSE(psv::from_json(c.doc, &error).has_value()) << c.label;
    EXPECT_FALSE(error.empty()) << c.label;
  }
}

TEST(PsvJson, IntegerValuedFloatTimestampAccepted) {
  // JSON Schema "integer" semantics: 1.75e12 has zero fractional part.
  const char* doc = R"({"schema_version":"1.0.0","vertical":"aqademiq","update_timestamp":1.75e12,
    "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
    "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})";
  auto v = psv::from_json(doc);
  ASSERT_TRUE(v.has_value());
  EXPECT_EQ(v->update_timestamp_ms, 1750000000000);
}

// --- Locale independence -------------------------------------------------------------
// The host owns the process locale (GTK/Flutter hosts call setlocale(LC_ALL, "")).
// Wire format must not follow it: under a comma-decimal locale, snprintf/strtod
// would otherwise emit "0,72" (invalid JSON) and silently parse "0.72" as 0.0.

class CommaDecimalLocaleTest : public ::testing::Test {
protected:
  void SetUp() override {
    const char* current = setlocale(LC_ALL, nullptr);
    saved_ = current ? current : "C";
    for (const char* name : {"de_DE.UTF-8", "fr_FR.UTF-8", "de_DE", "fr_FR"}) {
      if (setlocale(LC_ALL, name) != nullptr) {
        char buf[8];
        std::snprintf(buf, sizeof buf, "%.1f", 1.5);
        if (buf[1] == ',') {
          return; // comma-decimal locale active
        }
      }
    }
    setlocale(LC_ALL, saved_.c_str());
    GTEST_SKIP() << "no comma-decimal locale available on this system";
  }
  void TearDown() override { setlocale(LC_ALL, saved_.c_str()); }

private:
  std::string saved_;
};

TEST_F(CommaDecimalLocaleTest, EmitsValidJsonAndParsesExactly) {
  const auto v = sample();
  const std::string json = psv::to_json(v);
  EXPECT_EQ(json.find(','), json.find(",\"")) << "decimal comma leaked into: " << json;
  std::string error;
  auto back = psv::from_json(json, &error);
  ASSERT_TRUE(back.has_value()) << error;
  EXPECT_EQ(*back, v);
  // And canonical wire JSON parses to the exact fractional values.
  auto parsed = psv::from_json(R"({"schema_version":"1.0.0","vertical":"aqademiq",
    "update_timestamp":1,"state":{"arousal":{"value":0.72,"confidence":0.81},
    "valence":{"value":0.5,"confidence":0},"cognitive_load":{"value":0.5,"confidence":0},
    "readiness":{"value":0.5,"confidence":0}}})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_DOUBLE_EQ(parsed->arousal.value, 0.72);
  EXPECT_DOUBLE_EQ(parsed->arousal.confidence, 0.81);
}

// --- UTF-8 enforcement (RFC 8259 §8.1) --------------------------------------------------

TEST(PsvJson, RejectsInvalidUtf8InStrings) {
  std::string doc = psv::to_json(psv::neutral(psv::Vertical::Aqademiq, 1));
  const std::string needle = "\"mode_hint\":null";
  doc.replace(doc.find(needle), needle.size(), "\"mode_hint\":\"\x80\xFF\"");
  std::string error;
  EXPECT_FALSE(psv::from_json(doc, &error).has_value());
  EXPECT_NE(error.find("UTF-8"), std::string::npos);
}

TEST(PsvJson, AcceptsWellFormedUtf8AndEscapes) {
  auto v = psv::neutral(psv::Vertical::Aqademiq, 1);
  v.mode_hint = "caf\xC3\xA9 \xE2\x9C\x93 \xF0\x9F\x8E\xB5"; // é ✓ 🎵
  auto back = psv::from_json(psv::to_json(v));
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(*back, v);
  // Escaped form of a surrogate pair decodes to the same UTF-8.
  auto parsed = psv::from_json(R"({"schema_version":"1.0.0","vertical":"aqademiq",
    "update_timestamp":1,"mode_hint":"🎵",
    "state":{"arousal":{"value":0.5,"confidence":0},"valence":{"value":0.5,"confidence":0},
    "cognitive_load":{"value":0.5,"confidence":0},"readiness":{"value":0.5,"confidence":0}}})");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->mode_hint, "\xF0\x9F\x8E\xB5");
}

// --- Malformed JSON -----------------------------------------------------------------

TEST(PsvJson, RejectsMalformedDocuments) {
  const char* cases[] = {
      "",
      "   ",
      "[]",
      "\"just a string\"",
      "{",
      "{}",
      R"({"schema_version":})",
      R"({"a":1} trailing)",
      R"({"s":"unterminated)",
      R"({"s":"bad \q escape"})",
      R"({"n":NaN})",
      R"({"n":.5})",
      R"({"n":01})",
      R"({"a":1,})",
  };
  for (const char* doc : cases) {
    std::string error;
    EXPECT_FALSE(psv::from_json(doc, &error).has_value()) << "doc: " << doc;
    EXPECT_FALSE(error.empty()) << "doc: " << doc;
  }
}
