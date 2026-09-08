foreach(mode --run --run-hir --run-bytecode --run-adaptive-bytecode)
    set(extra_args)
    if(mode STREQUAL "--run-adaptive-bytecode")
        list(APPEND extra_args --adaptive-runs=1)
    endif()
    execute_process(
        COMMAND "${MPARSER}" "${mode}" ${extra_args} "${SAMPLE}"
        INPUT_FILE "${INPUT}"
        OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status
        TIMEOUT 15)
    if(NOT status EQUAL 0 OR NOT output MATCHES "summary = 32")
        message(FATAL_ERROR "${mode} input failed: ${status}\n${output}\n${error}")
    endif()
endforeach()

execute_process(
    COMMAND "${MPARSER}" --run --result-format=json-v1 "${SAMPLE}"
    OUTPUT_VARIABLE output ERROR_VARIABLE error RESULT_VARIABLE status
    TIMEOUT 15)
if(NOT status EQUAL 3)
    message(FATAL_ERROR "machine input must fail without blocking: ${status} ${error}")
endif()
string(JSON result_status GET "${output}" status)
string(JSON identifier GET "${output}" diagnostics 0 identifier)
if(NOT result_status STREQUAL "runtime-failed" OR
   NOT identifier STREQUAL "MParser:InputFailed")
    message(FATAL_ERROR "unexpected machine input result: ${output}")
endif()
