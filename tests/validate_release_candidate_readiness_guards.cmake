cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_CMAKE_COMMAND
        MPARSER_VALIDATOR
        MPARSER_BUILD_DIR
        MPARSER_TEST_ROOT
        MATRIX
        PUBLIC_CONTRACT
        RELEASE_NOTES
        ROADMAP
        EXPECTED_VERSION
        EXPECTED_CONTRACT_STATE
        EXPECTED_OPEN_MUST_HAVE
        EXPECTED_OPEN_SHOULD_HAVE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing readiness-guard variable: ${required_variable}")
    endif()
endforeach()

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
            "-DMATRIX=${tampered_matrix}"
            "-DPUBLIC_CONTRACT=${PUBLIC_CONTRACT}"
            "-DRELEASE_NOTES=${RELEASE_NOTES}"
            "-DROADMAP=${ROADMAP}"
            "-DEXPECTED_VERSION=${EXPECTED_VERSION}"
            "-DEXPECTED_CONTRACT_STATE=${EXPECTED_CONTRACT_STATE}"
            "-DEXPECTED_OPEN_MUST_HAVE=${EXPECTED_OPEN_MUST_HAVE}"
            "-DEXPECTED_OPEN_SHOULD_HAVE=${EXPECTED_OPEN_SHOULD_HAVE}"
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

find_gap_index("G-RELIABILITY-001" reliability_index)
set(framework_matrix "${matrix_json}")
string(JSON framework_matrix SET
    "${framework_matrix}" gaps ${reliability_index}
    framework_impact "\"contract-extension\"")
set(framework_matrix_file "${test_root}/framework-impact.json")
file(WRITE "${framework_matrix_file}" "${framework_matrix}\n")
expect_rejection(
    "Framework-impacting release blocker"
    "${framework_matrix_file}"
    "Open Must-have G-RELIABILITY-001")

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
    "Open Should-have G-JIT-001")

find_gap_index("G-DOCUMENTATION-001" documentation_index)
set(blocker_matrix "${matrix_json}")
string(JSON blocker_matrix SET
    "${blocker_matrix}" gaps ${documentation_index} state "\"closed\"")
set(blocker_matrix_file "${test_root}/blocker-set.json")
file(WRITE "${blocker_matrix_file}" "${blocker_matrix}\n")
expect_rejection(
    "Unreviewed release blocker-set change"
    "${blocker_matrix_file}"
    "Unexpected open Must-have")

message(STATUS
    "MParser release candidate readiness guards validated: "
    "Must-have/Should-have impact and blocker-set drift rejected")
