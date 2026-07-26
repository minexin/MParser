cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        LICENSE_FILE
        NOTICE_FILE
        THIRD_PARTY_NOTICES_FILE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "${required_variable} is required")
    endif()
    if(NOT EXISTS "${${required_variable}}")
        message(FATAL_ERROR
            "${required_variable} does not exist: "
            "${${required_variable}}")
    endif()
endforeach()

set(apache_2_license_sha256
    "cfc7749b96f63bd31c3c42b5c471bf756814053e847c10f3eb003417bc523d30")
file(READ "${LICENSE_FILE}" license_text)
string(REPLACE "\r\n" "\n" license_text "${license_text}")
string(SHA256 actual_license_sha256 "${license_text}")
if(NOT actual_license_sha256 STREQUAL apache_2_license_sha256)
    message(FATAL_ERROR
        "LICENSE does not match the official Apache-2.0 text: "
        "${actual_license_sha256}")
endif()

file(READ "${NOTICE_FILE}" project_notice)
foreach(required_notice IN ITEMS
        "MParser"
        "Copyright 2026 Wang Xin"
        "THIRD_PARTY_NOTICES.md")
    string(FIND "${project_notice}" "${required_notice}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR
            "NOTICE is missing: ${required_notice}")
    endif()
endforeach()

file(READ "${THIRD_PARTY_NOTICES_FILE}" third_party_notices)
foreach(required_notice IN ITEMS
        "SLJIT"
        "Simplified BSD"
        "Copyright Zoltan Herczeg")
    string(FIND
        "${third_party_notices}" "${required_notice}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR
            "THIRD_PARTY_NOTICES.md is missing: ${required_notice}")
    endif()
endforeach()

message(STATUS
    "MParser project license validated: "
    "Apache-2.0, Copyright 2026 Wang Xin")
