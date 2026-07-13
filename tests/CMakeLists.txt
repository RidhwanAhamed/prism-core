include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(prism_unit_tests
  pce/parity_test.cpp
  pce/pce_test.cpp
  psv/json_test.cpp
  psv/psv_test.cpp
  unit/smoke_test.cpp)

# The parity test reads golden-trace JSONL via psv's internal JSON value parser. Tests may
# reach into a module's src/ (the firewall governs the psv/pce/pgae modules, not tests);
# nothing outside tests/ may do this.
target_include_directories(prism_unit_tests PRIVATE ${PROJECT_SOURCE_DIR}/psv/src)
target_compile_definitions(prism_unit_tests PRIVATE
  PRISM_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden")
# Links all three modules: green tests prove the target graph resolves.
target_link_libraries(prism_unit_tests PRIVATE
  prism::psv
  prism::pce
  prism::pgae
  GTest::gtest_main)
prism_set_warnings(prism_unit_tests)

include(GoogleTest)
gtest_discover_tests(prism_unit_tests)

# The firewall as a test: fails if any module includes across the boundary.
add_test(NAME firewall_includes
  COMMAND ${CMAKE_COMMAND} -DROOT=${PROJECT_SOURCE_DIR}
          -P ${PROJECT_SOURCE_DIR}/cmake/CheckFirewall.cmake)

# Independent schema conformance: emitted PSVs validated against the spec's
# own JSON Schema by python-jsonschema. Registered only where python3 with
# the jsonschema module exists (dev machines, CI — which installs it).
add_executable(psv_emit_samples psv/emit_samples.cpp)
target_link_libraries(psv_emit_samples PRIVATE prism::psv)
prism_set_warnings(psv_emit_samples)

find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_FOUND)
  execute_process(COMMAND ${Python3_EXECUTABLE} -c "import jsonschema"
    RESULT_VARIABLE _psv_no_jsonschema OUTPUT_QUIET ERROR_QUIET)
  if(_psv_no_jsonschema EQUAL 0)
    add_test(NAME psv_schema_conformance
      COMMAND ${CMAKE_COMMAND}
        "-DEMITTER=$<TARGET_FILE:psv_emit_samples>"
        "-DPYTHON=${Python3_EXECUTABLE}"
        "-DVALIDATOR=${CMAKE_CURRENT_SOURCE_DIR}/psv/validate_samples.py"
        "-DSCHEMA=${CMAKE_CURRENT_SOURCE_DIR}/psv/psv.schema.json"
        -P ${CMAKE_CURRENT_SOURCE_DIR}/psv/run_schema_check.cmake)
  else()
    message(STATUS "psv_schema_conformance test skipped: python jsonschema module not found")
  endif()
endif()
