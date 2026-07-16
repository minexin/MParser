if(NOT DEFINED MPARSER_EXECUTABLE OR NOT DEFINED ENTRY_SOURCE)
    message(FATAL_ERROR "missing source diagnostic test arguments")
endif()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_EXECUTABLE}")

execute_process(
    COMMAND ${mparser_command} --hir "${ENTRY_SOURCE}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error)

if(result EQUAL 0)
    message(FATAL_ERROR "malformed loaded class unexpectedly compiled")
endif()

set(combined "${output}\n${error}")
if(NOT combined MATCHES "BrokenClass\\.m:[0-9]+:[0-9]+:")
    message(FATAL_ERROR
        "loaded-class diagnostic did not contain a file location:\n${combined}")
endif()
