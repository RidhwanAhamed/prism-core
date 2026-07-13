# psv — the contract: PSV types, validation, serialization. Depends on nothing.
add_library(prism_psv STATIC
  src/json.cpp
  src/json_value.cpp
  src/psv.cpp
  src/validate.cpp
  src/version.cpp)
add_library(prism::psv ALIAS prism_psv)

target_include_directories(prism_psv PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
prism_set_warnings(prism_psv)
