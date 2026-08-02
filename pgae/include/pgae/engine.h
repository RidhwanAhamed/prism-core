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
#include "pgae/swap.h"
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
  //
  // NON-OWNING on purpose. The caller owns the decoded buffers and must keep them alive
  // for as long as the scene is loaded. A crossfade holds two scenes at once and retires
  // one of them, and an owning handle here would put that release on whichever thread
  // performed the swap — the audio thread. Real-time rule 1 forbids a free on the render
  // path, so ownership stays on the control side where a release is legal.
  bool load_scene(const SceneAssets* assets, std::string* error = nullptr);

  // Consume one PSV (§3 read contract): retarget all smoothed parameters, schedule any
  // density crossfades at each stem's next loop boundary.
  AudioParams consume_psv(const psv::StateVector& v);
  // Real-time overload for the audio thread (Task 4 three-thread model): same semantics,
  // takes the trivially copyable snapshot from psv::RtExchange. Allocation/lock-free.
  AudioParams consume_psv(const psv::RtStateVector& v);

  // Pull-model render: mono f32. Allocation/lock/log/IO-free.
  void render(float* out, uint32_t frame_count);

  uint32_t sample_rate() const { return sample_rate_; }

  // Prepares a deck for `assets` without touching anything the render path is reading, and
  // hands it to the audio thread. Control thread only, and NOT real-time safe — but it
  // does no I/O either: the caller has already decoded. Returns false if a crossfade is
  // still in flight, in which case the caller keeps ownership of `assets` and retries.
  //
  // `crossfade_frames` is the equal-power overlap length; `align` waits for the outgoing
  // scene's next loop boundary before starting it.
  bool begin_crossfade(const SceneAssets* assets, uint64_t crossfade_frames, bool align);

  // True from the moment a swap is posted until the overlap finishes. Any thread.
  bool crossfade_active() const {
    return swap_.pending() || busy_.load(std::memory_order_acquire);
  }

  // Control thread: the scene the render path has finished with, for the caller to
  // release. Null when there is nothing to collect. Call it periodically — this is the
  // only way a retired scene is ever handed back.
  const SceneAssets* collect_retired() { return retired_.collect(); }

  // Control thread, with nothing rendering: drops an armed-but-unconsumed swap so a host
  // that stopped mid-transition does not stay busy forever. Returns the scene to release.
  const SceneAssets* abandon_pending_crossfade();

private:
  PgaeOptions opts_;
  uint32_t sample_rate_ = 0;
  uint64_t sample_clock_ = 0;

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

  // One playing scene: the buffers, every piece of state DERIVED from them, and this
  // scene's own loop phase, in one struct.
  //
  // Keeping them together is the point. `present` and `loop_frames` are caches of the
  // buffers (set in load_scene), and render() indexes `assets->stems[i]` by
  // `phase % loop_frames` with no bounds check — so a deck whose buffers and derived
  // state disagree reads out of range or divides by zero. Publishing a deck as one unit
  // makes that state impossible to represent.
  //
  // `phase` is per-deck rather than shared because an incoming scene starts its loops
  // from their beginning, part-way through the outgoing scene's.
  struct Deck {
    const SceneAssets* assets = nullptr; // non-owning; see load_scene
    StemRender stems[kStemRoleCount];
    uint64_t phase = 0;
  };
  // Two slots so a scene change can overlap. Outside a crossfade only decks_[live_] is
  // rendered and the other is inert; there is never a third, because a crossfade cannot
  // be armed while one is running.
  Deck decks_[2];

  // Everything below down to swap_ is touched ONLY by the render path. The control thread
  // reaches the crossfade exclusively through swap_ / retired_ / busy_, so there is no
  // second writer for any of it. `armed_` means a swap has been accepted and is waiting
  // for its start sample; `fading_` means the overlap is running.
  size_t live_ = 0;
  size_t incoming_ = 1;
  bool armed_ = false;
  bool fading_ = false;
  uint64_t fade_start_ = 0;
  uint64_t fade_frames_ = 1;

  SwapMailbox swap_;
  RetiredScenes retired_;

  // Set by the render path when it accepts a swap, cleared when the overlap ends. Exists
  // so the control thread can ask "is a crossfade in flight?" without reading render-path
  // state — the answer gates begin_crossfade and is what the ABI reports.
  std::atomic<bool> busy_{false};

  double level_coeff_ = 0.0;  // one-pole coefficient for level moves
  double cutoff_coeff_ = 0.0; // one-pole coefficient for cutoff moves
  double cutoff_value_ = 0.0;
  double cutoff_target_ = 0.0;

  detail::BiquadLowpass lowpass_;
  detail::PeakLimiter limiter_;

  double gain_to_amp(double x) const;
  void apply_params(const AudioParams& params);
  void apply_params_to(Deck& deck, const AudioParams& params);
  void schedule_crossfade(StemRender& stem, bool activate, uint64_t phase);
  double render_deck(Deck& deck);
  void init_deck(Deck& deck, const SceneAssets* assets) const;
  void service_swap();
  void finish_crossfade();
};

} // namespace prism::pgae
