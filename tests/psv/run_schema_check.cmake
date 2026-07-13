# Pipes psv_emit_samples output into the python jsonschema validator.
#   cmake -DEMITTER=... -DPYTHON=... -DVALIDATOR=... -DSCHEMA=... -P run_schema_check.cmake
# (consecutive COMMANDs in execute_process are connected stdout -> stdin)

execute_process(
  COMMAND ${EMITTER}
  COMMAND ${PYTHON} ${VALIDATOR} ${SCHEMA}
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error_output)

if(NOT result EQUAL 0)
  message(FATAL_ERROR "PSV schema conformance failed:\n${output}\n${error_output}")
endif()

message(STATUS "${output}")
