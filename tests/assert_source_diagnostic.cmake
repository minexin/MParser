if(NOT DEFINED MPARSER_EXECUTABLE OR NOT DEFINED ENTRY_SOURCE)
    message(FATAL_ERROR "missing source diagnostic test arguments")
endif()

execute_process(
    COMMAND "${MPARSER_EXECUTABLE}" --hir "${ENTRY_SOURCE}"
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
