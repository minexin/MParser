if(NOT DEFINED MPARSER_EXECUTABLE OR NOT DEFINED ENTRY_SOURCE)
    message(FATAL_ERROR "missing production CLI contract test arguments")
endif()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_EXECUTABLE}")

function(assert_cli_failure expected)
    execute_process(
        COMMAND ${mparser_command} ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(result EQUAL 0)
        message(FATAL_ERROR
            "CLI command unexpectedly succeeded: ${ARGN}\n${output}${error}")
    endif()
    set(combined "${output}\n${error}")
    if(NOT combined MATCHES "${expected}")
        message(FATAL_ERROR
            "CLI diagnostic did not match '${expected}':\n${combined}")
    endif()
endfunction()

assert_cli_failure("unknown option: --unknown"
    --run --unknown "${ENTRY_SOURCE}")
assert_cli_failure("choose only one execution or inspection mode"
    --run --run-hir "${ENTRY_SOURCE}")
assert_cli_failure("--jit is only valid with the production --run mode"
    --run-hir --jit=portable "${ENTRY_SOURCE}")

execute_process(
    COMMAND ${mparser_command} --run --jit=off -- "${ENTRY_SOURCE}"
    RESULT_VARIABLE separator_result
    OUTPUT_VARIABLE separator_output
    ERROR_VARIABLE separator_error)
if(NOT separator_result EQUAL 0 OR
   NOT separator_output MATCHES "summary = 705")
    message(FATAL_ERROR
        "-- source separator did not preserve production execution:\n"
        "${separator_output}${separator_error}")
endif()
