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
        EXPECTED_VERSION
        EXPECTED_JIT_EVIDENCE_VERSION
        EXPECTED_CONTRACT_STATE
        EXPECTED_OPEN_MUST_HAVE
        EXPECTED_OPEN_SHOULD_HAVE
        EXPECTED_DEFERRED_SHOULD_HAVE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing release-readiness variable: ${required_variable}")
    endif()
endforeach()

foreach(required_file IN ITEMS
        "${MATRIX}"
        "${PUBLIC_CONTRACT}"
        "${RELEASE_NOTES}"
        "${ROADMAP}"
        "${JIT_SCOPE_VALIDATOR}"
        "${JIT_SCOPE_DECISION}"
        "${NATIVE_SCALAR_REPORT}"
        "${NATIVE_ARRAY_REPORT}"
        "${NOJIT_ARRAY_REPORT}")
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
    "${public_contract_json}" candidate engine_version)
string(JSON contract_state GET
    "${public_contract_json}" candidate state)
if(NOT matrix_version STREQUAL EXPECTED_VERSION OR
   NOT contract_version STREQUAL EXPECTED_VERSION OR
   NOT contract_state STREQUAL EXPECTED_CONTRACT_STATE)
    message(FATAL_ERROR
        "Release candidate version/state drifted.\n"
        "matrix: ${matrix_version}\n"
        "contract: ${contract_version} (${contract_state})\n"
        "expected: ${EXPECTED_VERSION} (${EXPECTED_CONTRACT_STATE})")
endif()

string(FIND "${release_notes}"
    "Publication status: **release candidate documentation**."
    candidate_status_position)
string(FIND "${release_notes}"
    "remains version `${EXPECTED_VERSION}`"
    candidate_version_position)
string(FIND "${roadmap}"
    "**Status: candidate platform validation complete; publication preparation in progress.**"
    roadmap_status_position)
if(candidate_status_position EQUAL -1 OR
   candidate_version_position EQUAL -1 OR
   roadmap_status_position EQUAL -1)
    message(FATAL_ERROR
        "Release notes or roadmap no longer describe a pre-1.0 candidate")
endif()

set(actual_open_must_have)
set(actual_open_should_have)
set(actual_deferred_should_have)
string(JSON gap_count LENGTH "${matrix_json}" gaps)
if(gap_count LESS 1)
    message(FATAL_ERROR "Release candidate has no explicit gap contracts")
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
                    "v1.0 candidate")
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
    "MParser release candidate readiness validated: "
    "${EXPECTED_VERSION}, ${must_have_count} bounded Must-have blockers, "
    "${should_have_count} scoped Should-have items, "
    "${deferred_should_have_count} deferred Should-have items")
