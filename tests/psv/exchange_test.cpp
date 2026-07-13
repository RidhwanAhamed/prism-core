#include <gtest/gtest.h>

#include "psv/exchange.h"
#include "psv/rt.h"

#include <atomic>
#include <cstring>
#include <thread>

namespace psv = prism::psv;

// The inference→audio handoff (CLAUDE.md real-time rule 2): single writer, single reader,
// wait-free, and — the property everything hangs on — a reader NEVER observes a torn
// snapshot. Every field of a published snapshot is derived from its sequence number, so
// any mix of two snapshots is detectable.

namespace {

psv::RtStateVector snapshot_for(int64_t seq) {
  psv::RtStateVector v;
  for (int d = 0; d < psv::kDimensionCount; ++d) {
    // Values derived from seq, kept in [0,1] so they are realistic.
    v.value[d] = static_cast<double>((seq + d) % 1000) / 1000.0;
    v.confidence[d] = static_cast<double>((seq * 7 + d) % 1000) / 1000.0;
  }
  v.update_timestamp_ms = seq * 1000;
  v.sequence = seq;
  return v;
}

bool coherent(const psv::RtStateVector& v) {
  const psv::RtStateVector expected = snapshot_for(v.sequence);
  return std::memcmp(&expected, &v, sizeof v) == 0;
}

} // namespace

TEST(RtExchange, SingleThreadedSemantics) {
  psv::RtExchange exchange;
  psv::RtStateVector out;
  EXPECT_FALSE(exchange.poll(out)); // nothing published yet

  exchange.publish(snapshot_for(1));
  ASSERT_TRUE(exchange.poll(out));
  EXPECT_EQ(out.sequence, 1);
  EXPECT_FALSE(exchange.poll(out)); // same snapshot is not re-delivered

  exchange.publish(snapshot_for(2));
  exchange.publish(snapshot_for(3)); // reader was slow: older snapshot is superseded
  ASSERT_TRUE(exchange.poll(out));
  EXPECT_EQ(out.sequence, 3);
  EXPECT_FALSE(exchange.poll(out));
}

TEST(RtExchange, ConcurrentStressNeverTearsAndNeverGoesBackwards) {
  psv::RtExchange exchange;
  constexpr int64_t kPublishes = 400'000;
  std::atomic<bool> writer_done{false};

  std::thread writer([&] {
    for (int64_t i = 1; i <= kPublishes; ++i) {
      exchange.publish(snapshot_for(i));
    }
    writer_done.store(true, std::memory_order_release);
  });

  int64_t last_seq = 0;
  int64_t observed = 0;
  bool torn = false;
  bool backwards = false;
  for (;;) {
    psv::RtStateVector v;
    if (exchange.poll(v)) {
      ++observed;
      if (!coherent(v)) {
        torn = true;
        break;
      }
      if (v.sequence <= last_seq) {
        backwards = true;
        break;
      }
      last_seq = v.sequence;
    } else if (writer_done.load(std::memory_order_acquire)) {
      break;
    }
  }
  writer.join();

  EXPECT_FALSE(torn) << "reader observed a torn snapshot";
  EXPECT_FALSE(backwards) << "reader observed an older snapshot after a newer one";
  EXPECT_GT(observed, 0);
  // Drain: the final publish must be observable.
  psv::RtStateVector v;
  if (last_seq != kPublishes) {
    ASSERT_TRUE(exchange.poll(v));
    EXPECT_TRUE(coherent(v));
    EXPECT_EQ(v.sequence, kPublishes);
  }
}

TEST(RtSnapshot, RoundTripsThroughStateVector) {
  auto v = psv::neutral(psv::Vertical::Venues, 1'750'000'000'000);
  v.sequence = 42;
  v.mode_hint = psv::mode_hint::kVenueEnergize;
  v.arousal = {0.72, 0.81};
  const psv::StateVector back = psv::from_rt(psv::to_rt(v));
  EXPECT_EQ(back, v);

  // Null hint and absent sequence survive too.
  const auto plain = psv::neutral(psv::Vertical::Aqademiq, 5);
  EXPECT_EQ(psv::from_rt(psv::to_rt(plain)), plain);

  // Overlong unknown hints truncate to the buffer bound, always NUL-terminated.
  v.mode_hint = std::string(64, 'x');
  const auto truncated = psv::from_rt(psv::to_rt(v));
  EXPECT_EQ(truncated.mode_hint, std::string(23, 'x'));
}
