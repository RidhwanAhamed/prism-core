#pragma once

// Scaffold-only surface. The ported heuristic (event ingress, behavioral
// window, deadline decay, fusion — constants verbatim from the probe) lands
// in Task 2.
namespace prism::pce {

const char* version() noexcept;

} // namespace prism::pce
