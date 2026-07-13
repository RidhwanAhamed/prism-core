// Desktop harness.
//
// Two modes:
//   prism_harness [stem.wav] [--seconds N]
//     Task 0 smoke: preload one stem, loop it sample-accurately through a playback device.
//   prism_harness --scene assets/scenes.json [--fixture tests/golden/fixture-*.jsonl] [--seconds N]
//     Task 3: the full PGAE — scene playback driven by a PSV schedule, for the audible
//     parity check against the probe. Default schedule is a scripted sweep
//     (neutral → focus → overload → recovery); --fixture replays a golden trace's PSV
//     trajectory at its real cadence instead.
//
// Real-time rules hold even here: the audio callback only consumes pre-allocated state —
// no allocation, no locks, no logging, no I/O. PSV applications happen ON the audio
// thread from a pre-built schedule (Task 4 brings the cross-thread double buffer).

#include "miniaudio.h"

#include "pgae/engine.h"
#include "pgae/scene.h"
#include "psv/detail/json_value.h"
#include "psv/psv.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace {

// ---------------------------------------------------------------------------------------
// Task 0 mode: single looping stem (unchanged behavior).
// ---------------------------------------------------------------------------------------

struct LoopState {
  const float* frames = nullptr; // interleaved, pre-decoded
  ma_uint64 total_frames = 0;    // per-channel frame count
  ma_uint32 channels = 0;
  ma_uint64 cursor = 0;
};

// Real-time path: copy-and-wrap only.
void loop_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
  auto* s = static_cast<LoopState*>(device->pUserData);
  auto* dst = static_cast<float*>(output);
  ma_uint32 written = 0;
  while (written < frame_count) {
    ma_uint64 available = s->total_frames - s->cursor;
    ma_uint32 chunk = frame_count - written;
    if (static_cast<ma_uint64>(chunk) > available) {
      chunk = static_cast<ma_uint32>(available);
    }
    std::memcpy(dst + static_cast<ma_uint64>(written) * s->channels,
                s->frames + s->cursor * s->channels,
                static_cast<size_t>(chunk) * s->channels * sizeof(float));
    written += chunk;
    s->cursor += chunk;
    if (s->cursor == s->total_frames) {
      s->cursor = 0; // sample-accurate loop boundary
    }
  }
  (void)input;
}

int run_loop_mode(const std::string& path, long run_seconds) {
  ma_decoder_config decoder_config = ma_decoder_config_init(ma_format_f32, 0, 0);
  ma_decoder decoder;
  if (ma_decoder_init_file(path.c_str(), &decoder_config, &decoder) != MA_SUCCESS) {
    std::fprintf(stderr, "error: cannot open %s (run from the repo root?)\n", path.c_str());
    return 1;
  }

  const ma_uint32 channels = decoder.outputChannels;
  const ma_uint32 sample_rate = decoder.outputSampleRate;

  std::vector<float> pcm;
  std::vector<float> chunk(static_cast<size_t>(4096) * channels);
  ma_result decode_result = MA_SUCCESS;
  for (;;) {
    ma_uint64 frames_read = 0;
    decode_result = ma_decoder_read_pcm_frames(&decoder, chunk.data(), 4096, &frames_read);
    if (frames_read > 0) {
      pcm.insert(pcm.end(), chunk.begin(),
                 chunk.begin() + static_cast<size_t>(frames_read) * channels);
    }
    if (decode_result != MA_SUCCESS || frames_read < 4096) {
      break;
    }
  }
  ma_decoder_uninit(&decoder);

  if (decode_result != MA_SUCCESS && decode_result != MA_AT_END) {
    std::fprintf(stderr, "warning: decode of %s ended early (%s); looping the partial stem\n",
                 path.c_str(), ma_result_description(decode_result));
  }
  if (pcm.empty()) {
    std::fprintf(stderr, "error: %s decoded to zero frames\n", path.c_str());
    return 1;
  }

  LoopState state;
  state.frames = pcm.data();
  state.total_frames = pcm.size() / channels;
  state.channels = channels;

  ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
  device_config.playback.format = ma_format_f32;
  device_config.playback.channels = channels;
  device_config.sampleRate = sample_rate;
  device_config.dataCallback = loop_callback;
  device_config.pUserData = &state;

  ma_device device;
  if (ma_device_init(nullptr, &device_config, &device) != MA_SUCCESS) {
    std::fprintf(stderr, "error: failed to open playback device\n");
    return 1;
  }
  if (ma_device_start(&device) != MA_SUCCESS) {
    std::fprintf(stderr, "error: failed to start playback device\n");
    ma_device_uninit(&device);
    return 1;
  }

  std::printf("playing %s | %.1fs loop, %u Hz, %u ch\n", path.c_str(),
              static_cast<double>(state.total_frames) / sample_rate, sample_rate, channels);

  if (run_seconds > 0) {
    std::printf("running for %ld second(s)...\n", run_seconds);
    std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
  } else {
    std::printf("press Enter to stop.\n");
    std::getchar();
  }

  ma_device_uninit(&device);
  return 0;
}

// ---------------------------------------------------------------------------------------
// Task 3 mode: PGAE scene playback driven by a PSV schedule.
// ---------------------------------------------------------------------------------------

struct ScheduledPsv {
  ma_uint64 at_sample = 0;
  prism::psv::StateVector psv;
  const char* label = "";
};

struct SceneRunState {
  prism::pgae::Pgae* engine = nullptr;
  const ScheduledPsv* items = nullptr;
  size_t count = 0;
  size_t next = 0;
  ma_uint64 sample_clock = 0;
};

// Real-time path: schedule lookups and PGAE render only.
void scene_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
  auto* s = static_cast<SceneRunState*>(device->pUserData);
  while (s->next < s->count && s->sample_clock >= s->items[s->next].at_sample) {
    s->engine->consume_psv(s->items[s->next].psv);
    ++s->next;
  }
  s->engine->render(static_cast<float*>(output), frame_count);
  s->sample_clock += frame_count;
  (void)input;
}

prism::psv::StateVector sweep_psv(double a, double ac, double l, double lc, double r, double rc) {
  prism::psv::StateVector v;
  v.update_timestamp_ms = 1;
  v.arousal = {a, ac};
  v.cognitive_load = {l, lc};
  v.readiness = {r, rc};
  return v;
}

// Scripted 80s listening sweep: cold start → building focus → overload (audio recedes to
// bed+sub, darkens) → recovery (layers fade back at loop boundaries).
std::vector<ScheduledPsv> build_sweep_schedule(ma_uint32 sample_rate) {
  const auto at = [sample_rate](double seconds) {
    return static_cast<ma_uint64>(seconds * sample_rate);
  };
  return {
      {at(0), prism::psv::neutral(prism::psv::Vertical::Aqademiq, 1), "cold start (neutral)"},
      {at(10), sweep_psv(0.6, 0.7, 0.4, 0.6, 0.6, 0.6), "settling in"},
      {at(25), sweep_psv(0.7, 0.8, 0.7, 0.8, 0.5, 0.6), "focus building"},
      {at(40), sweep_psv(0.8, 0.9, 1.0, 0.9, 0.3, 0.7), "overload: recede + ground"},
      {at(60), sweep_psv(0.5, 0.8, 0.3, 0.8, 0.7, 0.7), "recovery: layers return"},
  };
}

// Replay the psv records of a golden trace at their real relative cadence.
std::vector<ScheduledPsv> build_fixture_schedule(const std::string& path, ma_uint32 sample_rate,
                                                 std::string* error) {
  std::ifstream in(path);
  if (!in.good()) {
    *error = "cannot open " + path;
    return {};
  }
  std::vector<ScheduledPsv> schedule;
  std::string line;
  ma_uint64 first_t = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    prism::psv::detail::JValue record;
    std::string parse_error;
    if (!prism::psv::detail::parse_json(line, record, parse_error)) {
      *error = path + ": " + parse_error;
      return {};
    }
    const auto* type = record.find("type");
    if (!type || type->string != "psv") {
      continue;
    }
    const auto* t = record.find("t");
    const auto* state = record.find("state");
    if (!t || !t->number_is_integer || !state) {
      continue;
    }
    prism::psv::StateVector v;
    v.update_timestamp_ms = t->integer;
    const auto read_dim = [&](const char* name, prism::psv::Dimension& dim) {
      if (const auto* d = state->find(name)) {
        if (const auto* value = d->find("value")) {
          dim.value = value->number;
        }
        if (const auto* confidence = d->find("confidence")) {
          dim.confidence = confidence->number;
        }
      }
    };
    read_dim("arousal", v.arousal);
    read_dim("valence", v.valence);
    read_dim("cognitive_load", v.cognitive_load);
    read_dim("readiness", v.readiness);
    if (schedule.empty()) {
      first_t = static_cast<ma_uint64>(t->integer);
    }
    const ma_uint64 offset_ms = static_cast<ma_uint64>(t->integer) - first_t;
    schedule.push_back({offset_ms * sample_rate / 1000, v, "fixture psv"});
  }
  if (schedule.empty()) {
    *error = path + ": no psv records";
  }
  return schedule;
}

std::string dir_of(const std::string& path) {
  const auto slash = path.find_last_of('/');
  return slash == std::string::npos ? std::string(".") : path.substr(0, slash);
}

int run_scene_mode(const std::string& manifest_path, const std::string& fixture_path,
                   long run_seconds) {
  std::ifstream in(manifest_path);
  if (!in.good()) {
    std::fprintf(stderr, "error: cannot open %s\n", manifest_path.c_str());
    return 1;
  }
  std::stringstream buffer;
  buffer << in.rdbuf();

  std::string error;
  const auto manifest = prism::pgae::parse_scene_manifest(buffer.str(), &error);
  if (!manifest) {
    std::fprintf(stderr, "error: bad manifest: %s\n", error.c_str());
    return 1;
  }
  auto assets = prism::pgae::load_scene_assets(*manifest, manifest->default_scene,
                                               dir_of(manifest_path), &error);
  if (!assets) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }
  const ma_uint32 sample_rate = assets->sample_rate;

  prism::pgae::Pgae engine;
  if (!engine.load_scene(std::move(*assets), &error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  std::vector<ScheduledPsv> schedule;
  if (!fixture_path.empty()) {
    schedule = build_fixture_schedule(fixture_path, sample_rate, &error);
    if (schedule.empty()) {
      std::fprintf(stderr, "error: %s\n", error.c_str());
      return 1;
    }
    std::printf("scene %s | driving from %s (%zu PSVs at real cadence)\n",
                manifest->default_scene.c_str(), fixture_path.c_str(), schedule.size());
  } else {
    schedule = build_sweep_schedule(sample_rate);
    std::printf("scene %s | scripted sweep:\n", manifest->default_scene.c_str());
    for (const auto& item : schedule) {
      std::printf("  %5.1fs  %s\n", static_cast<double>(item.at_sample) / sample_rate, item.label);
    }
  }

  SceneRunState state;
  state.engine = &engine;
  state.items = schedule.data();
  state.count = schedule.size();

  ma_device_config device_config = ma_device_config_init(ma_device_type_playback);
  device_config.playback.format = ma_format_f32;
  device_config.playback.channels = 1;
  device_config.sampleRate = sample_rate;
  device_config.dataCallback = scene_callback;
  device_config.pUserData = &state;

  ma_device device;
  if (ma_device_init(nullptr, &device_config, &device) != MA_SUCCESS) {
    std::fprintf(stderr, "error: failed to open playback device\n");
    return 1;
  }
  if (ma_device_start(&device) != MA_SUCCESS) {
    std::fprintf(stderr, "error: failed to start playback device\n");
    ma_device_uninit(&device);
    return 1;
  }

  if (run_seconds > 0) {
    std::printf("running for %ld second(s)...\n", run_seconds);
    std::this_thread::sleep_for(std::chrono::seconds(run_seconds));
  } else {
    std::printf("press Enter to stop.\n");
    std::getchar();
  }

  ma_device_uninit(&device);
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::string stem_path = "assets/stems/bed.wav";
  std::string scene_path;
  std::string fixture_path;
  long run_seconds = 0; // 0 = until Enter

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      run_seconds = std::strtol(argv[++i], nullptr, 10);
    } else if (std::strcmp(argv[i], "--scene") == 0 && i + 1 < argc) {
      scene_path = argv[++i];
    } else if (std::strcmp(argv[i], "--fixture") == 0 && i + 1 < argc) {
      fixture_path = argv[++i];
    } else {
      stem_path = argv[i];
    }
  }

  if (!scene_path.empty()) {
    return run_scene_mode(scene_path, fixture_path, run_seconds);
  }
  return run_loop_mode(stem_path, run_seconds);
}
