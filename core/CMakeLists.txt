# core — the C ABI (include/prism/prism_core.h) and its implementation: the composition
# root that wires PCE → RtExchange → PGAE behind the opaque handle. The ONLY target that
# links both pce and pgae; even here the PSV snapshot is the only thing crossing between
# them. Internal module headers are PRIVATE: consumers of prism_core see nothing but the
# public header — the same boundary dart:ffi and OEM hosts get.
add_library(prism_core_lib STATIC
  src/prism_core.cpp
  src/render.cpp)
add_library(prism::core ALIAS prism_core_lib)

target_include_directories(prism_core_lib PUBLIC ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(prism_core_lib PRIVATE
  prism::pce
  prism::pgae
  prism::psv
  miniaudio)
prism_set_warnings(prism_core_lib)

# The loadable form of the same ABI — what dart:ffi / OEM hosts dlopen. Exports are the
# PRISM_API functions only (global hidden-visibility default; verified by nm in review).
option(PRISM_BUILD_SHARED "Build libprism_core as a shared library (Android/FFI hosts)" OFF)
if(PRISM_BUILD_SHARED)
  add_library(prism_core_shared SHARED
    src/prism_core.cpp
    src/render.cpp)
  target_include_directories(prism_core_shared PUBLIC ${PROJECT_SOURCE_DIR}/include)
  target_link_libraries(prism_core_shared PRIVATE
    prism::pce
    prism::pgae
    prism::psv
    miniaudio)
  set_target_properties(prism_core_shared PROPERTIES OUTPUT_NAME prism_core)
  prism_set_warnings(prism_core_shared)
endif()
