# Render-path lock/blocking audit, run as a ctest:
#   cmake -DROOT=<repo root> -DCXX=<c++ compiler> -P CheckRtPrimitives.cmake
#
# The dynamic half of real-time rule 1 is the instrumented-allocator test
# (PgaeRtSafety.TenMinuteRenderRunAllocatesNothing) — it covers the C++ operator
# new/delete family. This static half token-scans the render path for locking, blocking,
# logging, file-I/O, and C-allocator primitives.
#
# The audited file set is the TRANSITIVE INCLUDE CLOSURE of the render-path translation
# units, computed by the real compiler (-MM), plus the handoff header the audio thread
# polls. Adversarial review proved a hand-maintained file list is a hole: a mutex in a
# header the render path includes (but the list missed) shipped green.

if(NOT DEFINED ROOT OR NOT DEFINED CXX)
  message(FATAL_ERROR "Pass -DROOT=<repo root> -DCXX=<c++ compiler>")
endif()

set(flags -std=c++17 "-I${ROOT}/pgae/include" "-I${ROOT}/psv/include" "-I${ROOT}/include")

# Render-path TUs; exchange.h is polled by the audio thread but not included by pgae TUs,
# so its closure is computed separately (as a standalone C++ header).
set(closure_output "")
execute_process(
  COMMAND ${CXX} ${flags} -MM "${ROOT}/pgae/src/engine.cpp" "${ROOT}/pgae/src/mapping.cpp"
          "${ROOT}/core/src/render.cpp"
  OUTPUT_VARIABLE tu_deps
  ERROR_VARIABLE tu_err
  RESULT_VARIABLE tu_rc)
if(NOT tu_rc EQUAL 0)
  message(FATAL_ERROR "closure computation failed:\n${tu_err}")
endif()
execute_process(
  COMMAND ${CXX} ${flags} -x c++ -MM "${ROOT}/psv/include/psv/exchange.h"
  OUTPUT_VARIABLE hdr_deps
  ERROR_VARIABLE hdr_err
  RESULT_VARIABLE hdr_rc)
if(NOT hdr_rc EQUAL 0)
  message(FATAL_ERROR "closure computation failed:\n${hdr_err}")
endif()

# Parse "-MM" output ("target.o: dep dep \\\n dep ...") into repo-relative dep paths.
# Only object-file targets are stripped: a bare "[^ \t\n]+:" also eats the "D:" of a
# Windows drive-letter path, which drops every dep out of the closure.
string(REPLACE "\\\n" " " all_deps "${tu_deps} ${hdr_deps}")
string(REGEX REPLACE "[^ \t\n]+\\.o(bj)?:" " " all_deps "${all_deps}")
separate_arguments(dep_items UNIX_COMMAND "${all_deps}")

set(audited "")
foreach(item IN LISTS dep_items)
  if(NOT IS_ABSOLUTE "${item}")
    set(item "${ROOT}/${item}")
  endif()
  string(FIND "${item}" "${ROOT}/" under_root)
  string(FIND "${item}" "third_party" is_third_party)
  if(under_root EQUAL 0 AND is_third_party EQUAL -1 AND EXISTS "${item}")
    list(APPEND audited "${item}")
  endif()
endforeach()
list(APPEND audited "${ROOT}/pgae/src/engine.cpp" "${ROOT}/pgae/src/mapping.cpp"
  "${ROOT}/core/src/render.cpp")
list(REMOVE_DUPLICATES audited)
list(LENGTH audited audited_count)
if(audited_count LESS 8)
  message(FATAL_ERROR "closure suspiciously small (${audited_count} files) — audit broken?")
endif()

# Forbidden tokens. Locking includes PLATFORM primitives (os_unfair_lock, GCD, Win32,
# futex, atomic waits) — the std:: names alone were another proven hole. C allocators are
# listed because the dynamic test can only interpose the C++ operator new family.
set(forbidden
  # locks / waits
  "std::mutex" "pthread_" "condition_variable" "lock_guard" "unique_lock" "scoped_lock"
  "shared_lock" "semaphore" "os_unfair_lock" "dispatch_" "CRITICAL_SECTION" "SRWLOCK"
  "AcquireSRW" "WaitForSingleObject" "futex" ".wait(" "atomic_wait"
  # blocking / scheduling
  "this_thread" "sleep"
  # logging / IO
  "printf" "fprintf" "std::cout" "std::cerr" "syslog" "os_log" "fopen" "ifstream"
  "ofstream" "fstream"
  # C allocators (operator new/delete are covered dynamically)
  "malloc(" "calloc(" "realloc(" "free(")

set(violations "")
foreach(path IN LISTS audited)
  file(READ "${path}" content)
  foreach(primitive IN LISTS forbidden)
    string(FIND "${content}" "${primitive}" at)
    if(NOT at EQUAL -1)
      string(REPLACE "${ROOT}/" "" rel "${path}")
      list(APPEND violations "  ${rel} names \"${primitive}\"")
    endif()
  endforeach()
endforeach()

if(violations)
  list(JOIN violations "\n" violation_text)
  message(FATAL_ERROR "Render-path closure names locking/blocking/logging/alloc primitives:\n${violation_text}")
endif()

message(STATUS "RT primitive audit passed: ${audited_count} files in the render-path closure are clean.")
