// The C ABI implementation: composition root wiring PCE → RtExchange → PGAE behind the
// opaque handle. This is the ONLY place in the repository that sees both pce and pgae —
// and even here, the PSV snapshot is the only thing that crosses between them (the
// structural firewall holds at the composition layer too).

#include "prism/prism_core.h"

#include "rt_part.h"

#include "pce/pce.h"
#include "pgae/scene.h"
#include "psv/rt.h"

#include "miniaudio.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <mutex>
#include <new>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

std::string dir_of(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

prism::pce::Priority priority_from(int32_t p) {
  if (p == 0) {
    return prism::pce::Priority::Low;
  }
  if (p == 2) {
    return prism::pce::Priority::High;
  }
  return prism::pce::Priority::Medium; // out of range reads as medium, like the probe
}

prism_psv to_abi_psv(const prism::psv::RtStateVector& v) {
  prism_psv out{};
  out.arousal = v.value[prism::psv::kArousal];
  out.arousal_confidence = v.confidence[prism::psv::kArousal];
  out.valence = v.value[prism::psv::kValence];
  out.valence_confidence = v.confidence[prism::psv::kValence];
  out.cognitive_load = v.value[prism::psv::kCognitiveLoad];
  out.cognitive_load_confidence = v.confidence[prism::psv::kCognitiveLoad];
  out.readiness = v.value[prism::psv::kReadiness];
  out.readiness_confidence = v.confidence[prism::psv::kReadiness];
  out.update_timestamp_ms = v.update_timestamp_ms;
  out.sequence = v.sequence;
  static_assert(sizeof out.mode_hint == sizeof v.mode_hint, "mode_hint sizes must match");
  std::memcpy(out.mode_hint, v.mode_hint, sizeof out.mode_hint);
  return out;
}

} // namespace

struct prism_core {
  // Control-plane state (mutex-guarded or control-thread-only). The RT half lives in
  // rt_ so the render TU's include closure stays lock-free (see rt_part.h).
  prism::pce::PceOptions pce_options;
  int64_t check_interval_ms = 5'000;

  std::mutex pce_mutex; // ingress threads + inference thread share the PCE
  prism::pce::Pce pce;

  PrismRt rt;

  std::mutex ui_mutex; // guards the UI read-out copy
  prism::psv::RtStateVector ui_psv{};
  bool has_ui_psv = false;

  bool scene_loaded = false;
  std::atomic<bool> running{false};
  std::thread inference;

  bool device_running = false;
  ma_device device{};

  explicit prism_core(const prism::pce::PceOptions& options) : pce_options(options), pce(options) {}
};

PrismRt& prism_core_rt(prism_core& core) {
  return core.rt;
}

namespace {

void publish_psv(prism_core& core, const prism::psv::StateVector& v) {
  const prism::psv::RtStateVector rt = prism::psv::to_rt(v);
  core.rt.exchange.publish(rt);
  std::lock_guard<std::mutex> lock(core.ui_mutex);
  core.ui_psv = rt;
  core.has_ui_psv = true;
}

void inference_main(prism_core* core) {
  while (core->running.load(std::memory_order_acquire)) {
    // Sleep in short slices so prism_stop stays responsive.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(core->check_interval_ms);
    while (core->running.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(
          std::chrono::milliseconds(core->check_interval_ms < 50 ? core->check_interval_ms : 50));
    }
    if (!core->running.load(std::memory_order_acquire)) {
      break;
    }
    std::optional<prism::psv::StateVector> emitted;
    {
      std::lock_guard<std::mutex> lock(core->pce_mutex);
      emitted = core->pce.evaluate(now_ms());
    }
    if (emitted) {
      publish_psv(*core, *emitted);
    }
  }
}

void device_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
  prism_render(static_cast<prism_core*>(device->pUserData), static_cast<float*>(output),
               frame_count);
  (void)input;
}

} // namespace

extern "C" {

const char* prism_version(void) {
  return "0.1.0";
}

const char* prism_result_description(prism_result result) {
  switch (result) {
  case PRISM_OK:
    return "ok";
  case PRISM_ERROR_INVALID_ARGUMENT:
    return "invalid argument";
  case PRISM_ERROR_INVALID_STATE:
    return "call violates the engine lifecycle";
  case PRISM_ERROR_IO:
    return "scene manifest or stem file unreadable or invalid";
  case PRISM_ERROR_DEVICE:
    return "audio device could not be opened or started";
  case PRISM_ERROR_OUT_OF_MEMORY:
    return "out of memory";
  }
  return "unknown result";
}

prism_config prism_config_default(void) {
  prism_config config;
  config.tz_offset_min = 0;
  config.vertical = PRISM_VERTICAL_AQADEMIQ;
  config.cadence_ms = 0;
  config.check_interval_ms = 0;
  config.significant_delta = 0.0;
  return config;
}

prism_result prism_create(const prism_config* config, prism_core** out_core) {
  if (out_core == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  *out_core = nullptr;

  const prism_config defaults = prism_config_default();
  const prism_config& c = config != nullptr ? *config : defaults;

  prism::pce::PceOptions options;
  options.tz_offset_min = c.tz_offset_min;
  if (c.cadence_ms < 0 || c.check_interval_ms < 0 || c.significant_delta < 0.0) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  if (c.cadence_ms > 0) {
    options.cadence_ms = c.cadence_ms;
  }
  if (c.significant_delta > 0.0) {
    options.significant_delta = c.significant_delta;
  }
  switch (c.vertical) {
  case PRISM_VERTICAL_AQADEMIQ:
    options.vertical = prism::psv::Vertical::Aqademiq;
    break;
  case PRISM_VERTICAL_VENUES:
    options.vertical = prism::psv::Vertical::Venues;
    break;
  case PRISM_VERTICAL_AUTOMOTIVE:
    options.vertical = prism::psv::Vertical::Automotive;
    break;
  default:
    return PRISM_ERROR_INVALID_ARGUMENT;
  }

  prism_core* core = nullptr;
  try {
    core = new (std::nothrow) prism_core(options);
  } catch (...) {
    return PRISM_ERROR_OUT_OF_MEMORY; // nothrow covers the allocation, not member ctors
  }
  if (core == nullptr) {
    return PRISM_ERROR_OUT_OF_MEMORY;
  }
  if (c.check_interval_ms > 0) {
    core->check_interval_ms = c.check_interval_ms;
  }
  *out_core = core;
  return PRISM_OK;
}

void prism_destroy(prism_core* core) {
  if (core == nullptr) {
    return;
  }
  prism_stop(core);
  // Belt only, NOT the contract: a pull-model host must have quiesced its render thread
  // already (see the header's threading contract). Dropping `ready` narrows — but cannot
  // close — the window for a host that violates it.
  core->rt.ready.store(false, std::memory_order_release);
  delete core;
}

prism_result prism_load_scene(prism_core* core, const char* scenes_json_path) {
  if (core == nullptr || scenes_json_path == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  if (core->running.load(std::memory_order_acquire) || core->scene_loaded) {
    return PRISM_ERROR_INVALID_STATE; // one scene per handle in v1; load before start
  }

  // Exception firewall: nothing may throw across the C boundary. bad_alloc (stem
  // buffers can be large) maps to the documented OUT_OF_MEMORY; anything else on this
  // I/O path maps to IO.
  try {
    std::ifstream in(scenes_json_path);
    if (!in.good()) {
      return PRISM_ERROR_IO;
    }
    std::stringstream buffer;
    buffer << in.rdbuf();

    std::string error;
    const auto manifest = prism::pgae::parse_scene_manifest(buffer.str(), &error);
    if (!manifest) {
      return PRISM_ERROR_IO;
    }
    auto assets = prism::pgae::load_scene_assets(*manifest, manifest->default_scene,
                                                 dir_of(scenes_json_path), &error);
    if (!assets) {
      return PRISM_ERROR_IO;
    }
    if (!core->rt.pgae.load_scene(std::move(*assets), &error)) {
      return PRISM_ERROR_IO;
    }
    core->scene_loaded = true;
    core->rt.ready.store(true, std::memory_order_release);
    return PRISM_OK;
  } catch (const std::bad_alloc&) {
    return PRISM_ERROR_OUT_OF_MEMORY;
  } catch (...) {
    return PRISM_ERROR_IO;
  }
}

prism_result prism_start(prism_core* core) {
  if (core == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  if (!core->scene_loaded || core->running.load(std::memory_order_acquire)) {
    return PRISM_ERROR_INVALID_STATE;
  }
  {
    std::lock_guard<std::mutex> lock(core->pce_mutex);
    publish_psv(*core, core->pce.start(now_ms())); // cold-start neutral vector (spec §6)
  }
  core->running.store(true, std::memory_order_release);
  try {
    core->inference = std::thread(inference_main, core);
  } catch (...) {
    core->running.store(false, std::memory_order_release);
    return PRISM_ERROR_OUT_OF_MEMORY; // thread resources exhausted
  }
  return PRISM_OK;
}

prism_result prism_stop(prism_core* core) {
  if (core == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  prism_device_stop(core);
  if (core->running.exchange(false, std::memory_order_acq_rel)) {
    if (core->inference.joinable()) {
      core->inference.join();
    }
  }
  return PRISM_OK;
}

prism_result prism_report_app_switch(prism_core* core, int64_t t_ms) {
  if (core == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(core->pce_mutex);
  core->pce.report_app_switch(t_ms);
  return PRISM_OK;
}

prism_result prism_report_idle(prism_core* core, int64_t t_ms, int64_t idle_ms) {
  if (core == nullptr || idle_ms < 0) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(core->pce_mutex);
  core->pce.report_idle(t_ms, idle_ms);
  return PRISM_OK;
}

prism_result prism_report_task_deadlines(prism_core* core, const prism_task_deadline* tasks,
                                         size_t count) {
  // Human-scale bound (documented): also shields against negative lengths cast to
  // size_t, which would otherwise throw std::length_error ACROSS the C boundary and
  // abort C/dart hosts (found by adversarial review).
  if (core == nullptr || (tasks == nullptr && count > 0) || count > 4096) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  try {
    std::vector<prism::pce::TaskDeadline> deadlines;
    deadlines.reserve(count);
    for (size_t i = 0; i < count; ++i) {
      deadlines.push_back({tasks[i].due_ms, priority_from(tasks[i].priority)});
    }
    std::lock_guard<std::mutex> lock(core->pce_mutex);
    core->pce.report_task_deadlines(std::move(deadlines));
    return PRISM_OK;
  } catch (const std::bad_alloc&) {
    return PRISM_ERROR_OUT_OF_MEMORY;
  }
}

prism_result prism_get_psv(prism_core* core, prism_psv* out) {
  if (core == nullptr || out == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  std::lock_guard<std::mutex> lock(core->ui_mutex);
  if (!core->has_ui_psv) {
    return PRISM_ERROR_INVALID_STATE; // nothing emitted before prism_start
  }
  *out = to_abi_psv(core->ui_psv);
  return PRISM_OK;
}

prism_result prism_device_start(prism_core* core) {
  if (core == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  if (!core->scene_loaded || core->device_running) {
    return PRISM_ERROR_INVALID_STATE;
  }

  ma_device_config config = ma_device_config_init(ma_device_type_playback);
  config.playback.format = ma_format_f32;
  config.playback.channels = 1;
  config.sampleRate = core->rt.pgae.sample_rate();
  config.dataCallback = device_callback;
  config.pUserData = core;

  if (ma_device_init(nullptr, &config, &core->device) != MA_SUCCESS) {
    return PRISM_ERROR_DEVICE;
  }
  if (ma_device_start(&core->device) != MA_SUCCESS) {
    ma_device_uninit(&core->device);
    return PRISM_ERROR_DEVICE;
  }
  core->device_running = true;
  return PRISM_OK;
}

prism_result prism_device_stop(prism_core* core) {
  if (core == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  if (core->device_running) {
    ma_device_uninit(&core->device);
    core->device_running = false;
  }
  return PRISM_OK;
}

} // extern "C"
