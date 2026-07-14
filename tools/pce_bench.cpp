// PCE tick benchmark (backs docs/perf-desktop.md; build plan: budgets are measured, not
// assumed). Lives in tools/ because it measures the internal engine directly — the
// harness may consume only the public C ABI (CLAUDE.md), which does not expose ticks.

#include "pce/pce.h"

#include <chrono>
#include <cstdio>

int main() {
  prism::pce::Pce engine;
  const int64_t t0 = 1'750'000'000'000;
  engine.report_task_deadlines({{t0 + 24 * 3'600'000, prism::pce::Priority::High}});
  engine.start(t0);
  // Worst case: a fully populated behavioral window.
  for (int i = 0; i < 24; ++i) {
    engine.report_idle(t0 + i * 5'000, (i % 3) * 700);
    if (i % 2 == 0) {
      engine.report_app_switch(t0 + i * 5'000);
    }
  }
  for (int i = 0; i < 1'000; ++i) {
    engine.evaluate(t0 + 120'000 + i); // warm-up
  }
  constexpr int kIters = 100'000;
  double total_us = 0.0;
  double max_us = 0.0;
  for (int i = 0; i < kIters; ++i) {
    const auto begin = std::chrono::steady_clock::now();
    engine.evaluate(t0 + 121'000 + static_cast<int64_t>(i) * 7);
    const auto end = std::chrono::steady_clock::now();
    const double us = std::chrono::duration<double, std::micro>(end - begin).count();
    total_us += us;
    if (us > max_us) {
      max_us = us;
    }
  }
  std::printf("PCE evaluate: avg %.3f us, max %.1f us over %d ticks (budget: < 1 ms)\n",
              total_us / kIters, max_us, kIters);
  return 0;
}
