// The ABI's real-time entry points. This TU is part of the rt_lock_primitives audit
// closure: nothing here (or in anything it includes) may name a lock, blocking call,
// logging, file I/O, or C allocator.

#include "prism/prism_core.h"

#include "rt_part.h"

namespace {

// Allocation-free zero fill (memset lives in <cstring>, which the audit tokens allow,
// but a plain loop keeps this TU's include set minimal).
void fill_silence(float* out, uint32_t frame_count) {
  for (uint32_t i = 0; i < frame_count; ++i) {
    out[i] = 0.0F;
  }
}

} // namespace

extern "C" {

uint32_t prism_sample_rate(const prism_core* core) {
  if (core == nullptr) {
    return 0;
  }
  const PrismRt& rt = prism_core_rt(const_cast<prism_core&>(*core));
  if (!rt.ready.load(std::memory_order_acquire)) {
    return 0;
  }
  return rt.pgae.sample_rate();
}

prism_result prism_render(prism_core* core, float* out_frames, uint32_t frame_count) {
  if (out_frames == nullptr) {
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  if (core == nullptr) {
    fill_silence(out_frames, frame_count);
    return PRISM_ERROR_INVALID_ARGUMENT;
  }
  PrismRt& rt = prism_core_rt(*core);
  if (!rt.ready.load(std::memory_order_acquire)) {
    fill_silence(out_frames, frame_count);
    return PRISM_ERROR_INVALID_STATE;
  }

  // The audited real-time path: one wait-free poll of the PSV exchange, then render.
  prism::psv::RtStateVector snapshot;
  if (rt.exchange.poll(snapshot)) {
    rt.pgae.consume_psv(snapshot);
  }
  rt.pgae.render(out_frames, frame_count);
  return PRISM_OK;
}

} // extern "C"
