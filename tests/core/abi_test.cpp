#include <gtest/gtest.h>

#include "prism/prism_core.h"

#include <chrono>
#include <cmath>
#include <string>
#include <thread>
#include <vector>

// Task 5 acceptance support: the full ABI lifecycle exercised end to end, offline (no
// audio device — the pull model is the CI-testable path; prism_device_* needs hardware
// and is covered by the harness).

namespace {

const char* scenes_path() {
  static const std::string path = std::string(PRISM_ASSETS_DIR) + "/scenes.json";
  return path.c_str();
}

// Poll prism_get_psv until `pred` holds or the deadline passes.
template <typename Pred> bool wait_for_psv(prism_core* core, Pred pred, int timeout_ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    prism_psv psv;
    if (prism_get_psv(core, &psv) == PRISM_OK && pred(psv)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

TEST(PrismAbi, LifecycleOrderIsEnforced) {
  prism_core* core = nullptr;
  ASSERT_EQ(prism_create(nullptr, &core), PRISM_OK);

  EXPECT_EQ(prism_start(core), PRISM_ERROR_INVALID_STATE); // scene first
  EXPECT_EQ(prism_load_scene(core, "no/such/manifest.json"), PRISM_ERROR_IO);
  EXPECT_EQ(prism_load_scene(core, scenes_path()), PRISM_OK);
  EXPECT_EQ(prism_load_scene(core, scenes_path()), PRISM_ERROR_INVALID_STATE); // once per handle
  EXPECT_GT(prism_sample_rate(core), 0u);

  EXPECT_EQ(prism_start(core), PRISM_OK);
  EXPECT_EQ(prism_start(core), PRISM_ERROR_INVALID_STATE); // already running
  EXPECT_EQ(prism_stop(core), PRISM_OK);
  EXPECT_EQ(prism_stop(core), PRISM_OK); // idempotent

  prism_destroy(core);
}

TEST(PrismAbi, ConfigIsValidated) {
  prism_core* core = nullptr;
  prism_config config = prism_config_default();
  config.cadence_ms = -1;
  EXPECT_EQ(prism_create(&config, &core), PRISM_ERROR_INVALID_ARGUMENT);
  config = prism_config_default();
  config.vertical = 99;
  EXPECT_EQ(prism_create(&config, &core), PRISM_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(prism_create(nullptr, nullptr), PRISM_ERROR_INVALID_ARGUMENT);
}

TEST(PrismAbi, ColdStartThenIngressDrivesEmissions) {
  prism_config config = prism_config_default();
  config.check_interval_ms = 25; // fast ticks so the test stays quick
  config.cadence_ms = 100;

  prism_core* core = nullptr;
  ASSERT_EQ(prism_create(&config, &core), PRISM_OK);
  ASSERT_EQ(prism_load_scene(core, scenes_path()), PRISM_OK);
  ASSERT_EQ(prism_start(core), PRISM_OK);

  // The cold-start neutral vector (spec §6) is readable immediately.
  prism_psv psv;
  ASSERT_EQ(prism_get_psv(core, &psv), PRISM_OK);
  EXPECT_EQ(psv.sequence, 0);
  EXPECT_DOUBLE_EQ(psv.arousal, 0.5);
  EXPECT_DOUBLE_EQ(psv.arousal_confidence, 0.0);
  EXPECT_STREQ(psv.mode_hint, "");

  // Feed deadline pressure + behavior; the internal inference loop must emit fused,
  // confident vectors within a few fast ticks.
  const prism_task_deadline deadline = {now_ms() + 3'600'000, 2};
  EXPECT_EQ(prism_report_task_deadlines(core, &deadline, 1), PRISM_OK);
  const int64_t t = now_ms();
  for (int i = 0; i < 24; ++i) {
    EXPECT_EQ(prism_report_idle(core, t - (23 - i) * 5'000, 0), PRISM_OK);
  }
  EXPECT_EQ(prism_report_app_switch(core, t), PRISM_OK);

  EXPECT_TRUE(wait_for_psv(
      core, [](const prism_psv& p) { return p.sequence >= 1 && p.arousal_confidence > 0.3; },
      3'000))
      << "inference thread never emitted a fused vector";

  prism_destroy(core);
}

TEST(PrismAbi, RestartContinuesTheSequence) {
  // stop→start is legal; the restart's neutral vector must CONTINUE the sequence, not
  // reuse the previous run's last number with different content (adversarial review:
  // sequence-deduping consumers silently missed the snap back to neutral).
  prism_config config = prism_config_default();
  config.check_interval_ms = 25;
  config.cadence_ms = 100;

  prism_core* core = nullptr;
  ASSERT_EQ(prism_create(&config, &core), PRISM_OK);
  ASSERT_EQ(prism_load_scene(core, scenes_path()), PRISM_OK);
  ASSERT_EQ(prism_start(core), PRISM_OK);

  const prism_task_deadline deadline = {now_ms() + 3'600'000, 2};
  ASSERT_EQ(prism_report_task_deadlines(core, &deadline, 1), PRISM_OK);
  ASSERT_TRUE(wait_for_psv(core, [](const prism_psv& p) { return p.sequence >= 1; }, 3'000));

  prism_psv before_stop;
  ASSERT_EQ(prism_get_psv(core, &before_stop), PRISM_OK);
  ASSERT_EQ(prism_stop(core), PRISM_OK);

  ASSERT_EQ(prism_start(core), PRISM_OK);
  prism_psv after_restart;
  ASSERT_EQ(prism_get_psv(core, &after_restart), PRISM_OK);
  EXPECT_GT(after_restart.sequence, before_stop.sequence) << "restart reused a sequence number";
  EXPECT_DOUBLE_EQ(after_restart.arousal, 0.5); // neutral again
  EXPECT_DOUBLE_EQ(after_restart.arousal_confidence, 0.0);

  // Emissions after the restart keep the sequence strictly monotonic.
  const int64_t restart_seq = after_restart.sequence;
  EXPECT_TRUE(wait_for_psv(
      core, [restart_seq](const prism_psv& p) { return p.sequence > restart_seq; }, 3'000));

  prism_destroy(core);
}

TEST(PrismAbi, PullModelRenderProducesBoundedAudio) {
  prism_config config = prism_config_default();
  config.check_interval_ms = 25;
  config.cadence_ms = 100;

  prism_core* core = nullptr;
  ASSERT_EQ(prism_create(&config, &core), PRISM_OK);
  ASSERT_EQ(prism_load_scene(core, scenes_path()), PRISM_OK);
  ASSERT_EQ(prism_start(core), PRISM_OK);

  EXPECT_EQ(prism_render(core, nullptr, 64), PRISM_ERROR_INVALID_ARGUMENT);

  const uint32_t rate = prism_sample_rate(core);
  std::vector<float> block(512);
  double peak = 0.0;
  // Render ~4s: through the master fade-in and into steady playback.
  for (uint32_t rendered = 0; rendered < rate * 4; rendered += 512) {
    ASSERT_EQ(prism_render(core, block.data(), 512), PRISM_OK);
    for (float s : block) {
      ASSERT_TRUE(std::isfinite(s));
      peak = std::max(peak, static_cast<double>(std::fabs(s)));
    }
  }
  EXPECT_GT(peak, 0.01);                               // audible output
  EXPECT_LE(peak, std::pow(10.0, -3.0 / 20.0) + 1e-9); // the limiter rail holds via the ABI too

  prism_destroy(core);
}

TEST(PrismAbi, MoodOverrideIsValidated) {
  prism_core* core = nullptr;
  ASSERT_EQ(prism_create(nullptr, &core), PRISM_OK);

  prism_mood_override o{};
  o.mode_hint = "venue_peak";
  o.arousal = 0.9;
  o.valence = 0.7;
  o.cognitive_load = 0.3;
  o.readiness = 0.6;
  o.confidence = 1.0;

  EXPECT_EQ(prism_set_mood_override(nullptr, &o), PRISM_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(prism_set_mood_override(core, nullptr), PRISM_ERROR_INVALID_ARGUMENT);

  // Every dimension is bounded [0,1] (spec §4.1); so is confidence.
  for (double* field : {&o.arousal, &o.valence, &o.cognitive_load, &o.readiness, &o.confidence}) {
    const double good = *field;
    *field = 1.5;
    EXPECT_EQ(prism_set_mood_override(core, &o), PRISM_ERROR_INVALID_ARGUMENT);
    *field = -0.1;
    EXPECT_EQ(prism_set_mood_override(core, &o), PRISM_ERROR_INVALID_ARGUMENT);
    *field = good;
  }

  // A hint too long to survive the fixed-width RT snapshot is rejected, not truncated:
  // a clipped hint would silently name a different mood downstream.
  o.mode_hint = "venue_a_mood_name_far_too_long_to_fit";
  EXPECT_EQ(prism_set_mood_override(core, &o), PRISM_ERROR_INVALID_ARGUMENT);

  o.mode_hint = nullptr; // no hint is legal
  EXPECT_EQ(prism_set_mood_override(core, &o), PRISM_OK);

  EXPECT_EQ(prism_clear_mood_override(nullptr), PRISM_ERROR_INVALID_ARGUMENT);
  EXPECT_EQ(prism_clear_mood_override(core), PRISM_OK);
  EXPECT_EQ(prism_clear_mood_override(core), PRISM_OK); // idempotent

  prism_destroy(core);
}

TEST(PrismAbi, MoodOverridePinsThePsvAndSurvivesInferenceTicks) {
  prism_config config = prism_config_default();
  config.vertical = PRISM_VERTICAL_VENUES;
  config.check_interval_ms = 25;
  config.cadence_ms = 50; // tick fast, so an un-pinned vector would overwrite quickly

  prism_core* core = nullptr;
  ASSERT_EQ(prism_create(&config, &core), PRISM_OK);
  ASSERT_EQ(prism_load_scene(core, scenes_path()), PRISM_OK);
  ASSERT_EQ(prism_start(core), PRISM_OK);

  prism_psv cold{};
  ASSERT_EQ(prism_get_psv(core, &cold), PRISM_OK);
  EXPECT_DOUBLE_EQ(cold.arousal, 0.5); // cold start is neutral (spec §6)

  prism_mood_override peak{};
  peak.mode_hint = "venue_peak";
  peak.arousal = 0.9;
  peak.valence = 0.75;
  peak.cognitive_load = 0.2;
  peak.readiness = 0.6;
  peak.confidence = 1.0;
  ASSERT_EQ(prism_set_mood_override(core, &peak), PRISM_OK);

  // Published synchronously: the mood is readable the instant the call returns, without
  // waiting out the inference cadence. A tap has to be heard now, not in five seconds.
  prism_psv pinned{};
  ASSERT_EQ(prism_get_psv(core, &pinned), PRISM_OK);
  EXPECT_DOUBLE_EQ(pinned.arousal, 0.9);
  EXPECT_DOUBLE_EQ(pinned.arousal_confidence, 1.0);
  EXPECT_STREQ(pinned.mode_hint, "venue_peak");
  EXPECT_GT(pinned.sequence, cold.sequence); // the counter still only moves forward

  // Several inference ticks pass; the PCE keeps evaluating and its output keeps being
  // discarded while the pin holds.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  prism_psv still{};
  ASSERT_EQ(prism_get_psv(core, &still), PRISM_OK);
  EXPECT_DOUBLE_EQ(still.arousal, 0.9);
  EXPECT_STREQ(still.mode_hint, "venue_peak");

  // Distinct moods must reach the vector distinctly — this is what makes six moods sound
  // like six moods rather than six labels on the same sound.
  prism_mood_override wind_down{};
  wind_down.mode_hint = "venue_wind_down";
  wind_down.arousal = 0.12;
  wind_down.valence = 0.45;
  wind_down.cognitive_load = 0.15;
  wind_down.readiness = 0.3;
  wind_down.confidence = 1.0;
  ASSERT_EQ(prism_set_mood_override(core, &wind_down), PRISM_OK);

  prism_psv quiet{};
  ASSERT_EQ(prism_get_psv(core, &quiet), PRISM_OK);
  EXPECT_DOUBLE_EQ(quiet.arousal, 0.12);
  EXPECT_STREQ(quiet.mode_hint, "venue_wind_down");
  EXPECT_GT(quiet.sequence, pinned.sequence);

  // Clearing hands control back: the PCE's next emission replaces the pinned vector.
  ASSERT_EQ(prism_clear_mood_override(core), PRISM_OK);
  EXPECT_TRUE(wait_for_psv(
      core, [](const prism_psv& p) { return p.arousal != 0.12 || p.mode_hint[0] == '\0'; }, 2000))
      << "the PCE never regained control after the override was cleared";

  prism_destroy(core);
}
