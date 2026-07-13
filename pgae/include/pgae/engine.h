#pragma once

// The minimum-viable PGAE (Task 3; consumption spec §11). The ONLY component that turns a
// PSV into sound; it reads ONLY a PSV — never PCE inputs or internals (the firewall, §2).
//
// Signal path (§4):  stems (sample-accurate loops) → env (density gate, equal-power
// crossfaded at loop boundaries) → level (one-pole smoothed mapped gain) → sum → master
// low-pass (smoothed cutoff) → master gain (click-free fade-in) → hard limiter → out.
//
// Real-time rules: render() never allocates, locks, logs, blocks, or does I/O. Everything
// it touches is pre-allocated in load_scene(). Single-threaded in this slice: the host
// calls consume_psv() between render blocks; Task 4 adds the three-thread model with the
// atomic double-buffer PSV handoff.
//
// DSP constants are ported from the probe's audio-engine.ts (time constants, crossfade
// length, headroom trim, limiter settings) — the validated feel. One deliberate
// divergence, documented at the crossfade site: a re-scheduled crossfade ramps from the
// env's CURRENT value instead of restarting the curve, so a rapid density flip can never
// step (click/pop prevention outranks verbatim here; identical behavior in normal
// operation, where flips arrive at least one PSV cadence apart).

#include "pgae/detail/dsp.h"
#include "pgae/mapping.h"
#include "pgae/scene.h"
#include "psv/psv.h"

#include <cstdint>
#include <string>

namespace prism::pgae {

struct PgaeOptions {
  // Verbatim from the probe (audio-engine.ts):
  double tc_gain_s = 0.25;    // one-pole time constant for level moves — never steps
  double tc_cutoff_s = 0.6;   // slower for timbral/spectral moves
  double crossfade_s = 1.5;   // equal-power density crossfade duration
  double amp_trim = 0.4;      // headroom so summed stems sit below the limiter
  double master_gain = 0.7;   // steady master level
  double master_fade_s = 0.5; // click-free fade-in from silence at start
  double initial_cutoff_hz = 2400.0;
  double lowpass_q = 0.7;
  double limiter_ceiling_db = -3.0;
  double limiter_attack_s = 0.003;
  double limiter_release_s = 0.1;
};

class Pgae {
public:
  explicit Pgae(PgaeOptions options = {});

  // Pre-allocates and configures everything the render path touches. Not real-time safe;
  // call before start().
  bool load_scene(SceneAssets assets, std::string* error = nullptr);

  // Consume one PSV (§3 read contract): retarget all smoothed parameters, schedule any
  // density crossfades at each stem's next loop boundary.
  AudioParams consume_psv(const psv::StateVector& v);
  // Real-time overload for the audio thread (Task 4 three-thread model): same semantics,
  // takes the trivially copyable snapshot from psv::RtExchange. Allocation/lock-free.
  AudioParams consume_psv(const psv::RtStateVector& v);

  // Pull-model render: mono f32. Allocation/lock/log/IO-free.
  void render(float* out, uint32_t frame_count);

  uint32_t sample_rate() const { return sample_rate_; }

private:
  PgaeOptions opts_;
  uint32_t sample_rate_ = 0;
  uint64_t sample_clock_ = 0;

  // Fixed-size per-stem state (index = StemRole); buffers live in assets_.
  SceneAssets assets_;
  struct StemRender {
    uint64_t loop_frames = 0;
    bool present = false;
    bool active = false;
    // env crossfade (equal-power), scheduled at a loop boundary
    double env = 0.0;
    bool ramping = false;
    bool ramp_to_active = false;
    double ramp_from = 0.0;
    uint64_t ramp_start = 0;
    uint64_t ramp_frames = 1;
    // level smoothing
    double level_value = 0.0;
    double level_target = 0.0;
  };
  StemRender stems_[kStemRoleCount];

  double level_coeff_ = 0.0;  // one-pole coefficient for level moves
  double cutoff_coeff_ = 0.0; // one-pole coefficient for cutoff moves
  double cutoff_value_ = 0.0;
  double cutoff_target_ = 0.0;

  detail::BiquadLowpass lowpass_;
  detail::PeakLimiter limiter_;

  double gain_to_amp(double x) const;
  void apply_params(const AudioParams& params);
  void schedule_crossfade(StemRender& stem, bool activate);
};

} // namespace prism::pgae
