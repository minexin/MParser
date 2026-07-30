cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_ASAN_RUNTIME_SOURCE
        MPARSER_RUNTIME_DIR
        MPARSER_EXECUTABLE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing MSVC AddressSanitizer variable: ${required_variable}")
    endif()
endforeach()

if(NOT EXISTS "${MPARSER_ASAN_RUNTIME_SOURCE}")
    message(FATAL_ERROR
        "MSVC AddressSanitizer source runtime is missing: "
        "${MPARSER_ASAN_RUNTIME_SOURCE}")
endif()
get_filename_component(
    mparser_asan_runtime_name
    "${MPARSER_ASAN_RUNTIME_SOURCE}" NAME)
set(mparser_staged_runtime
    "${MPARSER_RUNTIME_DIR}/${mparser_asan_runtime_name}")
if(NOT EXISTS "${mparser_staged_runtime}")
    message(FATAL_ERROR
        "MSVC AddressSanitizer runtime was not staged beside mparser: "
        "${mparser_staged_runtime}")
endif()

file(SHA256 "${MPARSER_ASAN_RUNTIME_SOURCE}" source_hash)
file(SHA256 "${mparser_staged_runtime}" staged_hash)
if(NOT source_hash STREQUAL staged_hash)
    message(FATAL_ERROR
        "Staged MSVC AddressSanitizer runtime hash does not match the "
        "compiler runtime")
endif()

execute_process(
    COMMAND "${MPARSER_EXECUTABLE}" --version
    RESULT_VARIABLE version_result
    OUTPUT_VARIABLE version_output
    ERROR_VARIABLE version_error)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR
        "ASan-instrumented mparser did not start (${version_result})\n"
        "stdout:\n${version_output}\n"
        "stderr:\n${version_error}")
endif()
if(NOT version_output MATCHES "^MParser [0-9]+\\.[0-9]+\\.[0-9]+")
    message(FATAL_ERROR
        "ASan-instrumented mparser returned an unexpected version: "
        "${version_output}")
endif()
