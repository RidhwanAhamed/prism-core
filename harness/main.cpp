// Desktop harness — consumes ONLY the public C ABI (include/prism/prism_core.h).
//
// That constraint is the point (CLAUDE.md): this program is the living proof that the
// boundary is complete, and the reference for every future binding (dart:ffi via ffigen,
// Capacitor, OEM SDKs). If the harness needs an internal header, the ABI is missing
// something.
//
//   prism_harness [--scene assets/scenes.json] [--seconds N]
//
// Runs the full live pipeline through the ABI: scripted ingress events (standing in for
// a platform shell's capture layer) → internal inference thread → PSV exchange → built-in
// audio device. Prints each newly emitted PSV via prism_get_psv. Behavior cycles every
// 90s: focused → scattered → away.

#include "prism/prism_core.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// JS Date#getTimezoneOffset convention (UTC − local, minutes); circadian is local-hours.
int32_t local_tz_offset_min() {
  const std::time_t now = std::time(nullptr);
  std::tm local{};
#ifdef _WIN32
  // No localtime_r/tm_gmtoff on Windows. Re-reading the local fields as if they were UTC
  // yields (local − UTC) directly, and stays DST-correct because localtime_s applied it.
  localtime_s(&local, &now);
  const std::time_t as_utc = _mkgmtime(&local);
  return static_cast<int32_t>(-(as_utc - now) / 60);
#else
  localtime_r(&now, &local);
  return static_cast<int32_t>(-local.tm_gmtoff / 60);
#endif
}

void sleep_or_stop(double seconds, const std::atomic<bool>& stop) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(static_cast<int64_t>(seconds * 1000));
  while (!stop.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

// Scripted behavior standing in for the capture layer: bare timestamps and durations
// only, exactly what a platform shell forwards.
void ingress_and_print_loop(prism_core* core, const std::atomic<bool>& stop) {
  int tick = 0;
  int64_t last_seq = -1;
  while (!stop.load(std::memory_order_acquire)) {
    sleep_or_stop(5.0, stop);
    if (stop.load(std::memory_order_acquire)) {
      return;
    }
    const int64_t t = now_ms();
    const int phase_s = (tick * 5) % 90;
    ++tick;
    if (phase_s < 30) {
      prism_report_idle(core, t, (tick % 3) * 700); // focused: sub-threshold idle noise
    } else if (phase_s < 60) {
      prism_report_idle(core, t, 500);
      prism_report_app_switch(core, t); // scattered: a switch every capture tick
    } else {
      prism_report_idle(core, t, 20'000 + (phase_s - 60) * 1'000); // away: idle climbing
    }

    prism_psv psv;
    if (prism_get_psv(core, &psv) == PRISM_OK && psv.sequence != last_seq) {
      last_seq = psv.sequence;
      std::printf("[psv] seq=%lld arousal=%.2f@%.2f load=%.2f@%.2f readiness=%.2f@%.2f\n",
                  static_cast<long long>(psv.sequence), psv.arousal, psv.arousal_confidence,
                  psv.cognitive_load, psv.cognitive_load_confidence, psv.readiness,
                  psv.readiness_confidence);
    }
  }
}

} // namespace

// --- Venue mood presets ------------------------------------------------------------------
// A mood is a point in PSV space (see prism_set_mood_override). These six are DERIVED from
// pgae/src/mapping.cpp rather than dialled by ear, because the mapping is what decides
// whether two moods actually sound different:
//
//   density   = 0.5 + 0.9a - 1.0l   gates which stems open: pulse >= 0.35, air >= 0.55,
//                                   lead >= 0.72 (bed and sub are always on)
//   brightness= 0.55 + 0.9a - 0.8l  drives the master low-pass, 300 Hz .. 12 kHz
//   sub gain  = 0.5 - 0.6r          low-end weight, INDEPENDENT of arousal
//
// where a/l/r are (value - 0.5) after confidence weighting. So arousal buys layers AND
// brightness together; load takes them away; readiness moves the bottom end on its own.
// Each preset below is chosen to land on a distinct side of the density gates, so the six
// moods differ in how many layers are playing — not merely in volume.
//
// valence is set for completeness but is INERT in v1 (mapping.cpp, PGAE §11).
//
// The resulting spread, computed from the formulas above:
//   wind-down       2 layers    642 Hz   sub 0.68  (darkest, heaviest bottom)
//   morning-calm    3 layers   2787 Hz   sub 0.43  (airy, light bottom)
//   daytime-flow    3 layers   3030 Hz   sub 0.50
//   evening-warmth  5 layers   4920 Hz   sub 0.59  (deep, warm)
//   afternoon-lift  4 layers   5970 Hz   sub 0.47  (bright, forward)
//   peak            5 layers  12000 Hz   sub 0.45  (widest open)
//
// UNVERIFIED BY EAR. These satisfy the mood spec's density/energy ordering on paper; which
// ones actually hold up is a listening judgement.
struct MoodPreset {
  const char* id;        // the app's mood id
  const char* mode_hint; // psv::mode_hint::kVenue*
  double arousal, valence, cognitive_load, readiness;
};

constexpr MoodPreset kMoods[] = {
    // Sparse and dark. Spec: "solo or duo textures only", "sub-heavy drone", no percussion.
    // Density 0.158 keeps it to bed+sub — the duo — and low readiness lifts the sub.
    {"wind-down", "venue_wind_down", 0.12, 0.40, 0.50, 0.20},
    // Sparse but OPEN. Same layer sparsity as wind-down at the low end, opposite character:
    // low load lets the air stem through and keeps the filter up, high readiness keeps the
    // bottom light — "felt as warmth not weight".
    {"morning-calm", "venue_morning_calm", 0.32, 0.70, 0.22, 0.62},
    // Steady and even. Just past the pulse gate, so the groove is present but not forward.
    {"daytime-flow", "venue_daytime_flow", 0.52, 0.65, 0.42, 0.50},
    // Golden hour. Just past the LEAD gate (density 0.728) so all five layers play, with
    // load held high enough to keep the filter down — rich but not bright, and low
    // readiness gives the "felt in the chest" bottom.
    {"evening-warmth", "venue_evening_warmth", 0.62, 0.60, 0.38, 0.35},
    // A step up, not a peak: four layers, brighter and more forward than daytime, but the
    // lead stays shut so it never becomes the busiest thing in the room.
    {"afternoon-lift", "venue_afternoon_lift", 0.70, 0.72, 0.40, 0.55},
    // Everything open, filter wide, pulse loudest of the six.
    {"peak", "venue_peak", 0.92, 0.70, 0.35, 0.58},
};

// Offline render to a 32-bit-float WAV via the PULL model (prism_render), so a mood can be
// captured and compared without an audio device in the loop. The 44-byte header is written
// by hand deliberately: the harness must not gain a dependency just to prove a point, and
// this keeps it consuming nothing but the C ABI.
bool render_to_wav(prism_core* core, const char* path, long seconds) {
  const uint32_t rate = prism_sample_rate(core);
  if (rate == 0) {
    std::fprintf(stderr, "error: no scene loaded\n");
    return false;
  }
  std::FILE* f = std::fopen(path, "wb");
  if (f == nullptr) {
    std::fprintf(stderr, "error: cannot write %s\n", path);
    return false;
  }

  const uint32_t total_frames = rate * static_cast<uint32_t>(seconds);
  const uint32_t data_bytes = total_frames * 4; // mono float32
  const auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
  const auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };

  std::fwrite("RIFF", 1, 4, f);
  u32(36 + data_bytes);
  std::fwrite("WAVEfmt ", 1, 8, f);
  u32(16);          // PCM-style fmt chunk size
  u16(3);           // WAVE_FORMAT_IEEE_FLOAT
  u16(1);           // mono — the engine renders mono
  u32(rate);
  u32(rate * 4);    // byte rate
  u16(4);           // block align
  u16(32);          // bits per sample
  std::fwrite("data", 1, 4, f);
  u32(data_bytes);

  constexpr uint32_t kBlock = 512;
  float block[kBlock];
  for (uint32_t done = 0; done < total_frames; done += kBlock) {
    const uint32_t n = (total_frames - done) < kBlock ? (total_frames - done) : kBlock;
    if (prism_render(core, block, n) != PRISM_OK) {
      std::fprintf(stderr, "error: render failed\n");
      std::fclose(f);
      return false;
    }
    std::fwrite(block, 4, n, f);
  }
  std::fclose(f);
  std::printf("wrote %s (%lds @ %u Hz, mono f32)\n", path, seconds, rate);
  return true;
}

const MoodPreset* find_mood(const char* id) {
  for (const MoodPreset& m : kMoods) {
    if (std::strcmp(m.id, id) == 0) {
      return &m;
    }
  }
  return nullptr;
}

int main(int argc, char** argv) {
  std::string scene_path = "assets/scenes.json";
  long run_seconds = 0; // 0 = until Enter
  const MoodPreset* mood = nullptr;
  const char* render_path = nullptr;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      run_seconds = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
      scene_path = argv[++i];
    } else if (std::strcmp(argv[i], "--mood") == 0 && i + 1 < argc) {
      mood = find_mood(argv[++i]);
      if (mood == nullptr) {
        std::fprintf(stderr, "error: unknown mood '%s'. known:", argv[i]);
        for (const MoodPreset& m : kMoods) {
          std::fprintf(stderr, " %s", m.id);
        }
        std::fprintf(stderr, "\n");
        return 1;
      }
    } else if (std::strcmp(argv[i], "--render") == 0 && i + 1 < argc) {
      render_path = argv[++i];
    } else {
      std::fprintf(stderr, "usage: prism_harness [--scene scenes.json] [--seconds N] "
                           "[--mood ID] [--render out.wav]\n");
      return 1;
    }
  }

  std::printf("prism core %s (ABI %d.%d.%d)\n", prism_version(), PRISM_ABI_VERSION_MAJOR,
              PRISM_ABI_VERSION_MINOR, PRISM_ABI_VERSION_PATCH);

  prism_config config = prism_config_default();
  config.tz_offset_min = local_tz_offset_min();

  prism_core* core = nullptr;
  prism_result result = prism_create(&config, &core);
  if (result != PRISM_OK) {
    std::fprintf(stderr, "error: create: %s\n", prism_result_description(result));
    return 1;
  }

  result = prism_load_scene(core, scene_path.c_str());
  if (result != PRISM_OK) {
    std::fprintf(stderr, "error: load %s: %s (run from the repo root?)\n", scene_path.c_str(),
                 prism_result_description(result));
    prism_destroy(core);
    return 1;
  }

  // One synthetic deadline tomorrow so deadline pressure participates.
  const prism_task_deadline deadline = {now_ms() + 24 * 3'600'000, 2};
  prism_report_task_deadlines(core, &deadline, 1);

  // In render mode the host owns the clock: prism_render pulls frames, so the built-in
  // device must NOT also be running (the two output paths are mutually exclusive).
  if ((result = prism_start(core)) != PRISM_OK ||
      (render_path == nullptr && (result = prism_device_start(core)) != PRISM_OK)) {
    std::fprintf(stderr, "error: start: %s\n", prism_result_description(result));
    prism_destroy(core);
    return 1;
  }

  if (mood != nullptr) {
    prism_mood_override o{};
    o.mode_hint = mood->mode_hint;
    o.arousal = mood->arousal;
    o.valence = mood->valence;
    o.cognitive_load = mood->cognitive_load;
    o.readiness = mood->readiness;
    o.confidence = 1.0; // a pinned mood is a statement of fact, not an inference
    if ((result = prism_set_mood_override(core, &o)) != PRISM_OK) {
      std::fprintf(stderr, "error: set mood: %s\n", prism_result_description(result));
      prism_destroy(core);
      return 1;
    }
    std::printf("mood PINNED: %s (%s) | a=%.2f l=%.2f r=%.2f | simulated behavior is "
                "ignored while pinned\n",
                mood->id, mood->mode_hint, mood->arousal, mood->cognitive_load,
                mood->readiness);
  }

  if (render_path != nullptr) {
    const bool ok = render_to_wav(core, render_path, run_seconds > 0 ? run_seconds : 40);
    prism_destroy(core);
    return ok ? 0 : 1;
  }

  std::printf("live pipeline through the C ABI | %u Hz | behavior cycles every 90s: "
              "focused → scattered → away\n",
              prism_sample_rate(core));

  std::atomic<bool> stop{false};
  std::thread ingress(ingress_and_print_loop, core, std::cref(stop));

  if (run_seconds > 0) {
    std::printf("running for %ld second(s)...\n", run_seconds);
    std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
  } else {
    std::printf("press Enter to stop.\n");
    std::getchar();
  }

  stop.store(true, std::memory_order_release);
  ingress.join();
  prism_destroy(core); // stops device + inference internally
  return 0;
}
