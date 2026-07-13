#include <gtest/gtest.h>

#include "psv/psv.h"
#include "psv/validate.h"

#include <cmath>
#include <limits>

namespace psv = prism::psv;

// --- Cold start (spec §6) ---------------------------------------------------

TEST(PsvColdStart, NeutralVectorMatchesSpec) {
  const auto v = psv::neutral(psv::Vertical::Aqademiq, 1750000000000);
  EXPECT_EQ(v.schema_version, psv::kSchemaVersion);
  EXPECT_EQ(v.vertical, psv::Vertical::Aqademiq);
  EXPECT_EQ(v.update_timestamp_ms, 1750000000000);
  EXPECT_FALSE(v.mode_hint.has_value());
  for (const auto* d : {&v.arousal, &v.valence, &v.cognitive_load, &v.readiness}) {
    EXPECT_DOUBLE_EQ(d->value, 0.5);
    EXPECT_DOUBLE_EQ(d->confidence, 0.0);
  }
  EXPECT_TRUE(psv::is_valid(v));
}

// --- Confidence weighting (spec §8.1) ----------------------------------------

TEST(PsvDimension, EffectiveBlendsTowardNeutralAsConfidenceFalls) {
  psv::Dimension d{0.9, 0.0};
  EXPECT_DOUBLE_EQ(d.effective(), 0.5); // inert at confidence 0
  d.confidence = 1.0;
  EXPECT_DOUBLE_EQ(d.effective(), 0.9); // full influence at confidence 1
  d.confidence = 0.5;
  EXPECT_DOUBLE_EQ(d.effective(), 0.7); // linear blend between
}

// --- Bounds (spec §3.1 / §5.2) -----------------------------------------------

TEST(PsvValidate, CatchesOutOfBoundsValues) {
  auto v = psv::neutral(psv::Vertical::Venues, 0);
  v.arousal.value = -0.001;
  v.valence.confidence = 1.001;
  const auto errors = psv::validate(v);
  ASSERT_EQ(errors.size(), 2u);
  EXPECT_NE(errors[0].find("arousal.value"), std::string::npos);
  EXPECT_NE(errors[1].find("valence.confidence"), std::string::npos);
}

TEST(PsvValidate, CatchesNonFiniteValues) {
  auto v = psv::neutral(psv::Vertical::Aqademiq, 0);
  v.cognitive_load.value = std::numeric_limits<double>::quiet_NaN();
  v.readiness.confidence = std::numeric_limits<double>::infinity();
  EXPECT_EQ(psv::validate(v).size(), 2u);
}

TEST(PsvValidate, CatchesBadMetadata) {
  auto v = psv::neutral(psv::Vertical::Aqademiq, 0);
  v.schema_version = "1.0";
  EXPECT_EQ(psv::validate(v).size(), 1u);
  v.schema_version = "1.0.0-beta";
  EXPECT_EQ(psv::validate(v).size(), 1u);
  v.schema_version = "1.0.0";
  v.update_timestamp_ms = -1;
  EXPECT_EQ(psv::validate(v).size(), 1u);
  v.update_timestamp_ms = 0;
  v.sequence = -5;
  EXPECT_EQ(psv::validate(v).size(), 1u);
  v.sequence = 0;
  EXPECT_TRUE(psv::is_valid(v));
}

TEST(PsvValidate, CatchesNonUtf8ModeHint) {
  auto v = psv::neutral(psv::Vertical::Aqademiq, 0);
  v.mode_hint = "\x80\xFF"; // raw invalid bytes: no RFC 8259 representation
  EXPECT_EQ(psv::validate(v).size(), 1u);
  v.mode_hint = "\xED\xA0\x80"; // encoded UTF-16 surrogate: also invalid
  EXPECT_EQ(psv::validate(v).size(), 1u);
  v.mode_hint = "caf\xC3\xA9"; // well-formed UTF-8 is fine
  EXPECT_TRUE(psv::is_valid(v));
}

TEST(PsvValidate, AcceptsBoundaryValues) {
  auto v = psv::neutral(psv::Vertical::Automotive, 0);
  v.arousal = {0.0, 1.0};
  v.readiness = {1.0, 0.0};
  EXPECT_TRUE(psv::is_valid(v));
}

// --- Vertical strings (spec §4.3) ---------------------------------------------

TEST(PsvVertical, StringsRoundTrip) {
  using psv::Vertical;
  for (auto vert : {Vertical::Aqademiq, Vertical::Venues, Vertical::Automotive}) {
    auto parsed = psv::vertical_from_string(psv::to_string(vert));
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(*parsed, vert);
  }
  EXPECT_FALSE(psv::vertical_from_string("desktop").has_value());
  EXPECT_FALSE(psv::vertical_from_string("").has_value());
  EXPECT_FALSE(psv::vertical_from_string("Aqademiq").has_value()); // case-sensitive
}

// --- Equality ------------------------------------------------------------------

TEST(PsvEquality, ExactFieldwiseComparison) {
  auto a = psv::neutral(psv::Vertical::Aqademiq, 42);
  auto b = a;
  EXPECT_EQ(a, b);
  b.arousal.value = std::nextafter(0.5, 1.0); // one ulp apart is not equal
  EXPECT_NE(a, b);
  b = a;
  b.mode_hint = psv::mode_hint::kDeepWork;
  EXPECT_NE(a, b);
  b = a;
  b.sequence = 0;
  EXPECT_NE(a, b);
}
