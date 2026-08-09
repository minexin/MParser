cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        PROJECT_ROOT
        MATRIX
        PUBLIC_CONTRACT
        RELEASE_NOTES
        ROADMAP
        JIT_SCOPE_VALIDATOR
        JIT_SCOPE_DECISION
        NATIVE_SCALAR_REPORT
        NATIVE_ARRAY_REPORT
        NOJIT_ARRAY_REPORT
        AUTHENTICATION_EVIDENCE_VALIDATOR
        AUTHENTICATION_EVIDENCE_ROOT
        PUBLICATION_EVIDENCE_VALIDATOR
        PUBLICATION_EVIDENCE_ROOT
        EXPECTED_VERSION
        EXPECTED_RELEASE_VERSION
        EXPECTED_PUBLIC_CONTRACT_VERSION
        EXPECTED_JIT_EVIDENCE_VERSION
        EXPECTED_AUTHENTICATION_VERSION
        EXPECTED_AUTHENTICATION_REVISION
        EXPECTED_AUTHENTICATION_RUN_ID
        EXPECTED_RELEASE_ID
        EXPECTED_RELEASE_PUBLISHED_AT
        EXPECTED_RELEASE_CHECKSUM_SHA256
        EXPECTED_CONTRACT_STATE
        EXPECTED_OPEN_SHOULD_HAVE
        EXPECTED_DEFERRED_SHOULD_HAVE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing release-readiness variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED EXPECTED_OPEN_MUST_HAVE)
    message(FATAL_ERROR
        "Missing release-readiness variable: EXPECTED_OPEN_MUST_HAVE")
endif()

foreach(required_file IN ITEMS
        "${MATRIX}"
        "${PUBLIC_CONTRACT}"
        "${RELEASE_NOTES}"
        "${ROADMAP}"
        "${JIT_SCOPE_VALIDATOR}"
        "${JIT_SCOPE_DECISION}"
        "${NATIVE_SCALAR_REPORT}"
        "${NATIVE_ARRAY_REPORT}"
        "${NOJIT_ARRAY_REPORT}"
        "${AUTHENTICATION_EVIDENCE_VALIDATOR}"
        "${AUTHENTICATION_EVIDENCE_ROOT}/manifest.json"
        "${PUBLICATION_EVIDENCE_VALIDATOR}"
        "${PUBLICATION_EVIDENCE_ROOT}/manifest.json")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "Release-readiness input is missing: ${required_file}")
    endif()
endforeach()

file(READ "${MATRIX}" matrix_json)
file(READ "${PUBLIC_CONTRACT}" public_contract_json)
file(READ "${RELEASE_NOTES}" release_notes)
file(READ "${ROADMAP}" roadmap)

string(JSON matrix_version GET "${matrix_json}" version)
string(JSON contract_version GET
    "${public_contract_json}" release engine_version)
string(JSON contract_state GET
    "${public_contract_json}" release state)
if(NOT matrix_version STREQUAL EXPECTED_VERSION OR
   NOT contract_version STREQUAL EXPECTED_PUBLIC_CONTRACT_VERSION OR
   NOT contract_state STREQUAL EXPECTED_CONTRACT_STATE)
    message(FATAL_ERROR
        "Release version/state drifted.\n"
        "matrix: ${matrix_version}\n"
        "contract: ${contract_version} (${contract_state})\n"
        "expected matrix: ${EXPECTED_VERSION}\n"
        "expected historical contract: ${EXPECTED_PUBLIC_CONTRACT_VERSION} "
        "(${EXPECTED_CONTRACT_STATE})")
endif()

if(EXPECTED_CONTRACT_STATE STREQUAL "frozen-v1")
    string(FIND "${release_notes}"
        "Publication contract: **frozen v1**."
        release_status_position)
    string(FIND "${release_notes}"
        "source project version is `${EXPECTED_RELEASE_VERSION}`"
        release_version_position)
    string(FIND "${release_notes}"
        "Publication status: **released**."
        publication_status_position)
    string(FIND "${roadmap}"
        "**Status: complete.**"
        roadmap_status_position)
    string(FIND "${roadmap}"
        "The 32-asset GitHub Release is published"
        roadmap_publication_position)
else()
    message(FATAL_ERROR
        "Unsupported public-contract release state: ${EXPECTED_CONTRACT_STATE}")
endif()
if(release_status_position EQUAL -1 OR
   release_version_position EQUAL -1 OR
   publication_status_position EQUAL -1 OR
   roadmap_status_position EQUAL -1 OR
   roadmap_publication_position EQUAL -1)
    message(FATAL_ERROR
        "Release notes or roadmap no longer match the frozen release state")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DMPARSER_EVIDENCE_ROOT=${AUTHENTICATION_EVIDENCE_ROOT}"
        "-DMPARSER_EXPECTED_VERSION=${EXPECTED_AUTHENTICATION_VERSION}"
        "-DMPARSER_EXPECTED_TAG=v${EXPECTED_AUTHENTICATION_VERSION}"
        "-DMPARSER_EXPECTED_REVISION=${EXPECTED_AUTHENTICATION_REVISION}"
        "-DMPARSER_EXPECTED_RUN_ID=${EXPECTED_AUTHENTICATION_RUN_ID}"
        -P "${AUTHENTICATION_EVIDENCE_VALIDATOR}"
    RESULT_VARIABLE authentication_validation_status
    OUTPUT_VARIABLE authentication_validation_output
    ERROR_VARIABLE authentication_validation_error)
if(NOT authentication_validation_status EQUAL 0)
    message(FATAL_ERROR
        "Release authentication evidence failed readiness validation\n"
        "stdout:\n${authentication_validation_output}\n"
        "stderr:\n${authentication_validation_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DMPARSER_EVIDENCE_ROOT=${PUBLICATION_EVIDENCE_ROOT}"
        "-DMPARSER_EXPECTED_VERSION=${EXPECTED_RELEASE_VERSION}"
        "-DMPARSER_EXPECTED_TAG=v${EXPECTED_RELEASE_VERSION}"
        "-DMPARSER_EXPECTED_REVISION=${EXPECTED_AUTHENTICATION_REVISION}"
        "-DMPARSER_EXPECTED_RELEASE_ID=${EXPECTED_RELEASE_ID}"
        "-DMPARSER_EXPECTED_PUBLISHED_AT=${EXPECTED_RELEASE_PUBLISHED_AT}"
        "-DMPARSER_EXPECTED_RELEASE_CHECKSUM_SHA256=${EXPECTED_RELEASE_CHECKSUM_SHA256}"
        -P "${PUBLICATION_EVIDENCE_VALIDATOR}"
    RESULT_VARIABLE publication_validation_status
    OUTPUT_VARIABLE publication_validation_output
    ERROR_VARIABLE publication_validation_error)
if(NOT publication_validation_status EQUAL 0)
    message(FATAL_ERROR
        "Release publication evidence failed readiness validation\n"
        "stdout:\n${publication_validation_output}\n"
        "stderr:\n${publication_validation_error}")
endif()

set(actual_open_must_have)
set(actual_open_should_have)
set(actual_deferred_should_have)
string(JSON gap_count LENGTH "${matrix_json}" gaps)
if(gap_count LESS 1)
    message(FATAL_ERROR "Release has no explicit gap contracts")
endif()
math(EXPR gap_last "${gap_count} - 1")
foreach(gap_index RANGE 0 ${gap_last})
    string(JSON gap_id GET
        "${matrix_json}" gaps ${gap_index} id)
    string(JSON gap_priority GET
        "${matrix_json}" gaps ${gap_index} priority)
    string(JSON gap_state GET
        "${matrix_json}" gaps ${gap_index} state)
    string(JSON gap_target GET
        "${matrix_json}" gaps ${gap_index} target)
    string(JSON gap_impact GET
        "${matrix_json}" gaps ${gap_index} framework_impact)

    if(gap_priority STREQUAL "must-have" AND
       NOT gap_state STREQUAL "closed")
        if(NOT gap_target STREQUAL "v1.0" OR
           NOT gap_impact STREQUAL "none")
            message(FATAL_ERROR
                "Open Must-have ${gap_id} is not a bounded v1.0 "
                "evidence-only blocker")
        endif()
        list(APPEND actual_open_must_have
            "${gap_id}:${gap_state}")
    elseif(gap_priority STREQUAL "should-have")
        if(gap_state STREQUAL "deferred")
            if(NOT gap_target STREQUAL "v1.x" OR
               (NOT gap_impact STREQUAL "none" AND
                NOT gap_impact STREQUAL "additive"))
                message(FATAL_ERROR
                    "Deferred Should-have ${gap_id} is not a bounded "
                    "additive v1.x item")
            endif()
            list(APPEND actual_deferred_should_have
                "${gap_id}:${gap_state}")
        elseif(NOT gap_state STREQUAL "closed")
            if(NOT gap_target STREQUAL "v1.0" OR
               (NOT gap_impact STREQUAL "none" AND
                NOT gap_impact STREQUAL "additive"))
                message(FATAL_ERROR
                    "Open Should-have ${gap_id} is not a bounded additive "
                    "v1.0 release")
            endif()
            list(APPEND actual_open_should_have
                "${gap_id}:${gap_state}")
        endif()
    elseif(gap_priority STREQUAL "post-v1.0" AND
           NOT gap_state STREQUAL "deferred")
        message(FATAL_ERROR
            "Post-v1.0 gap ${gap_id} is no longer explicitly deferred")
    endif()
endforeach()

set(expected_open_must_have ${EXPECTED_OPEN_MUST_HAVE})
set(expected_open_should_have ${EXPECTED_OPEN_SHOULD_HAVE})
set(expected_deferred_should_have ${EXPECTED_DEFERRED_SHOULD_HAVE})
list(SORT actual_open_must_have)
list(SORT actual_open_should_have)
list(SORT actual_deferred_should_have)
list(SORT expected_open_must_have)
list(SORT expected_open_should_have)
list(SORT expected_deferred_should_have)
if(NOT "${actual_open_must_have}" STREQUAL
       "${expected_open_must_have}")
    message(FATAL_ERROR
        "Unexpected open Must-have release gaps.\n"
        "actual: ${actual_open_must_have}\n"
        "expected: ${expected_open_must_have}")
endif()
if(NOT "${actual_open_should_have}" STREQUAL
       "${expected_open_should_have}")
    message(FATAL_ERROR
        "Unexpected open Should-have release gaps.\n"
        "actual: ${actual_open_should_have}\n"
        "expected: ${expected_open_should_have}")
endif()
if(NOT "${actual_deferred_should_have}" STREQUAL
       "${expected_deferred_should_have}")
    message(FATAL_ERROR
        "Unexpected deferred Should-have release gaps.\n"
        "actual: ${actual_deferred_should_have}\n"
        "expected: ${expected_deferred_should_have}")
endif()

include("${JIT_SCOPE_VALIDATOR}")

list(LENGTH actual_open_must_have must_have_count)
list(LENGTH actual_open_should_have should_have_count)
list(LENGTH actual_deferred_should_have deferred_should_have_count)
message(STATUS
    "MParser release readiness validated: "
    "${EXPECTED_VERSION}, ${must_have_count} bounded Must-have blockers, "
    "${should_have_count} scoped Should-have items, "
    "${deferred_should_have_count} deferred Should-have items")
