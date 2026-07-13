#pragma once

// The pre-allocated atomic double-buffer PSV handoff (CLAUDE.md real-time rule 2):
// single writer (inference thread), single reader (audio thread), no mutex on the path.
//
// Implemented as an atomic TRIPLE buffer — the textbook wait-free construction for
// "reader always gets the newest complete snapshot, nothing tears": the writer and reader
// each own a slot outright and trade with a shared mailbox slot via atomic exchange, so
// neither ever touches a slot the other is using. Three slots is the minimum that makes
// both sides wait-free without tearing; the "double buffer" of the rule names the
// technique, this is its safe form.

#include "psv/rt.h"

#include <atomic>
#include <cstdint>

namespace prism::psv {

class RtExchange {
public:
  RtExchange() = default;
  RtExchange(const RtExchange&) = delete;
  RtExchange& operator=(const RtExchange&) = delete;

  // Single writer. Wait-free, allocation-free.
  void publish(const RtStateVector& v) {
    slots_[write_idx_] = v;
    const uint32_t old = mailbox_.exchange(write_idx_ | kFreshBit, std::memory_order_acq_rel);
    write_idx_ = old & kIndexMask;
  }

  // Single reader. Wait-free, allocation-free. Returns true and fills `out` when a
  // snapshot newer than the last poll is available; leaves `out` untouched otherwise.
  bool poll(RtStateVector& out) {
    if ((mailbox_.load(std::memory_order_acquire) & kFreshBit) == 0) {
      return false; // fresh bit is only ever cleared by this reader → no race
    }
    const uint32_t old = mailbox_.exchange(read_idx_, std::memory_order_acq_rel);
    read_idx_ = old & kIndexMask;
    out = slots_[read_idx_];
    return true;
  }

private:
  static constexpr uint32_t kIndexMask = 3;
  static constexpr uint32_t kFreshBit = 4;

  RtStateVector slots_[3] = {};
  std::atomic<uint32_t> mailbox_{0}; // slot 0, not fresh
  uint32_t write_idx_ = 1;           // writer-owned slot
  uint32_t read_idx_ = 2;            // reader-owned slot
};

} // namespace prism::psv
