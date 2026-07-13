#include <gtest/gtest.h>

#include "iso8601.h"
#include "pce/attention.h"
#include "pce/circadian.h"
#include "pce/deadline.h"
#include "pce/fusion.h"
#include "pce/pce.h"

#include <cmath>

namespace pce = prism::pce;
namespace psv = prism::psv;

// Unit coverage documenting the ported heuristic's intent. The heavyweight correctness
// check is parity_test.cpp (golden-trace replay); these pin the behaviors the probe's own
// unit tests pinned.

// --- Deadline proximity -------------------------------------------------------------

TEST(PceDeadline, NoTasksIsNeutralAtZeroConfidence) {
  const auto r = pce::deadline_proximity({}, 1'000);
  EXPECT_DOUBLE_EQ(r.value, 0.5);
  EXPECT_DOUBLE_EQ(r.confidence, 0.0);
}

TEST(PceDeadline, PressureDecaysWithDistanceAndCapsByPriority) {
  const int64_t now = 1'750'000'000'000;
  const int64_t hour = 3'600'000;
  // 12h out, high priority: exp(-12/48) ≈ 0.7788
  auto r = pce::deadline_proximity({{now + 12 * hour, pce::Priority::High}}, now);
  EXPECT_NEAR(r.value, std::exp(-0.25), 1e-12);
  EXPECT_DOUBLE_EQ(r.confidence, 0.9);
  // Overdue clamps at the deadline: maximal pressure = priority weight.
  r = pce::deadline_proximity({{now - 5 * hour, pce::Priority::Low}}, now);
  EXPECT_DOUBLE_EQ(r.value, 0.4);
  // Most-pressing wins: a near low-priority task can lose to a farther high-priority one.
  r = pce::deadline_proximity(
      {{now + 2 * hour, pce::Priority::Low}, {now + 12 * hour, pce::Priority::High}}, now);
  EXPECT_NEAR(r.value, std::exp(-0.25), 1e-12); // the high task drives it
}

// --- Circadian ------------------------------------------------------------------------

TEST(PceCircadian, CurveShapeMatchesTheProbe) {
  EXPECT_LT(pce::circadian_alertness(4.0), 0.15);  // deep-night trough
  EXPECT_GT(pce::circadian_alertness(16.0), 0.85); // late-afternoon peak
  // Post-lunch dip: 14:00 sits below the plain cosine at the same hour.
  const double at14 = pce::circadian_alertness(14.0);
  EXPECT_LT(at14, 0.5 + 0.4 * std::cos(2.0 * 3.141592653589793 * (14.0 - 16.0) / 24.0) - 0.05);
  for (double h = 0.0; h < 24.0; h += 0.25) {
    const double a = pce::circadian_alertness(h);
    EXPECT_GE(a, 0.0);
    EXPECT_LE(a, 1.0);
  }
}

TEST(PceCircadian, EpochConversionUsesLocalMinuteQuantizedHours) {
  // 1970-01-01T00:30:45.500Z at UTC+4 (offset −240) → local 04:30 (seconds truncated).
  const auto r = pce::circadian_from_epoch(30 * 60'000 + 45'500, -240);
  EXPECT_DOUBLE_EQ(r.alertness, pce::circadian_alertness(4.0 + 30.0 / 60.0));
  EXPECT_DOUBLE_EQ(r.confidence, 0.5);
}

// --- Attention -------------------------------------------------------------------------

TEST(PceAttention, NoSamplesMeansNoSignal) {
  pce::AttentionMonitor m;
  const auto d = m.read_drivers(1'000'000);
  EXPECT_DOUBLE_EQ(d.activity, 0.5);
  EXPECT_DOUBLE_EQ(d.scatter, 0.0);
  EXPECT_DOUBLE_EQ(d.confidence, 0.0);
}

TEST(PceAttention, ScatterSaturatesAndIdleReadsAsAway) {
  pce::AttentionMonitor m;
  const int64_t t0 = 1'000'000'000;
  // Full 2-min window of idle samples every 5s; half of them away (> 15s idle).
  for (int i = 0; i < 24; ++i) {
    m.record_idle(t0 + i * 5'000, i % 2 == 0 ? 20'000 : 0);
  }
  // 12 switches in the 2-min window = 6/min → scatter = 1 − e^−1.
  for (int i = 0; i < 12; ++i) {
    m.record_switch(t0 + i * 9'000);
  }
  const auto d = m.read_drivers(t0 + 119'000);
  EXPECT_NEAR(d.scatter, 1.0 - std::exp(-1.0), 1e-12);
  EXPECT_DOUBLE_EQ(d.activity, 0.5);   // half the samples read as away
  EXPECT_DOUBLE_EQ(d.confidence, 0.7); // full coverage × base confidence
}

TEST(PceAttention, WindowEvictsOldEvents) {
  pce::AttentionMonitor m;
  const int64_t t0 = 1'000'000'000;
  m.record_idle(t0, 0);
  for (int i = 0; i < 50; ++i) {
    m.record_switch(t0);
  }
  // 121s later everything has aged out: back to no-signal.
  const auto d = m.read_drivers(t0 + 121'000);
  EXPECT_DOUBLE_EQ(d.confidence, 0.0);
  EXPECT_DOUBLE_EQ(d.scatter, 0.0);
}

// --- Fusion ------------------------------------------------------------------------------

TEST(PceFusion, AllInputsUnavailableYieldsNeutralInertVector) {
  const pce::FusionInputs unavailable{{0.5, 0.0}, {0.5, 0.0, 0.0}, {0.5, 0.0}};
  const auto v = pce::fuse_to_psv(unavailable, {1'000, 7, psv::Vertical::Aqademiq});
  for (const auto* d : {&v.arousal, &v.valence, &v.cognitive_load, &v.readiness}) {
    EXPECT_DOUBLE_EQ(d->value, 0.5);
    EXPECT_DOUBLE_EQ(d->confidence, 0.0);
  }
  EXPECT_EQ(v.sequence, 7);
  EXPECT_EQ(v.update_timestamp_ms, 1'000);
  EXPECT_FALSE(v.mode_hint.has_value());
}

TEST(PceFusion, FlowVersusOverwhelmAndReadinessErosion) {
  const pce::CircadianReading circadian{0.6, 0.5};
  const pce::AdapterReading pressure{0.9, 0.9};
  const pce::FusionInputs flow{pressure, {1.0, 0.05, 0.7}, circadian};
  const pce::FusionInputs overwhelm{pressure, {1.0, 0.9, 0.7}, circadian};
  const pce::FusionMeta meta{1'000, std::nullopt, psv::Vertical::Aqademiq};
  // Deep focus under pressure = flow (moderate load); scattered under pressure = overload.
  EXPECT_LT(pce::fuse_to_psv(flow, meta).cognitive_load.value,
            pce::fuse_to_psv(overwhelm, meta).cognitive_load.value);
  // Sustained pressure erodes readiness relative to the same circadian baseline.
  const pce::FusionInputs no_pressure{{0.5, 0.0}, {1.0, 0.05, 0.7}, circadian};
  EXPECT_LT(pce::fuse_to_psv(flow, meta).readiness.value,
            pce::fuse_to_psv(no_pressure, meta).readiness.value);
  // Valence ships inert in v1.
  EXPECT_DOUBLE_EQ(pce::fuse_to_psv(overwhelm, meta).valence.confidence, 0.0);
}

// --- Engine ---------------------------------------------------------------------------------

TEST(PceEngine, ColdStartEmitsNeutralThenCadenceAndChangeRulesApply) {
  pce::Pce engine;
  const int64_t t0 = 1'750'000'000'000;

  const auto neutral = engine.start(t0);
  EXPECT_EQ(neutral.sequence, 0);
  EXPECT_DOUBLE_EQ(neutral.arousal.value, 0.5);

  // First evaluate emits via the significant-change rule: the circadian prior pulls
  // arousal/readiness ≥ significant_delta away from the neutral start vector.
  auto e1 = engine.evaluate(t0 + 5'000);
  ASSERT_TRUE(e1.has_value());
  EXPECT_EQ(e1->sequence, 1);

  // Nothing changed, cadence not elapsed → silent tick; sequence does not advance.
  EXPECT_FALSE(engine.evaluate(t0 + 10'000).has_value());

  // A significant input change forces an early emit.
  engine.report_task_deadlines({{t0 + 3'600'000, pce::Priority::High}});
  auto e2 = engine.evaluate(t0 + 15'000);
  ASSERT_TRUE(e2.has_value());
  EXPECT_EQ(e2->sequence, 2);

  // Cadence alone forces an emit even with unchanged inputs.
  EXPECT_FALSE(engine.evaluate(t0 + 20'000).has_value());
  auto e3 = engine.evaluate(t0 + 45'100);
  ASSERT_TRUE(e3.has_value());
  EXPECT_EQ(e3->sequence, 3);
}

// --- Test-support ISO-8601 parser -------------------------------------------------------------

TEST(Iso8601, MatchesKnownInstants) {
  // Cross-checked with JS Date.parse.
  EXPECT_EQ(prism::test::parse_iso8601_ms("1970-01-01T00:00:00Z"), 0);
  EXPECT_EQ(prism::test::parse_iso8601_ms("2026-07-14T10:30:00.000Z"), 1'784'025'000'000);
  EXPECT_EQ(prism::test::parse_iso8601_ms("2026-07-14T18:00:00+04:00"), 1'784'037'600'000);
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-07-14").has_value());
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-07-14T18:00:00").has_value());
}

TEST(Iso8601, RangeRulesMatchDateParse) {
  // Out-of-range month/hour/minute → NaN in JS; must be nullopt here (a fabricated
  // instant would spuriously fail parity — found by adversarial review).
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-13-01T00:00:00Z").has_value());
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-07-14T25:00:00Z").has_value());
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-07-14T18:75:00Z").has_value());
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-07-14T18:00:75Z").has_value());
  EXPECT_FALSE(prism::test::parse_iso8601_ms("2026-07-14T24:30:00Z").has_value());
  // ...but JS ROLLS OVER an overflowing day-of-month, and so does days_from_civil.
  EXPECT_EQ(prism::test::parse_iso8601_ms("2026-06-31T00:00:00Z"),
            prism::test::parse_iso8601_ms("2026-07-01T00:00:00Z"));
  // Hour 24 is exactly-midnight-only.
  EXPECT_EQ(prism::test::parse_iso8601_ms("2026-07-14T24:00:00Z"),
            prism::test::parse_iso8601_ms("2026-07-15T00:00:00Z"));
}
