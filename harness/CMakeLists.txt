# Desktop harness. From Task 5 on it consumes ONLY the public C ABI (CLAUDE.md): it
# links prism::core, whose include surface is include/prism/prism_core.h alone — the
# internal module headers are not even on its include path, so a violation fails to
# compile. It doubles as the binding reference for dart:ffi / OEM hosts.
add_executable(prism_harness
  main.cpp)
target_link_libraries(prism_harness PRIVATE prism::core)
prism_set_warnings(prism_harness)
