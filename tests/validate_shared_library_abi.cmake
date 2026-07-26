cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_SHARED_LIBRARY
        MPARSER_SYMBOL_LIST
        MPARSER_ABI_SOVERSION)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "${required_variable} is required")
    endif()
endforeach()

if(NOT EXISTS "${MPARSER_SHARED_LIBRARY}")
    message(FATAL_ERROR
        "Shared library does not exist: ${MPARSER_SHARED_LIBRARY}")
endif()
if(NOT EXISTS "${MPARSER_SYMBOL_LIST}")
    message(FATAL_ERROR
        "ABI symbol list does not exist: ${MPARSER_SYMBOL_LIST}")
endif()

file(STRINGS "${MPARSER_SYMBOL_LIST}" expected_symbols)
list(FILTER expected_symbols EXCLUDE REGEX "^[ \t]*(#|$)")
list(REMOVE_DUPLICATES expected_symbols)
list(SORT expected_symbols)

set(actual_symbols)
if(WIN32)
    if(DEFINED MPARSER_LINKER AND EXISTS "${MPARSER_LINKER}")
        get_filename_component(
            mparser_tool_directory "${MPARSER_LINKER}" DIRECTORY)
    endif()
    find_program(mparser_dumpbin
        NAMES dumpbin
        HINTS "${mparser_tool_directory}")
    if(NOT mparser_dumpbin)
        message(FATAL_ERROR "dumpbin was not found")
    endif()
    execute_process(
        COMMAND "${mparser_dumpbin}" /nologo /exports
            "${MPARSER_SHARED_LIBRARY}"
        RESULT_VARIABLE tool_status
        OUTPUT_VARIABLE tool_output
        ERROR_VARIABLE tool_error)
elseif(APPLE)
    find_program(mparser_nm NAMES nm REQUIRED)
    execute_process(
        COMMAND "${mparser_nm}" -gU "${MPARSER_SHARED_LIBRARY}"
        RESULT_VARIABLE tool_status
        OUTPUT_VARIABLE tool_output
        ERROR_VARIABLE tool_error)
else()
    find_program(mparser_readelf NAMES readelf REQUIRED)
    execute_process(
        COMMAND "${mparser_readelf}" --dyn-syms --wide
            "${MPARSER_SHARED_LIBRARY}"
        RESULT_VARIABLE tool_status
        OUTPUT_VARIABLE tool_output
        ERROR_VARIABLE tool_error)
endif()

if(NOT tool_status EQUAL 0)
    message(FATAL_ERROR
        "Unable to inspect shared-library exports: ${tool_error}")
endif()

string(REPLACE "\r\n" "\n" tool_output "${tool_output}")
string(REPLACE "\n" ";" tool_lines "${tool_output}")
set(leaked_internal_symbols)
foreach(line IN LISTS tool_lines)
    if(WIN32 AND
       line MATCHES "[ \t](mparser_[A-Za-z0-9_]+)[ \t]*$")
        list(APPEND actual_symbols "${CMAKE_MATCH_1}")
    elseif(APPLE AND
           line MATCHES "[ \t]_?(mparser_[A-Za-z0-9_]+)[ \t]*$")
        list(APPEND actual_symbols "${CMAKE_MATCH_1}")
    elseif(UNIX AND
           line MATCHES "[ \t](mparser_[A-Za-z0-9_]+)[ \t]*$")
        list(APPEND actual_symbols "${CMAKE_MATCH_1}")
    endif()
    if(line MATCHES
       "[ \t](_?sljit_[A-Za-z0-9_]+|_*ZNK?7mparser[A-Za-z0-9_]+)[ \t]*$")
        list(APPEND leaked_internal_symbols "${CMAKE_MATCH_1}")
    endif()
endforeach()
list(REMOVE_DUPLICATES actual_symbols)
list(SORT actual_symbols)

set(missing_symbols)
foreach(symbol IN LISTS expected_symbols)
    if(NOT symbol IN_LIST actual_symbols)
        list(APPEND missing_symbols "${symbol}")
    endif()
endforeach()

set(unexpected_symbols)
foreach(symbol IN LISTS actual_symbols)
    if(NOT symbol IN_LIST expected_symbols)
        list(APPEND unexpected_symbols "${symbol}")
    endif()
endforeach()

if(missing_symbols OR unexpected_symbols OR leaked_internal_symbols)
    message(FATAL_ERROR
        "C ABI export mismatch.\n"
        "Missing: ${missing_symbols}\n"
        "Unexpected: ${unexpected_symbols}\n"
        "Leaked internal symbols: ${leaked_internal_symbols}")
endif()

if(APPLE)
    find_program(mparser_otool NAMES otool REQUIRED)
    execute_process(
        COMMAND "${mparser_otool}" -D "${MPARSER_SHARED_LIBRARY}"
        RESULT_VARIABLE identity_status
        OUTPUT_VARIABLE identity_output
        ERROR_VARIABLE identity_error)
    if(NOT identity_status EQUAL 0)
        message(FATAL_ERROR
            "Unable to inspect install name: ${identity_error}")
    endif()
    if(NOT identity_output MATCHES
       "libmparser_c\\.${MPARSER_ABI_SOVERSION}\\.dylib")
        message(FATAL_ERROR
            "Unexpected macOS install name: ${identity_output}")
    endif()
elseif(UNIX)
    execute_process(
        COMMAND "${mparser_readelf}" -d "${MPARSER_SHARED_LIBRARY}"
        RESULT_VARIABLE identity_status
        OUTPUT_VARIABLE identity_output
        ERROR_VARIABLE identity_error)
    if(NOT identity_status EQUAL 0)
        message(FATAL_ERROR
            "Unable to inspect SONAME: ${identity_error}")
    endif()
    if(NOT identity_output MATCHES
       "SONAME.*libmparser_c\\.so\\.${MPARSER_ABI_SOVERSION}")
        message(FATAL_ERROR
            "Unexpected ELF SONAME: ${identity_output}")
    endif()
endif()

list(LENGTH actual_symbols symbol_count)
message(STATUS
    "MParser shared C ABI validated: ${symbol_count} symbols, "
    "SOVERSION ${MPARSER_ABI_SOVERSION}")
