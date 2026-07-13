# pgae — audio engine. Depends on psv only. Must never include or link pce.
add_library(prism_pgae STATIC
  src/version.cpp)
add_library(prism::pgae ALIAS prism_pgae)

target_include_directories(prism_pgae PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# miniaudio is PRIVATE: an implementation detail (device I/O, decode), never
# part of pgae's public surface.
target_link_libraries(prism_pgae
  PUBLIC prism::psv
  PRIVATE miniaudio)
prism_set_warnings(prism_pgae)
