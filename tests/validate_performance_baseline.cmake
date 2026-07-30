foreach(required_variable IN ITEMS
        MPARSER_BASELINE_TOOL
        MPARSER_CLI
        MPARSER_C_API_LIBRARY
        MPARSER_SCHEMA_VALIDATOR
        MPARSER_SCHEMA
        MPARSER_SOURCE
        MPARSER_OUTPUT_DIR)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "${required_variable} is required")
    endif()
endforeach()

file(MAKE_DIRECTORY "${MPARSER_OUTPUT_DIR}")
set(report_path
    "${MPARSER_OUTPUT_DIR}/performance-baseline-quick.json")

set(baseline_command)
if(DEFINED MPARSER_EMULATOR AND
   NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND baseline_command ${MPARSER_EMULATOR})
endif()
list(APPEND baseline_command "${MPARSER_BASELINE_TOOL}")
if(DEFINED MPARSER_EMULATOR AND
   NOT MPARSER_EMULATOR STREQUAL "")
    foreach(emulator_argument IN LISTS MPARSER_EMULATOR)
        list(APPEND baseline_command
            "--child-prefix=${emulator_argument}")
    endforeach()
endif()
list(APPEND baseline_command
    "--cli=${MPARSER_CLI}"
    "--library=${MPARSER_C_API_LIBRARY}"
    "--source=${MPARSER_SOURCE}"
    "--output=${report_path}"
    "--workload-id=quick-contract-v1"
    "--revision=ctest-working-tree"
    "--quick")

execute_process(
    COMMAND ${baseline_command}
    RESULT_VARIABLE baseline_result
    OUTPUT_VARIABLE baseline_output
    ERROR_VARIABLE baseline_error
    TIMEOUT 120)
if(NOT "${baseline_result}" STREQUAL "0")
    message(FATAL_ERROR
        "performance baseline collector failed (${baseline_result})\n"
        "stdout:\n${baseline_output}\n"
        "stderr:\n${baseline_error}")
endif()

file(READ "${report_path}" report_document)
file(SHA256 "${MPARSER_SOURCE}" expected_source_sha256)
string(JSON reported_source_sha256 GET
    "${report_document}" workload source_sha256)
if(NOT reported_source_sha256 STREQUAL expected_source_sha256)
    message(FATAL_ERROR
        "performance baseline source SHA-256 is inconsistent\n"
        "expected: ${expected_source_sha256}\n"
        "reported: ${reported_source_sha256}")
endif()

function(validate_reported_artifact artifact_name artifact_path)
    file(SIZE "${artifact_path}" expected_size)
    file(SHA256 "${artifact_path}" expected_sha256)
    string(JSON reported_size GET
        "${report_document}" resources binary_artifacts
        "${artifact_name}" size_bytes)
    string(JSON reported_sha256 GET
        "${report_document}" resources binary_artifacts
        "${artifact_name}" sha256)
    if(NOT reported_size EQUAL expected_size OR
       NOT reported_sha256 STREQUAL expected_sha256)
        message(FATAL_ERROR
            "performance baseline ${artifact_name} identity is "
            "inconsistent\n"
            "expected size/SHA-256: ${expected_size} ${expected_sha256}\n"
            "reported size/SHA-256: ${reported_size} "
            "${reported_sha256}")
    endif()
endfunction()

validate_reported_artifact(
    baseline_tool "${MPARSER_BASELINE_TOOL}")
validate_reported_artifact(
    mparser_cli "${MPARSER_CLI}")
validate_reported_artifact(
    mparser_c_api "${MPARSER_C_API_LIBRARY}")

set(validator_command)
if(DEFINED MPARSER_EMULATOR AND
   NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND validator_command ${MPARSER_EMULATOR})
endif()
list(APPEND validator_command
    "${MPARSER_SCHEMA_VALIDATOR}"
    "--require-native-cache"
    "${MPARSER_SCHEMA}"
    "${report_path}")
execute_process(
    COMMAND ${validator_command}
    RESULT_VARIABLE validator_result
    OUTPUT_VARIABLE validator_output
    ERROR_VARIABLE validator_error
    TIMEOUT 60)
if(NOT "${validator_result}" STREQUAL "0")
    message(FATAL_ERROR
        "performance baseline validation failed (${validator_result})\n"
        "stdout:\n${validator_output}\n"
        "stderr:\n${validator_error}")
endif()

set(alias_source
    "${MPARSER_OUTPUT_DIR}/performance-baseline-alias-source.m")
configure_file("${MPARSER_SOURCE}" "${alias_source}" COPYONLY)
set(alias_command)
if(DEFINED MPARSER_EMULATOR AND
   NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND alias_command ${MPARSER_EMULATOR})
endif()
list(APPEND alias_command
    "${MPARSER_BASELINE_TOOL}"
    "--cli=${MPARSER_CLI}"
    "--library=${MPARSER_C_API_LIBRARY}"
    "--source=${alias_source}"
    "--output=${alias_source}"
    "--quick")
execute_process(
    COMMAND ${alias_command}
    RESULT_VARIABLE alias_result
    OUTPUT_VARIABLE alias_output
    ERROR_VARIABLE alias_error
    TIMEOUT 30)
if("${alias_result}" STREQUAL "0" OR
   NOT alias_error MATCHES
       "--output cannot overwrite the source workload")
    message(FATAL_ERROR
        "performance baseline collector did not reject an output/source "
        "alias (${alias_result})\n"
        "stdout:\n${alias_output}\n"
        "stderr:\n${alias_error}")
endif()

message(STATUS "${baseline_output}")
message(STATUS "${validator_output}")
message(STATUS
    "performance baseline source/binary identities and collector alias "
    "guard validated")
