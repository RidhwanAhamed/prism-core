// Task 0 harness: preload one stem into memory, loop it sample-accurately
// through a miniaudio playback device.
//
// Even at scaffold stage the render callback obeys the real-time rules: the
// whole stem is decoded up front, and the callback only copies from that
// pre-allocated buffer — no allocation, no locks, no logging, no I/O.
//
// Usage: prism_harness [path/to/stem.wav] [--seconds N]
//   default stem: assets/stems/bed.wav (run from the repo root)
//   --seconds N : stop automatically after N seconds (useful for scripted runs)

#include "miniaudio.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

struct LoopState {
  const float* frames = nullptr; // interleaved, pre-decoded
  ma_uint64 total_frames = 0;    // per-channel frame count
  ma_uint32 channels = 0;
  ma_uint64 cursor = 0;
};

// Real-time path: copy-and-wrap only. Nothing here may allocate, lock, log,
// block, or touch I/O.
void data_callback(ma_device* device, void* output, const void* input, ma_uint32 frame_count) {
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

} // namespace

int main(int argc, char** argv) {
  std::string path = "assets/stems/bed.wav";
  long run_seconds = 0; // 0 = until Enter

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc) {
      run_seconds = std::strtol(argv[++i], nullptr, 10);
    } else {
      path = argv[i];
    }
  }

  // Decode the entire stem up front (stems are preloaded at scene load — rule 4).
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
  device_config.dataCallback = data_callback;
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
