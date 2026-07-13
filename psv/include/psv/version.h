#pragma once

// Scaffold-only surface. The real PSV types (four {value, confidence}
// dimensions, metadata, neutral vector, JSON round-trip) land in Task 1.
namespace prism::psv {

const char* version() noexcept;

} // namespace prism::psv
