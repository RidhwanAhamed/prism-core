cmake_minimum_required(VERSION 3.24)

project(prism_core
  VERSION 0.1.0
  DESCRIPTION "Prism Engine core: PCE + PGAE behind a stable C ABI"
  LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(PRISM_BUILD_TESTS "Build unit tests" ON)
option(PRISM_BUILD_HARNESS "Build the desktop harness" ON)
option(PRISM_SANITIZE "Enable Address + UB sanitizers" OFF)

if(PRISM_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

include(cmake/CompilerWarnings.cmake)

add_subdirectory(third_party)

# The firewall, encoded as targets: psv depends on nothing; pce and pgae link
# psv and nothing else from this repo. pgae never links pce (and vice versa) —
# module headers are only reachable through their targets, so the include paths
# physically don't exist for a violating #include. tests/ adds a ctest that
# additionally greps for raw relative includes.
add_subdirectory(psv)
add_subdirectory(pce)
add_subdirectory(pgae)
add_subdirectory(core)
add_subdirectory(tools)

if(PRISM_BUILD_HARNESS)
  add_subdirectory(harness)
endif()

if(PRISM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
