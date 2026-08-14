cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_CLI HOST_SOURCE MPARSER_NATIVE_JIT)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing host-integration input: ${required_variable}")
    endif()
endforeach()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_CLI}")

set(expected_output
    "value=42\n\npi=3.1\nans = 42\n\nVariables:\n  ans = 43\n  formatted = 'value=42'\n  written = 7\n")

function(run_host_case name)
    execute_process(
        COMMAND ${mparser_command} ${ARGN} "${HOST_SOURCE}"
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE actual_output
        ERROR_VARIABLE actual_error)
    string(REPLACE "\r\n" "\n" actual_output "${actual_output}")
    if(NOT actual_exit EQUAL 0 OR
       NOT actual_error STREQUAL "" OR
       NOT actual_output STREQUAL expected_output)
        message(FATAL_ERROR
            "${name}: host integration output mismatch\n"
            "exit: ${actual_exit}\nstdout:\n${actual_output}\n"
            "stderr:\n${actual_error}")
    endif()
endfunction()

run_host_case(interpreter --run-hir)
run_host_case(bytecode --run --jit=off)
run_host_case(portable --run --jit=portable)
if(MPARSER_NATIVE_JIT)
    run_host_case(native --run --jit=native)
endif()

message(STATUS
    "MParser host integration CLI validated: interpreter, bytecode, "
    "portable, native=${MPARSER_NATIVE_JIT}")
