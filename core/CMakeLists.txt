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
  # Turns PRISM_API into __declspec(dllexport) on Windows. Without it MinGW auto-exports
  # every symbol in the binary (1251, including prism::pce internals); with it the DLL's
  # surface is exactly the public header.
  target_compile_definitions(prism_core_shared PRIVATE PRISM_CORE_BUILD_SHARED)
  if(IOS)
    # Ship as a dynamic framework: embedded frameworks are signed by the app build, and
    # dart:ffi opens 'prism_core.framework/prism_core' by rpath.
    set_target_properties(prism_core_shared PROPERTIES
      FRAMEWORK TRUE
      FRAMEWORK_VERSION A
      MACOSX_FRAMEWORK_IDENTIFIER com.prism.core
      MACOSX_FRAMEWORK_BUNDLE_VERSION 0.1.0
      MACOSX_FRAMEWORK_SHORT_VERSION_STRING 0.1.0)
  endif()
  prism_set_warnings(prism_core_shared)
endif()
