cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_BASELINE_TOOL
        MPARSER_CLI
        MPARSER_C_API_LIBRARY
        MPARSER_SCHEMA_VALIDATOR
        MPARSER_SCHEMA
        MPARSER_SCALAR_SOURCE
        MPARSER_ARRAY_SOURCE
        MPARSER_OUTPUT_DIR
        MPARSER_REVISION
        MPARSER_EXPECTED_OS
        MPARSER_EXPECTED_ARCHITECTURE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "performance evidence collector requires ${required_variable}")
    endif()
endforeach()

foreach(required_file IN ITEMS
        "${MPARSER_BASELINE_TOOL}"
        "${MPARSER_CLI}"
        "${MPARSER_C_API_LIBRARY}"
        "${MPARSER_SCHEMA_VALIDATOR}"
        "${MPARSER_SCHEMA}"
        "${MPARSER_SCALAR_SOURCE}"
        "${MPARSER_ARRAY_SOURCE}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "performance evidence input is missing: ${required_file}")
    endif()
endforeach()

if(NOT DEFINED MPARSER_QUICK)
    set(MPARSER_QUICK OFF)
endif()

file(MAKE_DIRECTORY "${MPARSER_OUTPUT_DIR}")

function(collect_performance_report report_name workload_id source_path)
    set(report_path "${MPARSER_OUTPUT_DIR}/${report_name}.json")
    set(collector_command
        "${MPARSER_BASELINE_TOOL}"
        "--cli=${MPARSER_CLI}"
        "--library=${MPARSER_C_API_LIBRARY}"
        "--source=${source_path}"
        "--output=${report_path}"
        "--workload-id=${workload_id}"
        "--revision=${MPARSER_REVISION}")
    if(MPARSER_QUICK)
        list(APPEND collector_command --quick)
    endif()

    execute_process(
        COMMAND ${collector_command}
        RESULT_VARIABLE collector_result
        OUTPUT_VARIABLE collector_output
        ERROR_VARIABLE collector_error
        TIMEOUT 600)
    if(NOT "${collector_result}" STREQUAL "0")
        message(FATAL_ERROR
            "performance evidence collector failed for ${workload_id} "
            "(${collector_result})\n"
            "stdout:\n${collector_output}\n"
            "stderr:\n${collector_error}")
    endif()

    execute_process(
        COMMAND "${MPARSER_SCHEMA_VALIDATOR}"
            --require-native-cache
            "${MPARSER_SCHEMA}"
            "${report_path}"
        RESULT_VARIABLE validator_result
        OUTPUT_VARIABLE validator_output
        ERROR_VARIABLE validator_error
        TIMEOUT 120)
    if(NOT "${validator_result}" STREQUAL "0")
        message(FATAL_ERROR
            "performance evidence validation failed for ${workload_id} "
            "(${validator_result})\n"
            "stdout:\n${validator_output}\n"
            "stderr:\n${validator_error}")
    endif()

    file(READ "${report_path}" report_json)
    string(JSON reported_revision GET "${report_json}" revision)
    string(JSON reported_workload GET "${report_json}" workload id)
    string(JSON reported_os GET "${report_json}" environment os)
    string(JSON reported_architecture GET
        "${report_json}" environment architecture)
    string(JSON reported_cpu_model GET
        "${report_json}" environment cpu_model)
    string(JSON reported_emulated GET
        "${report_json}" environment emulated)
    string(JSON native_available GET
        "${report_json}" build native_jit_available)
    if(NOT reported_revision STREQUAL MPARSER_REVISION OR
       NOT reported_workload STREQUAL workload_id OR
       NOT reported_os STREQUAL MPARSER_EXPECTED_OS OR
       NOT reported_architecture STREQUAL
           MPARSER_EXPECTED_ARCHITECTURE OR
       reported_cpu_model STREQUAL "unknown" OR
       reported_emulated OR
       NOT native_available)
        message(FATAL_ERROR
            "performance evidence identity mismatch for ${workload_id}\n"
            "revision: ${reported_revision}\n"
            "workload: ${reported_workload}\n"
            "OS: ${reported_os}\n"
            "architecture: ${reported_architecture}\n"
            "CPU model: ${reported_cpu_model}\n"
            "emulated: ${reported_emulated}\n"
            "native JIT: ${native_available}")
    endif()

    file(SHA256 "${source_path}" expected_source_sha256)
    string(JSON reported_source_sha256 GET
        "${report_json}" workload source_sha256)
    if(NOT reported_source_sha256 STREQUAL expected_source_sha256)
        message(FATAL_ERROR
            "performance evidence source hash mismatch for ${workload_id}")
    endif()

    foreach(artifact_name IN ITEMS
            baseline_tool
            mparser_cli
            mparser_c_api)
        if(artifact_name STREQUAL "baseline_tool")
            set(artifact_path "${MPARSER_BASELINE_TOOL}")
        elseif(artifact_name STREQUAL "mparser_cli")
            set(artifact_path "${MPARSER_CLI}")
        else()
            set(artifact_path "${MPARSER_C_API_LIBRARY}")
        endif()
        file(SIZE "${artifact_path}" expected_artifact_size)
        file(SHA256 "${artifact_path}" expected_artifact_sha256)
        string(JSON reported_artifact_size GET
            "${report_json}" resources binary_artifacts
            ${artifact_name} size_bytes)
        string(JSON reported_artifact_sha256 GET
            "${report_json}" resources binary_artifacts
            ${artifact_name} sha256)
        if(NOT reported_artifact_size EQUAL expected_artifact_size OR
           NOT reported_artifact_sha256 STREQUAL
               expected_artifact_sha256)
            message(FATAL_ERROR
                "performance evidence artifact mismatch for "
                "${artifact_name} in ${workload_id}")
        endif()
    endforeach()

    message(STATUS
        "Validated ${workload_id}: ${report_path}\n${collector_output}"
        "${validator_output}")
endfunction()

collect_performance_report(
    native-scalar-loop-v1 scalar-loop-v1 "${MPARSER_SCALAR_SOURCE}")
collect_performance_report(
    native-linear-array-v1 linear-array-v1 "${MPARSER_ARRAY_SOURCE}")

message(STATUS
    "MParser performance evidence collected: 2 native reports; "
    "revision=${MPARSER_REVISION}; OS=${MPARSER_EXPECTED_OS}; "
    "architecture=${MPARSER_EXPECTED_ARCHITECTURE}; "
    "quick=${MPARSER_QUICK}")
