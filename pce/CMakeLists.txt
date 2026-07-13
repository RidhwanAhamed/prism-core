# pce — state inference. Depends on psv only. Never links pgae.
add_library(prism_pce STATIC
  src/attention.cpp
  src/circadian.cpp
  src/deadline.cpp
  src/fusion.cpp
  src/pce.cpp
  src/version.cpp)
add_library(prism::pce ALIAS prism_pce)

target_include_directories(prism_pce PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(prism_pce PUBLIC prism::psv)
prism_set_warnings(prism_pce)
