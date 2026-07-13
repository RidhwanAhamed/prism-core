# Firewall include check, run as a ctest:
#   cmake -DROOT=<repo root> -P CheckFirewall.cmake
#
# The CMake target graph already makes cross-module includes fail to compile
# (the include paths aren't visible). This script catches the remaining hole:
# raw relative includes (#include "../pce/...") that bypass target include
# directories. Rules mirror CLAUDE.md: the PSV is the only thing that crosses.

if(NOT DEFINED ROOT)
  message(FATAL_ERROR "Pass -DROOT=<repo root>")
endif()

# module:forbidden-module pairs
set(rules
  "pgae:pce"
  "pce:pgae"
  "psv:pce"
  "psv:pgae")

set(violations "")

foreach(rule IN LISTS rules)
  string(REPLACE ":" ";" pair "${rule}")
  list(GET pair 0 module)
  list(GET pair 1 forbidden)

  file(GLOB_RECURSE files
    "${ROOT}/${module}/*.h"
    "${ROOT}/${module}/*.hpp"
    "${ROOT}/${module}/*.c"
    "${ROOT}/${module}/*.cpp")

  foreach(f IN LISTS files)
    file(READ "${f}" content)
    if(content MATCHES "#[ \t]*include[ \t]*[\"<][^\"<>]*${forbidden}/")
      list(APPEND violations "  ${f} includes from ${forbidden}/")
    endif()
  endforeach()
endforeach()

if(violations)
  list(JOIN violations "\n" violation_text)
  message(FATAL_ERROR "Structural firewall violation — the PSV is the only thing that crosses:\n${violation_text}")
endif()

message(STATUS "Firewall check passed: no cross-module includes between psv/pce/pgae.")
