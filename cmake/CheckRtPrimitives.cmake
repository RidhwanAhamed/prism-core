# Render-path lock/blocking audit, run as a ctest:
#   cmake -DROOT=<repo root> -P CheckRtPrimitives.cmake
#
# The dynamic half of real-time rule 1 is the instrumented-allocator test
# (PgaeRtSafety.TenMinuteRenderRunAllocatesNothing). This is the static half: the sources
# that make up the render path and the PSV handoff must never name a locking, blocking,
# logging, or file-I/O primitive. Scene loading (scene.cpp) is deliberately NOT listed —
# it does I/O by design, before start.

if(NOT DEFINED ROOT)
  message(FATAL_ERROR "Pass -DROOT=<repo root>")
endif()

set(render_path_sources
  "pgae/include/pgae/engine.h"
  "pgae/include/pgae/mapping.h"
  "pgae/include/pgae/fade.h"
  "pgae/include/pgae/detail/dsp.h"
  "pgae/src/engine.cpp"
  "pgae/src/mapping.cpp"
  "psv/include/psv/rt.h"
  "psv/include/psv/exchange.h")

set(forbidden
  "std::mutex" "pthread_" "condition_variable" "lock_guard" "unique_lock" "scoped_lock"
  "shared_lock" "semaphore" "this_thread" "sleep" "printf" "fprintf" "std::cout"
  "std::cerr" "syslog" "os_log" "fopen" "ifstream" "ofstream" "fstream")

set(violations "")
foreach(source IN LISTS render_path_sources)
  set(path "${ROOT}/${source}")
  if(NOT EXISTS "${path}")
    list(APPEND violations "  ${source}: file missing (audit list is stale)")
    continue()
  endif()
  file(READ "${path}" content)
  foreach(primitive IN LISTS forbidden)
    string(FIND "${content}" "${primitive}" at)
    if(NOT at EQUAL -1)
      list(APPEND violations "  ${source} names \"${primitive}\"")
    endif()
  endforeach()
endforeach()

if(violations)
  list(JOIN violations "\n" violation_text)
  message(FATAL_ERROR "Render-path sources name locking/blocking/logging primitives:\n${violation_text}")
endif()

message(STATUS "RT primitive audit passed: render-path sources are lock/log/IO-free.")
