#pragma once

// The real-time half of a prism_core handle, split into its own header so the render
// translation unit (render.cpp) never sees a locking primitive even by name — the
// rt_lock_primitives audit scans render.cpp's whole include closure. The full handle
// (mutexes, inference thread, device) lives in prism_core.cpp only.

#include "pgae/engine.h"
#include "psv/exchange.h"

#include <atomic>

struct PrismRt {
  prism::pgae::Pgae pgae;
  prism::psv::RtExchange exchange;
  std::atomic<bool> ready{false}; // set once a scene is loaded
};

// Defined in prism_core.cpp (the only file that knows the full handle layout).
PrismRt& prism_core_rt(prism_core& core);
