#pragma once

// Control → audio scene handover, and the retirement the audio side hands back.
//
// A mood change means a different SCENE — a different set of stems — so the engine has to
// hold two at once for the length of the crossfade. Decoding is I/O and allocation, so it
// happens on the control thread; all the render path ever receives is a pointer to buffers
// that are already resident, plus how long to take.
//
// Shaped after psv::RtExchange (pre-allocated, single-writer/single-reader, wait-free) with
// one deliberate difference: RtExchange is a latest-wins SNAPSHOT and may drop, because a
// stale PSV is harmless. This is a COMMAND. Dropping one would strand a decoded scene that
// nobody ever plays and leave the room on the old mood, so the writer refuses to overwrite
// an unconsumed slot and reports that to its caller instead.
//
// Neither side ever owns a scene here. The pointer is non-owning both directions: the
// control side owns the storage and is the only side allowed to release it, because
// releasing is one of the things real-time rule 1 forbids on the render path.

#include "pgae/scene.h"

#include <atomic>
#include <cstdint>

namespace prism::pgae {

// What the control side asks the render path to do.
struct SceneSwap {
  // Decoded and resident before this is posted. Non-owning.
  const SceneAssets* assets = nullptr;
  // Length of the equal-power overlap, in frames. Never zero — the caller clamps.
  uint64_t crossfade_frames = 1;
  // Begin at the outgoing scene's next loop boundary (real-time rule 5) rather than at the
  // next block. Musically the right thing, but it costs up to one loop period of waiting,
  // which for the Venues stems is 16 s — long enough that the shortest transition setting
  // deliberately turns it off.
  bool align_to_loop_boundary = true;
};

// One-shot command slot. post() from the control thread, take() from the audio thread.
// Wait-free on both sides; no allocation, no blocking, no ownership.
class SwapMailbox {
public:
  // Control thread. Returns false when a command is still unconsumed — the caller must
  // treat that as "busy" and keep owning the scene it was trying to hand over.
  bool post(const SceneSwap& swap) {
    if (pending_.load(std::memory_order_acquire)) {
      return false;
    }
    slot_ = swap;
    pending_.store(true, std::memory_order_release);
    return true;
  }

  // Audio thread. Returns false when there is nothing to do, which is the common case.
  bool take(SceneSwap& out) {
    if (!pending_.load(std::memory_order_acquire)) {
      return false;
    }
    out = slot_;
    pending_.store(false, std::memory_order_release);
    return true;
  }

  bool pending() const { return pending_.load(std::memory_order_acquire); }

  // Control thread, and only while nothing is rendering: abandons an unconsumed command.
  // The escape hatch for a host that armed a swap and then stopped — without it the slot
  // would stay occupied and every later post() would report busy forever.
  void abandon() { pending_.store(false, std::memory_order_release); }

private:
  SceneSwap slot_{};
  std::atomic<bool> pending_{false};
};

// Audio → control: the scene the render path has finished with and will never read again.
//
// The render path cannot release it, so it publishes the pointer here and forgets it. The
// control side collects it later and releases it there. A single slot is enough because a
// crossfade cannot start while one is in flight, so at most one scene is ever awaiting
// collection.
class RetiredScenes {
public:
  // Audio thread. Wait-free; overwrites nothing, because collect() has always run before
  // another crossfade can be armed.
  void retire(const SceneAssets* assets) { slot_.store(assets, std::memory_order_release); }

  // Control thread. Returns the scene to release, or null. Clears the slot.
  const SceneAssets* collect() { return slot_.exchange(nullptr, std::memory_order_acq_rel); }

private:
  std::atomic<const SceneAssets*> slot_{nullptr};
};

} // namespace prism::pgae
