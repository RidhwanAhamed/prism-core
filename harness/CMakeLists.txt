# Desktop harness. Through Task 4 it may link internal targets; from Task 5 it
# is rebuilt on top of the public C ABI only (CLAUDE.md).
add_executable(prism_harness
  main.cpp)
target_link_libraries(prism_harness PRIVATE
  prism::pgae
  prism::psv
  miniaudio)
prism_set_warnings(prism_harness)
