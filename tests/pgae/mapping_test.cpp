#include <gtest/gtest.h>

#include "pgae/fade.h"
#include "pgae/mapping.h"

#include <cmath>

namespace pgae = prism::pgae;
namespace psv = prism::psv;

namespace {

psv::StateVector make_psv(double a, double ac, double l, double lc, double r, double rc) {
  psv::StateVector v;
  v.update_timestamp_ms = 1;
  v.arousal = {a, ac};
  v.cognitive_load = {l, lc};
  v.readiness = {r, rc};
  return v;
}

} // namespace

// Values cross-checked by hand against the probe's audio/mapping.ts formulas — this
// mapping is the validated feel; parity with the probe is the bar.

TEST(PgaeMapping, NeutralPsvGivesBaselineParams) {
  const auto p = pgae::psv_to_audio_params(psv::neutral(psv::Vertical::Aqademiq, 1));
  EXPECT_DOUBLE_EQ(p.cutoff_hz, pgae::brightness_to_hz(0.55));
  EXPECT_DOUBLE_EQ(p.density, 0.5);
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Bed), 0.6);
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Sub), 0.5);
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Pulse), 0.5);
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Lead), 0.5);
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Air), 0.5);
  // Density 0.5: pulse (≥0.35) on, air (≥0.55) and lead (≥0.72) off, bed+sub always on.
  EXPECT_TRUE(active_of(p, pgae::StemRole::Bed));
  EXPECT_TRUE(active_of(p, pgae::StemRole::Sub));
  EXPECT_TRUE(active_of(p, pgae::StemRole::Pulse));
  EXPECT_FALSE(active_of(p, pgae::StemRole::Air));
  EXPECT_FALSE(active_of(p, pgae::StemRole::Lead));
}

TEST(PgaeMapping, ZeroConfidenceIsInert) {
  // Extreme values at confidence 0 behave exactly like the neutral vector (PGAE §3).
  const auto extreme = pgae::psv_to_audio_params(make_psv(1, 0, 1, 0, 0, 0));
  const auto neutral = pgae::psv_to_audio_params(psv::neutral(psv::Vertical::Aqademiq, 1));
  EXPECT_DOUBLE_EQ(extreme.cutoff_hz, neutral.cutoff_hz);
  EXPECT_DOUBLE_EQ(extreme.density, neutral.density);
  for (size_t i = 0; i < pgae::kStemRoleCount; ++i) {
    EXPECT_DOUBLE_EQ(extreme.gains[i], neutral.gains[i]);
  }
}

TEST(PgaeMapping, HighLoadRecedesToTheCore) {
  // Full-confidence saturated load: l = +0.5 → density 0 (only bed+sub), bed rises to
  // 0.9, lead recedes to 0.05 — the focus-protection case (PGAE §5).
  const auto p = pgae::psv_to_audio_params(make_psv(0.5, 0, 1, 1, 0.5, 0));
  EXPECT_DOUBLE_EQ(p.density, 0.0);
  EXPECT_TRUE(active_of(p, pgae::StemRole::Bed));
  EXPECT_TRUE(active_of(p, pgae::StemRole::Sub));
  EXPECT_FALSE(active_of(p, pgae::StemRole::Pulse));
  EXPECT_FALSE(active_of(p, pgae::StemRole::Air));
  EXPECT_FALSE(active_of(p, pgae::StemRole::Lead));
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Bed), 0.9);
  EXPECT_NEAR(gain_of(p, pgae::StemRole::Lead), 0.05, 1e-12);
  // Brightness closes: 0.55 − 0.8·0.5 = 0.15.
  EXPECT_DOUBLE_EQ(p.cutoff_hz, pgae::brightness_to_hz(0.15));
}

TEST(PgaeMapping, FatigueRaisesTheGroundingSub) {
  // readiness 0 at full confidence: r = −0.5 → sub = 0.5 + 0.3 = 0.8 (support the fatigued).
  const auto p = pgae::psv_to_audio_params(make_psv(0.5, 0, 0.5, 0, 0.0, 1));
  EXPECT_DOUBLE_EQ(gain_of(p, pgae::StemRole::Sub), 0.8);
}

TEST(PgaeMapping, BrightnessIsLogScaled) {
  EXPECT_DOUBLE_EQ(pgae::brightness_to_hz(0.0), 300.0);
  EXPECT_DOUBLE_EQ(pgae::brightness_to_hz(1.0), 12'000.0);
  EXPECT_NEAR(pgae::brightness_to_hz(0.5), 300.0 * std::sqrt(40.0), 1e-9);
  EXPECT_DOUBLE_EQ(pgae::brightness_to_hz(-1.0), 300.0);   // clamped
  EXPECT_DOUBLE_EQ(pgae::brightness_to_hz(2.0), 12'000.0); // clamped
}

// --- Fade shape and loop scheduling (pure helpers used by the render path) -------------

TEST(PgaeFade, EqualPowerPropertyHolds) {
  for (double t = 0.0; t <= 1.0; t += 1.0 / 257.0) {
    const double in = pgae::equal_power_fade(t, true);
    const double out = pgae::equal_power_fade(t, false);
    EXPECT_NEAR(in * in + out * out, 1.0, 1e-12);
  }
  EXPECT_DOUBLE_EQ(pgae::equal_power_fade(0.0, true), 0.0);
  EXPECT_DOUBLE_EQ(pgae::equal_power_fade(1.0, true), 1.0);
  EXPECT_DOUBLE_EQ(pgae::equal_power_fade(0.0, false), 1.0);
  EXPECT_NEAR(pgae::equal_power_fade(1.0, false), 0.0, 1e-15);
}

TEST(PgaeFade, NextLoopBoundaryIsSampleAccurate) {
  EXPECT_EQ(pgae::next_loop_boundary(0, 100), 100u);
  EXPECT_EQ(pgae::next_loop_boundary(99, 100), 100u);
  EXPECT_EQ(pgae::next_loop_boundary(100, 100), 200u); // exactly at a boundary → the next one
  EXPECT_EQ(pgae::next_loop_boundary(250, 100), 300u);
}
