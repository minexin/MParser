cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_CMAKE_COMMAND
        MPARSER_VALIDATOR
        MPARSER_BUILD_DIR
        MPARSER_TEST_ROOT
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
        EXPECTED_VERSION
        EXPECTED_JIT_EVIDENCE_VERSION
        EXPECTED_AUTHENTICATION_REVISION
        EXPECTED_AUTHENTICATION_RUN_ID
        EXPECTED_CONTRACT_STATE
        EXPECTED_OPEN_SHOULD_HAVE
        EXPECTED_DEFERRED_SHOULD_HAVE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing readiness-guard variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED EXPECTED_OPEN_MUST_HAVE)
    message(FATAL_ERROR
        "Missing readiness-guard variable: EXPECTED_OPEN_MUST_HAVE")
endif()

get_filename_component(build_dir "${MPARSER_BUILD_DIR}" ABSOLUTE)
get_filename_component(test_root "${MPARSER_TEST_ROOT}" ABSOLUTE)
file(RELATIVE_PATH test_relative "${build_dir}" "${test_root}")
if(test_relative STREQUAL "" OR
   IS_ABSOLUTE "${test_relative}" OR
   test_relative MATCHES "^\\.\\.")
    message(FATAL_ERROR
        "Readiness-guard test root must stay inside the build tree: "
        "${test_root}")
endif()

file(REMOVE_RECURSE "${test_root}")
file(MAKE_DIRECTORY "${test_root}")
file(READ "${MATRIX}" matrix_json)

function(find_gap_index expected_id output_variable)
    string(JSON local_gap_count LENGTH "${matrix_json}" gaps)
    math(EXPR local_gap_last "${local_gap_count} - 1")
    foreach(local_gap_index RANGE 0 ${local_gap_last})
        string(JSON local_gap_id GET
            "${matrix_json}" gaps ${local_gap_index} id)
        if(local_gap_id STREQUAL expected_id)
            set(${output_variable} "${local_gap_index}" PARENT_SCOPE)
            return()
        endif()
    endforeach()
    message(FATAL_ERROR "Readiness-guard gap is missing: ${expected_id}")
endfunction()

function(expect_rejection description tampered_matrix expected_pattern)
    execute_process(
        COMMAND "${MPARSER_CMAKE_COMMAND}"
            "-DPROJECT_ROOT=${PROJECT_ROOT}"
            "-DMATRIX=${tampered_matrix}"
            "-DPUBLIC_CONTRACT=${PUBLIC_CONTRACT}"
            "-DRELEASE_NOTES=${RELEASE_NOTES}"
            "-DROADMAP=${ROADMAP}"
            "-DJIT_SCOPE_VALIDATOR=${JIT_SCOPE_VALIDATOR}"
            "-DJIT_SCOPE_DECISION=${JIT_SCOPE_DECISION}"
            "-DNATIVE_SCALAR_REPORT=${NATIVE_SCALAR_REPORT}"
            "-DNATIVE_ARRAY_REPORT=${NATIVE_ARRAY_REPORT}"
            "-DNOJIT_ARRAY_REPORT=${NOJIT_ARRAY_REPORT}"
            "-DAUTHENTICATION_EVIDENCE_VALIDATOR=${AUTHENTICATION_EVIDENCE_VALIDATOR}"
            "-DAUTHENTICATION_EVIDENCE_ROOT=${AUTHENTICATION_EVIDENCE_ROOT}"
            "-DEXPECTED_VERSION=${EXPECTED_VERSION}"
            "-DEXPECTED_JIT_EVIDENCE_VERSION=${EXPECTED_JIT_EVIDENCE_VERSION}"
            "-DEXPECTED_AUTHENTICATION_REVISION=${EXPECTED_AUTHENTICATION_REVISION}"
            "-DEXPECTED_AUTHENTICATION_RUN_ID=${EXPECTED_AUTHENTICATION_RUN_ID}"
            "-DEXPECTED_CONTRACT_STATE=${EXPECTED_CONTRACT_STATE}"
            "-DEXPECTED_OPEN_MUST_HAVE=${EXPECTED_OPEN_MUST_HAVE}"
            "-DEXPECTED_OPEN_SHOULD_HAVE=${EXPECTED_OPEN_SHOULD_HAVE}"
            "-DEXPECTED_DEFERRED_SHOULD_HAVE=${EXPECTED_DEFERRED_SHOULD_HAVE}"
            -P "${MPARSER_VALIDATOR}"
        RESULT_VARIABLE validation_status
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error)
    if(validation_status EQUAL 0)
        message(FATAL_ERROR
            "${description} was accepted by the readiness validator")
    endif()
    set(validation_log "${validation_output}\n${validation_error}")
    if(NOT validation_log MATCHES "${expected_pattern}")
        message(FATAL_ERROR
            "${description} failed for an unexpected reason\n"
            "${validation_log}")
    endif()
endfunction()

find_gap_index("G-PROVENANCE-001" provenance_index)
set(framework_matrix "${matrix_json}")
string(JSON framework_matrix SET
    "${framework_matrix}" gaps ${provenance_index}
    state "\"in-progress\"")
string(JSON framework_matrix SET
    "${framework_matrix}" gaps ${provenance_index}
    framework_impact "\"contract-extension\"")
set(framework_matrix_file "${test_root}/framework-impact.json")
file(WRITE "${framework_matrix_file}" "${framework_matrix}\n")
expect_rejection(
    "Framework-impacting release blocker"
    "${framework_matrix_file}"
    "Open Must-have G-PROVENANCE-001")

find_gap_index("G-JIT-001" jit_index)
set(should_have_matrix "${matrix_json}")
string(JSON should_have_matrix SET
    "${should_have_matrix}" gaps ${jit_index}
    framework_impact "\"representation-change\"")
set(should_have_matrix_file "${test_root}/should-have-impact.json")
file(WRITE "${should_have_matrix_file}" "${should_have_matrix}\n")
expect_rejection(
    "Framework-impacting Should-have"
    "${should_have_matrix_file}"
    "Deferred Should-have G-JIT-001")

set(reactivated_jit_matrix "${matrix_json}")
string(JSON reactivated_jit_matrix SET
    "${reactivated_jit_matrix}" gaps ${jit_index}
    state "\"open\"")
string(JSON reactivated_jit_matrix SET
    "${reactivated_jit_matrix}" gaps ${jit_index}
    target "\"v1.0\"")
set(reactivated_jit_matrix_file "${test_root}/reactivated-jit.json")
file(WRITE "${reactivated_jit_matrix_file}"
    "${reactivated_jit_matrix}\n")
expect_rejection(
    "Unreviewed v1.0 JIT reactivation"
    "${reactivated_jit_matrix_file}"
    "Unexpected open Should-have")

find_gap_index("G-DOCUMENTATION-001" documentation_index)
set(blocker_matrix "${matrix_json}")
string(JSON blocker_matrix SET
    "${blocker_matrix}" gaps ${documentation_index} state "\"in-progress\"")
set(blocker_matrix_file "${test_root}/blocker-set.json")
file(WRITE "${blocker_matrix_file}" "${blocker_matrix}\n")
expect_rejection(
    "Unreviewed release blocker-set change"
    "${blocker_matrix_file}"
    "Unexpected open Must-have")

message(STATUS
    "MParser release candidate readiness guards validated: "
    "Must-have/Should-have impact, deferred JIT reactivation, and "
    "blocker-set drift rejected")
