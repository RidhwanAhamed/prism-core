#include <gtest/gtest.h>

#include "pgae/detail/dsp.h"
#include "pgae/engine.h"
#include "pgae/scene.h"

#include <cmath>
#include <fstream>
#include <sstream>
#include <vector>

namespace pgae = prism::pgae;
namespace psv = prism::psv;

// Task 3 acceptance ("offline render tests pass"): limiter ceiling held, no parameter
// steps, equal-power crossfades, sample-accurate scheduling. Rendered fully offline —
// no audio device.

namespace {

constexpr uint32_t kRate = 44'100;
constexpr double kCeiling = 0.7079457843841379; // 10^(-3/20)

// Synthetic DC stems make gain paths directly observable at the output.
pgae::SceneAssets dc_assets(float value) {
  pgae::SceneAssets assets;
  assets.sample_rate = kRate;
  const size_t loops[pgae::kStemRoleCount] = {8'000, 6'000, 4'000, 5'000, 7'000};
  for (size_t i = 0; i < pgae::kStemRoleCount; ++i) {
    assets.stems[i].assign(loops[i], value);
  }
  return assets;
}

psv::StateVector make_psv(double a, double ac, double l, double lc, double r, double rc) {
  psv::StateVector v;
  v.update_timestamp_ms = 1;
  v.arousal = {a, ac};
  v.cognitive_load = {l, lc};
  v.readiness = {r, rc};
  return v;
}

std::vector<float> render_seconds(pgae::Pgae& engine, double seconds) {
  std::vector<float> out(static_cast<size_t>(seconds * kRate));
  for (size_t at = 0; at < out.size(); at += 512) {
    const uint32_t n = static_cast<uint32_t>(std::min<size_t>(512, out.size() - at));
    engine.render(out.data() + at, n);
  }
  return out;
}

} // namespace

TEST(PgaeRender, StartsFromSilence) {
  pgae::Pgae engine;
  ASSERT_TRUE(engine.load_scene(dc_assets(1.0F)));
  const auto out = render_seconds(engine, 0.01);
  EXPECT_NEAR(out[0], 0.0, 1e-4); // click-free master fade-in from zero
}

TEST(PgaeRender, LimiterCeilingIsNeverExceeded) {
  pgae::Pgae engine;
  ASSERT_TRUE(engine.load_scene(dc_assets(1.0F)));
  // Hostile drive: maximum arousal, zero load, exhausted readiness → loud, dense mix.
  engine.consume_psv(make_psv(1, 1, 0, 1, 0, 1));
  const auto out = render_seconds(engine, 4.0);
  double peak = 0.0;
  for (float s : out) {
    peak = std::max(peak, static_cast<double>(std::fabs(s)));
  }
  EXPECT_LE(peak, kCeiling + 1e-9);
  EXPECT_GT(peak, 0.5); // and the limiter is actually working, not just silence
}

TEST(PgaeRender, NoParameterSteps) {
  pgae::Pgae engine;
  ASSERT_TRUE(engine.load_scene(dc_assets(0.3F)));

  std::vector<float> all;
  auto run = [&](double seconds) {
    const auto part = render_seconds(engine, seconds);
    all.insert(all.end(), part.begin(), part.end());
  };

  run(1.0);                                             // settle after master fade-in
  engine.consume_psv(make_psv(0.9, 1, 0.1, 1, 0.5, 0)); // big retarget + density fades in
  run(3.0);
  engine.consume_psv(make_psv(0.1, 1, 1.0, 1, 0.2, 1)); // slam to overload: fades back out
  run(3.0);

  // DC stems: every output change is a parameter change. Smoothed one-pole levels, 1.5s
  // equal-power fades, 0.5s master ramp and an idle limiter bound the legitimate
  // per-sample delta well under 1e-3; any step (e.g. an env jump) is orders larger.
  double max_delta = 0.0;
  for (size_t i = 1; i < all.size(); ++i) {
    max_delta = std::max(max_delta, static_cast<double>(std::fabs(all[i] - all[i - 1])));
  }
  EXPECT_LT(max_delta, 1e-3);
}

TEST(PgaeRender, DensityFadeArrivesAndRaisesTheMix) {
  pgae::Pgae engine;
  ASSERT_TRUE(engine.load_scene(dc_assets(0.2F)));
  engine.consume_psv(psv::neutral(psv::Vertical::Aqademiq, 1)); // density 0.5: pulse only
  const auto before = render_seconds(engine, 2.0);
  // Full arousal opens air + lead; their fades start at loop boundaries and take 1.5s.
  engine.consume_psv(make_psv(1, 1, 0.5, 0, 0.5, 0));
  render_seconds(engine, 3.0); // let fades schedule, start, and complete
  const auto after = render_seconds(engine, 1.0);
  EXPECT_GT(std::fabs(after.back()), std::fabs(before.back()) + 0.01);
}

TEST(PgaeLimiter, HardClampBacksUpTheEnvelopeFollower) {
  pgae::detail::PeakLimiter limiter;
  limiter.configure(kCeiling, 0.003, 0.1, kRate);
  // Full-scale sine burst: nothing may cross the rail, not even the first samples where
  // the envelope follower is still attacking.
  for (int i = 0; i < 44'100; ++i) {
    const double x = 0.99 * std::sin(2.0 * 3.141592653589793 * 220.0 * i / kRate);
    const double y = limiter.process(x);
    ASSERT_LE(std::fabs(y), kCeiling + 1e-12);
  }
}

TEST(PgaeScene, RepoManifestParsesAndValidates) {
  std::ifstream in(std::string(PRISM_ASSETS_DIR) + "/scenes.json");
  ASSERT_TRUE(in.good());
  std::stringstream buffer;
  buffer << in.rdbuf();
  std::string error;
  const auto manifest = pgae::parse_scene_manifest(buffer.str(), &error);
  ASSERT_TRUE(manifest.has_value()) << error;
  EXPECT_EQ(manifest->default_scene, "deep_work");
  const auto* scene = manifest->find("deep_work");
  ASSERT_TRUE(scene != nullptr);
  EXPECT_EQ(scene->stems.size(), pgae::kStemRoleCount);
  EXPECT_EQ(scene->key, "C Dorian");
}

TEST(PgaeScene, RejectsBrokenManifests) {
  std::string error;
  EXPECT_FALSE(pgae::parse_scene_manifest("{}", &error).has_value());
  EXPECT_FALSE(pgae::parse_scene_manifest(
                   R"({"schema_version":"1.0.0","default_scene":"x","scenes":[
                       {"id":"x","stems":[{"role":"vocals","file":"v.wav"}]}]})",
                   &error)
                   .has_value());
  EXPECT_NE(error.find("unknown stem role"), std::string::npos);
  EXPECT_FALSE(pgae::parse_scene_manifest(
                   R"({"schema_version":"1.0.0","default_scene":"x","scenes":[
                       {"id":"x","stems":[{"role":"bed","file":"a.wav"},
                                          {"role":"bed","file":"b.wav"}]}]})",
                   &error)
                   .has_value());
  EXPECT_NE(error.find("duplicate stem role"), std::string::npos);
  EXPECT_FALSE(pgae::parse_scene_manifest(
                   R"({"schema_version":"1.0.0","default_scene":"missing","scenes":[
                       {"id":"x","stems":[{"role":"bed","file":"a.wav"}]}]})",
                   &error)
                   .has_value());
  EXPECT_NE(error.find("does not exist"), std::string::npos);
}

TEST(PgaeScene, LoadsTheRealStems) {
  std::ifstream in(std::string(PRISM_ASSETS_DIR) + "/scenes.json");
  std::stringstream buffer;
  buffer << in.rdbuf();
  const auto manifest = pgae::parse_scene_manifest(buffer.str(), nullptr);
  ASSERT_TRUE(manifest.has_value());
  std::string error;
  const auto assets = pgae::load_scene_assets(*manifest, "deep_work", PRISM_ASSETS_DIR, &error);
  ASSERT_TRUE(assets.has_value()) << error;
  EXPECT_EQ(assets->sample_rate, 44'100u);
  for (size_t i = 0; i < pgae::kStemRoleCount; ++i) {
    EXPECT_FALSE(assets->stems[i].empty()) << pgae::kStemRoleNames[i];
  }
}
