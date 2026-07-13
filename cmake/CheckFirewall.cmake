# Firewall include check, run as a ctest:
#   cmake -DROOT=<repo root> -P CheckFirewall.cmake
#
# The CMake target graph already makes cross-module includes fail to compile
# (the include paths aren't visible). This script catches the remaining holes:
#   1. raw relative includes (#include "../pce/...") that bypass target include
#      directories — in ANY file under a module, whatever its extension, since
#      the preprocessor doesn't care about extensions (.inl, .ipp, .tpp, ...);
#   2. macro-indirected includes (#define X "..." + #include X), which can't be
#      pattern-checked, so they are banned outright in module code.
# Rules mirror CLAUDE.md: the PSV is the only thing that crosses.

if(NOT DEFINED ROOT)
  message(FATAL_ERROR "Pass -DROOT=<repo root>")
endif()

# module:forbidden-module pairs
set(rules
  "pgae:pce"
  "pce:pgae"
  "psv:pce"
  "psv:pgae")

set(modules psv pce pgae)
set(violations "")

foreach(rule IN LISTS rules)
  string(REPLACE ":" ";" pair "${rule}")
  list(GET pair 0 module)
  list(GET pair 1 forbidden)

  file(GLOB_RECURSE files LIST_DIRECTORIES false "${ROOT}/${module}/*")

  foreach(f IN LISTS files)
    file(READ "${f}" content)
    if(content MATCHES "#[ \t]*include[ \t]*[\"<][^\"<>]*${forbidden}/")
      list(APPEND violations "  ${f} includes from ${forbidden}/")
    endif()
  endforeach()
endforeach()

# Macro-indirected includes are banned in module code: they evade the check
# above, and this codebase never needs them. First non-whitespace char after
# #include must be a quote or angle bracket.
foreach(module IN LISTS modules)
  file(GLOB_RECURSE files LIST_DIRECTORIES false "${ROOT}/${module}/*")
  foreach(f IN LISTS files)
    file(READ "${f}" content)
    if(content MATCHES "#[ \t]*include[ \t]+[^\"< \t\r\n]")
      list(APPEND violations "  ${f} uses a macro-indirected #include (banned: evades the firewall check — use a literal path)")
    endif()
  endforeach()
endforeach()

if(violations)
  list(JOIN violations "\n" violation_text)
  message(FATAL_ERROR "Structural firewall violation — the PSV is the only thing that crosses:\n${violation_text}")
endif()

message(STATUS "Firewall check passed: no cross-module includes between psv/pce/pgae.")
