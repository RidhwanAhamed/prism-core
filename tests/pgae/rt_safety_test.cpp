#include <gtest/gtest.h>

#include "pgae/engine.h"
#include "psv/exchange.h"
#include "psv/rt.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <new>
#include <thread>
#include <vector>

#ifdef _WIN32
// Windows has no posix_memalign; the CRT pairs _aligned_malloc with _aligned_free.
#include <malloc.h>
#endif

// Task 4 acceptance: "instrumentation shows zero allocations/locks in the render path
// under a 10-minute run." This TU replaces the global allocation functions for the whole
// test binary; a THREAD-LOCAL flag scopes counting to the render thread, so the writer
// thread (and gtest itself) can allocate freely while the audio path is being audited.
// Coverage split (per adversarial review): this dynamic audit catches the C++ operator
// new/delete family only — direct C-allocator calls (malloc/free) and locks allocate
// nothing it can see, so those classes are enforced by the rt_lock_primitives ctest,
// which token-scans the compiler-computed include closure of the render path.

namespace {
thread_local bool t_rt_audit = false;
std::atomic<uint64_t> g_rt_allocations{0};

void count_if_audited() {
  if (t_rt_audit) {
    g_rt_allocations.fetch_add(1, std::memory_order_relaxed);
  }
}
} // namespace

// GCC 16 pairs the std::free below against the operator new above and reports
// -Wmismatched-new-delete. That analysis is for ordinary call sites; here these ARE the
// global replacement operators, and [new.delete] requires exactly this pairing — malloc
// in the replacement new, free in the replacement delete. Scoped to this TU, which
// exists only to count allocations.
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif

void* operator new(std::size_t size) {
  count_if_audited();
  if (void* p = std::malloc(size)) {
    return p;
  }
  throw std::bad_alloc();
}
void* operator new[](std::size_t size) {
  return ::operator new(size);
}
void* operator new(std::size_t size, std::align_val_t align) {
  count_if_audited();
#ifdef _WIN32
  void* p = _aligned_malloc(size, static_cast<std::size_t>(align));
  if (p == nullptr) {
    throw std::bad_alloc();
  }
#else
  void* p = nullptr;
  if (posix_memalign(&p, static_cast<std::size_t>(align), size) != 0) {
    throw std::bad_alloc();
  }
#endif
  return p;
}
void* operator new[](std::size_t size, std::align_val_t align) {
  return ::operator new(size, align);
}
void operator delete(void* p) noexcept {
  count_if_audited();
  std::free(p);
}
void operator delete[](void* p) noexcept {
  ::operator delete(p);
}
void operator delete(void* p, std::size_t) noexcept {
  ::operator delete(p);
}
void operator delete[](void* p, std::size_t) noexcept {
  ::operator delete(p);
}
void operator delete(void* p, std::align_val_t) noexcept {
  count_if_audited();
#ifdef _WIN32
  _aligned_free(p);
#else
  std::free(p);
#endif
}
void operator delete[](void* p, std::align_val_t align) noexcept {
  ::operator delete(p, align);
}

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

namespace {

namespace pgae = prism::pgae;
namespace psv = prism::psv;

constexpr uint32_t kRate = 44'100;

pgae::SceneAssets synthetic_assets() {
  pgae::SceneAssets assets;
  assets.sample_rate = kRate;
  const size_t loops[pgae::kStemRoleCount] = {8'000, 6'000, 4'000, 5'000, 7'000};
  for (size_t i = 0; i < pgae::kStemRoleCount; ++i) {
    assets.stems[i].assign(loops[i], 0.25F);
  }
  return assets;
}

psv::RtStateVector rt_psv(int64_t seq) {
  psv::RtStateVector v;
  // Sweep the whole state space so density gates flip and every parameter moves.
  v.value[psv::kArousal] = static_cast<double>(seq % 11) / 10.0;
  v.confidence[psv::kArousal] = 0.9;
  v.value[psv::kCognitiveLoad] = static_cast<double>((seq * 3) % 11) / 10.0;
  v.confidence[psv::kCognitiveLoad] = 0.8;
  v.value[psv::kReadiness] = static_cast<double>((seq * 7) % 11) / 10.0;
  v.confidence[psv::kReadiness] = 0.7;
  v.sequence = seq;
  v.update_timestamp_ms = seq * 5'000;
  return v;
}

} // namespace

TEST(PgaeRtSafety, TenMinuteRenderRunAllocatesNothing) {
  pgae::Pgae engine;
  const pgae::SceneAssets assets = synthetic_assets();
  ASSERT_TRUE(engine.load_scene(&assets)); // pre-allocation happens HERE

  psv::RtExchange exchange;
  std::atomic<bool> stop_writer{false};

  // Inference-side writer: publishes a fresh snapshot continuously while the audio
  // thread runs — the realistic contention pattern for the handoff.
  std::thread writer([&] {
    int64_t seq = 0;
    while (!stop_writer.load(std::memory_order_acquire)) {
      exchange.publish(rt_psv(++seq));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  });

  constexpr uint64_t kTenMinutes = static_cast<uint64_t>(kRate) * 600;
  constexpr uint32_t kBlock = 512;
  std::vector<float> block(kBlock); // render buffer owned by the host, allocated up front

  uint64_t rendered = 0;
  uint64_t consumed = 0;
  const uint64_t allocs_before = g_rt_allocations.load();

  // The audited real-time path, exactly as the audio callback runs it:
  // poll → consume_psv(snapshot) → render.
  t_rt_audit = true;
  while (rendered < kTenMinutes) {
    psv::RtStateVector v;
    if (exchange.poll(v)) {
      engine.consume_psv(v);
      ++consumed;
    }
    engine.render(block.data(), kBlock);
    rendered += kBlock;
  }
  t_rt_audit = false;

  stop_writer.store(true, std::memory_order_release);
  writer.join();

  EXPECT_EQ(g_rt_allocations.load() - allocs_before, 0u)
      << "the render path allocated or freed memory";
  EXPECT_GE(rendered, kTenMinutes);
  EXPECT_GT(consumed, 100u) << "the handoff never delivered fresh snapshots";
}

TEST(PgaeRtSafety, SceneCrossfadeAllocatesNothingOnTheRenderPath) {
  // The crossfade is the one feature that puts a SECOND scene under the render path, and
  // the outgoing one has to go somewhere when the overlap ends. Releasing it on the audio
  // thread would be a real-time rule 1 violation, and the instrumented operator delete
  // above counts a release just as it counts an allocation — so retiring through
  // RetiredScenes rather than destroying in place is what this test pins.
  pgae::Pgae engine;
  const pgae::SceneAssets from = synthetic_assets();
  const pgae::SceneAssets to = synthetic_assets();
  ASSERT_TRUE(engine.load_scene(&from));

  constexpr uint32_t kBlock = 512;
  std::vector<float> block(kBlock); // host-owned, allocated up front
  for (int i = 0; i < 200; ++i) {
    engine.render(block.data(), kBlock); // settle before arming
  }

  ASSERT_TRUE(engine.begin_crossfade(&to, kRate / 2, /*align=*/false));

  const uint64_t allocs_before = g_rt_allocations.load();
  t_rt_audit = true;
  // Long enough to cover arming, the whole overlap, and the retirement at the end.
  for (int i = 0; i < 400; ++i) {
    engine.render(block.data(), kBlock);
  }
  t_rt_audit = false;

  EXPECT_EQ(g_rt_allocations.load() - allocs_before, 0u)
      << "the scene crossfade allocated or released memory on the render path";
  EXPECT_FALSE(engine.crossfade_active()) << "the overlap never completed";
  EXPECT_EQ(engine.collect_retired(), &from) << "the outgoing scene was not handed back";
}
