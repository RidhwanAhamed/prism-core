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
  localtime_r(&now, &local);
  return static_cast<int32_t>(-local.tm_gmtoff / 60);
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

int main(int argc, char** argv) {
  std::string scene_path = "assets/scenes.json";
  long run_seconds = 0; // 0 = until Enter

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      run_seconds = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
      scene_path = argv[++i];
    } else {
      std::fprintf(stderr, "usage: prism_harness [--scene scenes.json] [--seconds N]\n");
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

  if ((result = prism_start(core)) != PRISM_OK || (result = prism_device_start(core)) != PRISM_OK) {
    std::fprintf(stderr, "error: start: %s\n", prism_result_description(result));
    prism_destroy(core);
    return 1;
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
