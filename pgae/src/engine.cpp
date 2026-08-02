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

bool Pgae::load_scene(const SceneAssets* assets, std::string* error) {
  if (assets == nullptr || assets->sample_rate == 0) {
    if (error) {
      *error = assets == nullptr ? "scene assets are null" : "scene assets carry no sample rate";
    }
    return false;
  }
  sample_rate_ = assets->sample_rate;
  Deck& deck = decks_[0];
  deck.assets = assets;
  deck.phase = 0;
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

  init_deck(deck, assets);
  return true;
}

void Pgae::init_deck(Deck& deck, const SceneAssets* assets) const {
  // Buffers and the state derived from them, set together — see the Deck comment for why
  // they must never be published apart. Allocation-free by construction: StemRender is
  // trivial and the buffers are already decoded, which is what lets the render path run
  // this when it accepts a swap.
  deck.assets = assets;
  deck.phase = 0;
  for (size_t i = 0; i < kStemRoleCount; ++i) {
    StemRender& stem = deck.stems[i];
    stem = StemRender{};
    const std::vector<float>& frames = assets->stems[i];
    stem.present = !frames.empty();
    stem.loop_frames = frames.size();
    stem.active = kInitiallyActive[i];
    stem.env = stem.active ? 1.0 : 0.0;
    stem.ramp_from = -1.0; // sentinel: captured when a scheduled ramp actually begins
    stem.level_value = gain_to_amp(0.5);
    stem.level_target = stem.level_value;
  }
}

bool Pgae::begin_crossfade(const SceneAssets* assets, uint64_t crossfade_frames, bool align) {
  if (assets == nullptr || assets->sample_rate != sample_rate_) {
    return false;
  }
  if (crossfade_active()) {
    return false; // caller keeps ownership and retries
  }
  SceneSwap swap;
  swap.assets = assets;
  swap.crossfade_frames = crossfade_frames > 0 ? crossfade_frames : 1;
  swap.align_to_loop_boundary = align;
  return swap_.post(swap);
}

const SceneAssets* Pgae::abandon_pending_crossfade() {
  SceneSwap swap;
  const SceneAssets* stranded = swap_.take(swap) ? swap.assets : nullptr;
  swap_.abandon();
  return stranded;
}

void Pgae::service_swap() {
  // Render path. Accept at most one command, then arm it; the overlap itself starts later,
  // at fade_start_.
  SceneSwap swap;
  if (!swap_.take(swap)) {
    return;
  }
  incoming_ = 1 - live_;
  init_deck(decks_[incoming_], swap.assets);
  fade_frames_ = swap.crossfade_frames;

  // Aligning starts the overlap where the outgoing scene's bed completes a loop, so the
  // material leaving is never cut mid-phrase. Without a bed to key off — a scene can be
  // built from any subset of roles — fall through to the next sample rather than guess.
  const StemRender& bed = decks_[live_].stems[static_cast<size_t>(StemRole::Bed)];
  fade_start_ = (swap.align_to_loop_boundary && bed.present && bed.loop_frames > 0)
                    ? next_loop_boundary(decks_[live_].phase, bed.loop_frames)
                    : decks_[live_].phase;
  armed_ = true;
  busy_.store(true, std::memory_order_release);
}

void Pgae::finish_crossfade() {
  // Render path. The outgoing scene is published for the control thread to release; the
  // render path merely stops pointing at it. Releasing here would be a real-time rule 1
  // violation, which is the whole reason RetiredScenes exists.
  Deck& outgoing = decks_[live_];
  retired_.retire(outgoing.assets);
  outgoing.assets = nullptr;
  for (size_t i = 0; i < kStemRoleCount; ++i) {
    outgoing.stems[i].present = false; // cannot be rendered again before a fresh init_deck
  }
  live_ = incoming_;
  fading_ = false;
  armed_ = false;
  busy_.store(false, std::memory_order_release);
}

AudioParams Pgae::consume_psv(const psv::StateVector& v) {
  const AudioParams params = psv_to_audio_params(v);
  apply_params(params);
  return params;
}

AudioParams Pgae::consume_psv(const psv::RtStateVector& v) {
  const AudioParams params = psv_to_audio_params(v);
  apply_params(params);
  return params;
}

void Pgae::apply_params(const AudioParams& params) {
  cutoff_target_ = params.cutoff_hz;
  // Both decks during an overlap. The mapping is keyed by StemRole, not by scene, so the
  // same params are the right answer for either set of buffers — and a PSV arriving
  // mid-crossfade must not leave the incoming scene stuck at its load-time defaults.
  apply_params_to(decks_[live_], params);
  if (armed_ || fading_) {
    apply_params_to(decks_[incoming_], params);
  }
}

void Pgae::apply_params_to(Deck& deck, const AudioParams& params) {
  for (size_t i = 0; i < kStemRoleCount; ++i) {
    StemRender& stem = deck.stems[i];
    if (!stem.present) {
      continue;
    }
    stem.level_target = gain_to_amp(params.gains[i]);
    if (params.active[i] != stem.active) {
      stem.active = params.active[i];
      schedule_crossfade(stem, params.active[i], deck.phase);
    }
  }
}

void Pgae::schedule_crossfade(StemRender& stem, bool activate, uint64_t phase) {
  // At the stem's next loop boundary (PGAE §6.2) — the coprime loop architecture is what
  // hides the seams. A newly scheduled fade replaces any pending one (the probe cancels
  // scheduled values the same way).
  stem.ramping = true;
  stem.ramp_to_active = activate;
  stem.ramp_start = next_loop_boundary(phase, stem.loop_frames);
  const uint64_t frames =
      static_cast<uint64_t>(opts_.crossfade_s * static_cast<double>(sample_rate_));
  stem.ramp_frames = frames > 0 ? frames : 1;
  stem.ramp_from = -1.0; // capture the env at ramp start, not at schedule time
}

double Pgae::render_deck(Deck& deck) {
  // One sample of one scene: every present stem's density envelope and smoothed level,
  // summed. Lifted verbatim out of render()'s inner loop so a second deck can be summed
  // the same way; the master chain (low-pass, master gain, limiter) stays downstream and
  // shared, so there is still exactly one limiter and it is still last.
  //
  // Advances deck.phase, so each scene keeps its own loop position.
  double mix = 0.0;
  for (size_t i = 0; i < kStemRoleCount; ++i) {
    StemRender& stem = deck.stems[i];
    if (!stem.present) {
      continue;
    }

    // Density envelope: equal-power ramp between 0 and 1 from the stem's loop boundary.
    // Deliberate divergence from the probe (documented in engine.h): the ramp leaves
    // from the env's CURRENT value, so a mid-fade retarget can never step.
    if (stem.ramping && deck.phase >= stem.ramp_start) {
      if (stem.ramp_from < 0.0) {
        stem.ramp_from = stem.env;
      }
      const double target = stem.ramp_to_active ? 1.0 : 0.0;
      const double t =
          static_cast<double>(deck.phase - stem.ramp_start) / static_cast<double>(stem.ramp_frames);
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

    const float sample =
        deck.assets->stems[i][static_cast<size_t>(deck.phase % stem.loop_frames)];
    mix += static_cast<double>(sample) * stem.env * stem.level_value;
  }
  ++deck.phase;
  return mix;
}

void Pgae::render(float* out, uint32_t frame_count) {
  // Once per block, not per sample: a scene change is a human tapping a tile, and the
  // block boundary is far finer than that. Allocation- and lock-free either way.
  service_swap();

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

    // Scene crossfade. The overlap is the ONLY place two decks are summed; outside it the
    // second deck is inert and costs nothing. Both sides use the same equal-power shape as
    // the density fades, so the pair holds constant acoustic power and the room never dips
    // through the middle of a mood change.
    if (armed_ && !fading_ && decks_[live_].phase >= fade_start_) {
      fading_ = true; // t == 0 this sample, so the incoming deck enters at gain 0
    }

    double mix;
    if (fading_) {
      const double t = static_cast<double>(decks_[live_].phase - fade_start_) /
                       static_cast<double>(fade_frames_);
      const double out_gain = equal_power_fade(t, false);
      const double in_gain = equal_power_fade(t, true);
      // Both decks advance, so the incoming scene's loops keep their own phase.
      mix = render_deck(decks_[live_]) * out_gain + render_deck(decks_[incoming_]) * in_gain;
      if (t >= 1.0) {
        finish_crossfade();
      }
    } else {
      mix = render_deck(decks_[live_]);
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
