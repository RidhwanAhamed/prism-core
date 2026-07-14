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
