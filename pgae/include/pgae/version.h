#pragma once

// Scaffold-only surface. The ported audio engine (stem player, mapping,
// smoothing, crossfades, limiter — per consumption spec §11) lands in Task 3.
namespace prism::pgae {

const char* version() noexcept;

} // namespace prism::pgae
