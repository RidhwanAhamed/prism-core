# Developer tools. Unlike the harness, tools may link internal targets (they measure or
# poke internals by design and are not shipped).
add_executable(pce_bench pce_bench.cpp)
target_link_libraries(pce_bench PRIVATE prism::pce)
prism_set_warnings(pce_bench)
