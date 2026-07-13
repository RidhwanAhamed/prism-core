include(FetchContent)
FetchContent_Declare(googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.15.2
  GIT_SHALLOW TRUE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

add_executable(prism_unit_tests
  unit/smoke_test.cpp)
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
