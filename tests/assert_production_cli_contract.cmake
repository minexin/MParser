if(NOT DEFINED MPARSER_EXECUTABLE OR NOT DEFINED ENTRY_SOURCE)
    message(FATAL_ERROR "missing production CLI contract test arguments")
endif()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_EXECUTABLE}")

function(assert_cli_failure expected expected_exit)
    execute_process(
        COMMAND ${mparser_command} ${ARGN}
        RESULT_VARIABLE result
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error)
    if(NOT "${result}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR
            "CLI command expected exit ${expected_exit}, got ${result}: "
            "${ARGN}\n${output}${error}")
    endif()
    set(combined "${output}\n${error}")
    if(NOT combined MATCHES "${expected}")
        message(FATAL_ERROR
            "CLI diagnostic did not match '${expected}':\n${combined}")
    endif()
endfunction()

assert_cli_failure("unknown option: --unknown" 2
    --run --unknown "${ENTRY_SOURCE}")
assert_cli_failure("choose only one execution or inspection mode" 2
    --run --run-hir "${ENTRY_SOURCE}")
assert_cli_failure("--jit is only valid with the production --run mode" 2
    --run-hir --jit=portable "${ENTRY_SOURCE}")
assert_cli_failure("--typed-backend is only valid with" 2
    --run --typed-backend=portable "${ENTRY_SOURCE}")
assert_cli_failure("--entry-function is only valid with" 2
    --run-hir --entry-function=main "${ENTRY_SOURCE}")
assert_cli_failure("--argument is only valid with" 2
    --run-hir --argument=1 "${ENTRY_SOURCE}")
assert_cli_failure("--outputs is only valid with" 2
    --run-hir --outputs=1 "${ENTRY_SOURCE}")
assert_cli_failure("option may be specified only once: --run" 2
    --run --run "${ENTRY_SOURCE}")
assert_cli_failure("option may be specified only once: --jit" 2
    --run --jit=off --jit=portable "${ENTRY_SOURCE}")
assert_cli_failure("unknown option: --run-interpreter" 2
    --run-interpreter "${ENTRY_SOURCE}")
assert_cli_failure("--benchmark-warmup is only valid with" 2
    --run --benchmark-warmup=1 "${ENTRY_SOURCE}")
assert_cli_failure("--adaptive-runs is only valid with" 2
    --run --adaptive-runs=2 "${ENTRY_SOURCE}")
assert_cli_failure("--module-call is only valid with" 2
    --run --module-call=main "${ENTRY_SOURCE}")
assert_cli_failure("--run-module-runtime requires at least one" 2
    --run-module-runtime "${ENTRY_SOURCE}")
assert_cli_failure("--native-cache-stats is only valid with" 2
    --native-cache-stats "${ENTRY_SOURCE}")
assert_cli_failure("--path/--class-path is only valid with" 2
    --tokens --path=. "${ENTRY_SOURCE}")

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
