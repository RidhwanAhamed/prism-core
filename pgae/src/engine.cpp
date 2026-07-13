#include "pgae/engine.h"

#include "pgae/fade.h"

#include <cmath>

namespace prism::pgae {

namespace {
// bed + sub are always audible (mapping.cpp keeps their density gate true); they start
// open, optional layers start closed — exactly the probe's init.
constexpr bool kInitiallyActive[kStemRoleCount] = {true, true, false, false, false};
constexpr uint32_t kCoeffUpdateInterval = 32; // biquad refresh cadence, samples
} // namespace

Pgae::Pgae(PgaeOptions options) : opts_(options) {}

double Pgae::gain_to_amp(double x) const {
  // Perceptual [0,1] loudness → amplitude, with headroom trim (verbatim: 0.4 · x^1.5).
  return opts_.amp_trim * std::pow(x, 1.5);
}

bool Pgae::load_scene(SceneAssets assets, std::string* error) {
  if (assets.sample_rate == 0) {
    if (error) {
      *error = "scene assets carry no sample rate";
    }
    return false;
  }
  sample_rate_ = assets.sample_rate;
  assets_ = std::move(assets);
  sample_clock_ = 0;

  const double fs = static_cast<double>(sample_rate_);
  level_coeff_ = 1.0 - std::exp(-1.0 / (opts_.tc_gain_s * fs));
  cutoff_coeff_ = 1.0 - std::exp(-1.0 / (opts_.tc_cutoff_s * fs));
  cutoff_value_ = opts_.initial_cutoff_hz;
  cutoff_target_ = opts_.initial_cutoff_hz;
  lowpass_ = detail::BiquadLowpass{};
  lowpass_.set(cutoff_value_, opts_.lowpass_q, fs);
  limiter_ = detail::PeakLimiter{};
  limiter_.configure(std::pow(10.0, opts_.limiter_ceiling_db / 20.0), opts_.limiter_attack_s,
                     opts_.limiter_release_s, fs);

  for (size_t i = 0; i < kStemRoleCount; ++i) {
    StemRender& stem = stems_[i];
    stem = StemRender{};
    const std::vector<float>& frames = assets_.stems[i];
    stem.present = !frames.empty();
    stem.loop_frames = frames.size();
    stem.active = kInitiallyActive[i];
    stem.env = stem.active ? 1.0 : 0.0;
    stem.ramp_from = -1.0; // sentinel: captured when a scheduled ramp actually begins
    stem.level_value = gain_to_amp(0.5);
    stem.level_target = stem.level_value;
  }
  return true;
}

AudioParams Pgae::consume_psv(const psv::StateVector& v) {
  const AudioParams params = psv_to_audio_params(v);

  cutoff_target_ = params.cutoff_hz;
  for (size_t i = 0; i < kStemRoleCount; ++i) {
    StemRender& stem = stems_[i];
    if (!stem.present) {
      continue;
    }
    stem.level_target = gain_to_amp(params.gains[i]);
    if (params.active[i] != stem.active) {
      stem.active = params.active[i];
      schedule_crossfade(stem, params.active[i]);
    }
  }
  return params;
}

void Pgae::schedule_crossfade(StemRender& stem, bool activate) {
  // At the stem's next loop boundary (PGAE §6.2) — the coprime loop architecture is what
  // hides the seams. A newly scheduled fade replaces any pending one (the probe cancels
  // scheduled values the same way).
  stem.ramping = true;
  stem.ramp_to_active = activate;
  stem.ramp_start = next_loop_boundary(sample_clock_, stem.loop_frames);
  const uint64_t frames =
      static_cast<uint64_t>(opts_.crossfade_s * static_cast<double>(sample_rate_));
  stem.ramp_frames = frames > 0 ? frames : 1;
  stem.ramp_from = -1.0; // capture the env at ramp start, not at schedule time
}

void Pgae::render(float* out, uint32_t frame_count) {
  const double fs = static_cast<double>(sample_rate_);
  const uint64_t master_fade_frames = static_cast<uint64_t>(opts_.master_fade_s * fs) > 0
                                          ? static_cast<uint64_t>(opts_.master_fade_s * fs)
                                          : 1;

  for (uint32_t f = 0; f < frame_count; ++f) {
    // Smoothed cutoff: the parameter moves every sample; the biquad refreshes per block.
    cutoff_value_ += (cutoff_target_ - cutoff_value_) * cutoff_coeff_;
    if (sample_clock_ % kCoeffUpdateInterval == 0) {
      lowpass_.set(cutoff_value_, opts_.lowpass_q, fs);
    }

    double mix = 0.0;
    for (size_t i = 0; i < kStemRoleCount; ++i) {
      StemRender& stem = stems_[i];
      if (!stem.present) {
        continue;
      }

      // Density envelope: equal-power ramp between 0 and 1 from the stem's loop boundary.
      // Deliberate divergence from the probe (documented in engine.h): the ramp leaves
      // from the env's CURRENT value, so a mid-fade retarget can never step.
      if (stem.ramping && sample_clock_ >= stem.ramp_start) {
        if (stem.ramp_from < 0.0) {
          stem.ramp_from = stem.env;
        }
        const double target = stem.ramp_to_active ? 1.0 : 0.0;
        const double t = static_cast<double>(sample_clock_ - stem.ramp_start) /
                         static_cast<double>(stem.ramp_frames);
        if (t >= 1.0) {
          stem.env = target;
          stem.ramping = false;
          stem.ramp_from = -1.0;
        } else {
          // Probe curves at the endpoints: sin(tπ/2) in, cos(tπ/2) out.
          const double progress =
              stem.ramp_to_active ? equal_power_fade(t, true) : 1.0 - equal_power_fade(t, false);
          stem.env = stem.ramp_from + (target - stem.ramp_from) * progress;
        }
      }

      stem.level_value += (stem.level_target - stem.level_value) * level_coeff_;

      const float sample = assets_.stems[i][static_cast<size_t>(sample_clock_ % stem.loop_frames)];
      mix += static_cast<double>(sample) * stem.env * stem.level_value;
    }

    const double filtered = lowpass_.process(mix);

    // Click-free start (PGAE §8): linear master ramp from silence, then steady.
    const double master = sample_clock_ < master_fade_frames
                              ? opts_.master_gain * (static_cast<double>(sample_clock_) /
                                                     static_cast<double>(master_fade_frames))
                              : opts_.master_gain;

    out[f] = static_cast<float>(limiter_.process(filtered * master));
    ++sample_clock_;
  }
}

} // namespace prism::pgae
