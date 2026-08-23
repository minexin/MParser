cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_BASELINE_TOOL
        MPARSER_CLI
        MPARSER_C_API_LIBRARY
        MPARSER_BASELINE_VALIDATOR
        MPARSER_BASELINE_SCHEMA
        MPARSER_SUITE_VALIDATOR
        MPARSER_SUITE_SCHEMA
        MPARSER_SUITE_INTEGRITY_VALIDATOR
        MPARSER_MANIFEST
        MPARSER_SOURCE_ROOT
        MPARSER_OUTPUT_DIR
        MPARSER_REVISION)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "performance suite requires ${required_variable}")
    endif()
endforeach()

foreach(required_file IN ITEMS
        "${MPARSER_BASELINE_TOOL}"
        "${MPARSER_CLI}"
        "${MPARSER_C_API_LIBRARY}"
        "${MPARSER_BASELINE_VALIDATOR}"
        "${MPARSER_BASELINE_SCHEMA}"
        "${MPARSER_SUITE_VALIDATOR}"
        "${MPARSER_SUITE_SCHEMA}"
        "${MPARSER_SUITE_INTEGRITY_VALIDATOR}"
        "${MPARSER_MANIFEST}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "performance suite input is missing: ${required_file}")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${MPARSER_SOURCE_ROOT}")
    message(FATAL_ERROR
        "performance suite source root is not a directory: "
        "${MPARSER_SOURCE_ROOT}")
endif()
if(NOT DEFINED MPARSER_QUICK)
    set(MPARSER_QUICK OFF)
endif()

function(json_quote input output)
    set(value "${input}")
    string(REPLACE "\\" "\\\\" value "${value}")
    string(REPLACE "\"" "\\\"" value "${value}")
    string(REPLACE "\r" "\\r" value "${value}")
    string(REPLACE "\n" "\\n" value "${value}")
    string(REPLACE "\t" "\\t" value "${value}")
    set(${output} "\"${value}\"" PARENT_SCOPE)
endfunction()

function(read_required_json output document)
    string(JSON value ERROR_VARIABLE json_error GET
        "${document}" ${ARGN})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN ARGN "/" json_path)
        message(FATAL_ERROR
            "performance suite JSON is missing ${json_path}: "
            "${json_error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(classify_typed_coverage output report phase)
    read_required_json(status "${report}" measurements ${phase} status)
    if(status STREQUAL "unavailable")
        set(${output} "unavailable" PARENT_SCOPE)
        return()
    endif()
    if(NOT status STREQUAL "measured")
        message(FATAL_ERROR
            "performance suite report has unknown ${phase} status: "
            "${status}")
    endif()

    read_required_json(attempts "${report}" measurements ${phase}
        execution_summary typed_region_attempt_count)
    read_required_json(executions "${report}" measurements ${phase}
        execution_summary typed_region_execution_count)
    read_required_json(fallbacks "${report}" measurements ${phase}
        execution_summary typed_region_fallback_count)
    if(fallbacks GREATER 0)
        set(coverage "guarded-fallback")
    elseif(executions GREATER 0)
        set(coverage "executed")
    elseif(attempts EQUAL 0)
        set(coverage "uncovered")
    else()
        message(FATAL_ERROR
            "performance suite ${phase} attempts have neither execution "
            "nor fallback")
    endif()
    set(${output} "${coverage}" PARENT_SCOPE)
endfunction()

function(increment_coverage backend coverage)
    string(REPLACE "-" "_" key "${coverage}")
    set(variable "${backend}_${key}")
    math(EXPR updated "${${variable}} + 1")
    set(${variable} "${updated}" PARENT_SCOPE)
endfunction()

file(REAL_PATH "${MPARSER_SOURCE_ROOT}" source_root)
file(REAL_PATH "${MPARSER_MANIFEST}" manifest_path)
file(TO_CMAKE_PATH "${source_root}" source_root)
file(TO_CMAKE_PATH "${manifest_path}" manifest_path)
file(READ "${manifest_path}" manifest_document)
file(SHA256 "${manifest_path}" manifest_sha256)
file(RELATIVE_PATH manifest_relative "${source_root}" "${manifest_path}")
file(TO_CMAKE_PATH "${manifest_relative}" manifest_relative)
if(IS_ABSOLUTE "${manifest_relative}" OR
   manifest_relative MATCHES "(^|/)\\.\\.(/|$)")
    message(FATAL_ERROR
        "performance manifest must stay below the source root")
endif()

read_required_json(manifest_protocol "${manifest_document}"
    protocol name)
read_required_json(manifest_protocol_major "${manifest_document}"
    protocol major)
read_required_json(manifest_suite_id "${manifest_document}" suite id)
read_required_json(manifest_suite_version "${manifest_document}"
    suite version)
string(JSON workload_count LENGTH "${manifest_document}" workloads)
if(NOT manifest_protocol STREQUAL
       "mparser.performance-workload-manifest" OR
   NOT manifest_protocol_major EQUAL 1)
    message(FATAL_ERROR
        "unsupported performance workload manifest protocol")
endif()
if(NOT manifest_suite_id MATCHES "^[a-z0-9][a-z0-9.-]*$" OR
   manifest_suite_version LESS 1)
    message(FATAL_ERROR
        "performance workload manifest has an invalid suite identity")
endif()
if(workload_count LESS 1 OR workload_count GREATER 32)
    message(FATAL_ERROR
        "performance workload manifest must contain 1 through 32 entries")
endif()

file(MAKE_DIRECTORY "${MPARSER_OUTPUT_DIR}")
file(REAL_PATH "${MPARSER_OUTPUT_DIR}" output_root)
file(TO_CMAKE_PATH "${output_root}" output_root)

set(portable_executed 0)
set(portable_guarded_fallback 0)
set(portable_uncovered 0)
set(portable_unavailable 0)
set(native_executed 0)
set(native_guarded_fallback 0)
set(native_uncovered 0)
set(native_unavailable 0)
set(workload_ids)
set(report_entries)
set(first_environment)
set(first_build)

math(EXPR last_workload "${workload_count} - 1")
foreach(workload_index RANGE 0 ${last_workload})
    read_required_json(workload_id "${manifest_document}"
        workloads ${workload_index} id)
    read_required_json(category "${manifest_document}"
        workloads ${workload_index} category)
    read_required_json(source_relative "${manifest_document}"
        workloads ${workload_index} source)
    read_required_json(result_variable "${manifest_document}"
        workloads ${workload_index} result_variable)
    read_required_json(optimization_focus "${manifest_document}"
        workloads ${workload_index} optimization_focus)

    if(NOT workload_id MATCHES "^[a-z0-9][a-z0-9-]*$" OR
       NOT category MATCHES "^[a-z0-9][a-z0-9-]*$" OR
       NOT result_variable MATCHES "^[A-Za-z][A-Za-z0-9_]*$" OR
       optimization_focus STREQUAL "")
        message(FATAL_ERROR
            "performance workload ${workload_index} has invalid metadata")
    endif()
    list(FIND workload_ids "${workload_id}" duplicate_index)
    if(NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "duplicate performance workload id: ${workload_id}")
    endif()
    list(APPEND workload_ids "${workload_id}")
    if(IS_ABSOLUTE "${source_relative}" OR
       source_relative MATCHES "(^|/)\\.\\.(/|$)" OR
       source_relative MATCHES "(^|\\\\)\\.\\.(\\\\|$)")
        message(FATAL_ERROR
            "performance workload source must stay below the source root: "
            "${source_relative}")
    endif()

    file(REAL_PATH "${source_relative}" source_path
        BASE_DIRECTORY "${source_root}")
    file(TO_CMAKE_PATH "${source_path}" source_path)
    string(FIND "${source_path}" "${source_root}/" source_root_prefix)
    if(NOT source_root_prefix EQUAL 0 OR NOT EXISTS "${source_path}")
        message(FATAL_ERROR
            "performance workload source is missing or outside the source "
            "root: ${source_relative}")
    endif()

    set(report_file "${workload_id}.json")
    set(report_path "${output_root}/${report_file}")
    set(collector_command)
    if(DEFINED MPARSER_EMULATOR AND
       NOT MPARSER_EMULATOR STREQUAL "")
        list(APPEND collector_command ${MPARSER_EMULATOR})
    endif()
    list(APPEND collector_command "${MPARSER_BASELINE_TOOL}")
    if(DEFINED MPARSER_EMULATOR AND
       NOT MPARSER_EMULATOR STREQUAL "")
        foreach(emulator_argument IN LISTS MPARSER_EMULATOR)
            list(APPEND collector_command
                "--child-prefix=${emulator_argument}")
        endforeach()
    endif()
    list(APPEND collector_command
        "--cli=${MPARSER_CLI}"
        "--library=${MPARSER_C_API_LIBRARY}"
        "--source=${source_path}"
        "--output=${report_path}"
        "--workload-id=${workload_id}"
        "--result-variable=${result_variable}"
        "--revision=${MPARSER_REVISION}")
    if(MPARSER_QUICK)
        list(APPEND collector_command --quick)
    endif()

    execute_process(
        COMMAND ${collector_command}
        RESULT_VARIABLE collector_result
        OUTPUT_VARIABLE collector_output
        ERROR_VARIABLE collector_error
        TIMEOUT 900)
    if(NOT "${collector_result}" STREQUAL "0")
        message(FATAL_ERROR
            "performance suite collector failed for ${workload_id} "
            "(${collector_result})\nstdout:\n${collector_output}\n"
            "stderr:\n${collector_error}")
    endif()

    set(baseline_validator_command)
    if(DEFINED MPARSER_EMULATOR AND
       NOT MPARSER_EMULATOR STREQUAL "")
        list(APPEND baseline_validator_command ${MPARSER_EMULATOR})
    endif()
    list(APPEND baseline_validator_command
        "${MPARSER_BASELINE_VALIDATOR}"
        "${MPARSER_BASELINE_SCHEMA}"
        "${report_path}")
    execute_process(
        COMMAND ${baseline_validator_command}
        RESULT_VARIABLE baseline_validator_result
        OUTPUT_VARIABLE baseline_validator_output
        ERROR_VARIABLE baseline_validator_error
        TIMEOUT 120)
    if(NOT "${baseline_validator_result}" STREQUAL "0")
        message(FATAL_ERROR
            "performance baseline validation failed for ${workload_id} "
            "(${baseline_validator_result})\n"
            "stdout:\n${baseline_validator_output}\n"
            "stderr:\n${baseline_validator_error}")
    endif()

    file(READ "${report_path}" report_document)
    file(SHA256 "${source_path}" expected_source_sha256)
    file(SHA256 "${report_path}" report_sha256)
    read_required_json(reported_revision "${report_document}" revision)
    read_required_json(reported_workload "${report_document}"
        workload id)
    read_required_json(reported_result_variable "${report_document}"
        workload result_variable)
    read_required_json(reported_source_sha256 "${report_document}"
        workload source_sha256)
    read_required_json(all_results_match "${report_document}"
        correctness all_runtime_results_match)
    if(NOT reported_revision STREQUAL MPARSER_REVISION OR
       NOT reported_workload STREQUAL workload_id OR
       NOT reported_result_variable STREQUAL result_variable OR
       NOT reported_source_sha256 STREQUAL expected_source_sha256 OR
       NOT all_results_match)
        message(FATAL_ERROR
            "performance suite identity or correctness mismatch for "
            "${workload_id}")
    endif()

    string(JSON report_environment GET "${report_document}" environment)
    string(JSON report_build GET "${report_document}" build)
    if(workload_index EQUAL 0)
        set(first_environment "${report_environment}")
        set(first_build "${report_build}")
    elseif(NOT report_environment STREQUAL first_environment OR
           NOT report_build STREQUAL first_build)
        message(FATAL_ERROR
            "performance suite reports do not share one environment/build")
    endif()
    if(DEFINED MPARSER_EXPECTED_OS AND
       NOT MPARSER_EXPECTED_OS STREQUAL "")
        read_required_json(reported_os "${report_document}"
            environment os)
        if(NOT reported_os STREQUAL MPARSER_EXPECTED_OS)
            message(FATAL_ERROR
                "performance suite OS mismatch for ${workload_id}: "
                "${reported_os}")
        endif()
    endif()
    if(DEFINED MPARSER_EXPECTED_ARCHITECTURE AND
       NOT MPARSER_EXPECTED_ARCHITECTURE STREQUAL "")
        read_required_json(reported_architecture "${report_document}"
            environment architecture)
        if(NOT reported_architecture STREQUAL
               MPARSER_EXPECTED_ARCHITECTURE)
            message(FATAL_ERROR
                "performance suite architecture mismatch for "
                "${workload_id}: ${reported_architecture}")
        endif()
    endif()

    classify_typed_coverage(portable_coverage
        "${report_document}" portable)
    classify_typed_coverage(native_coverage
        "${report_document}" native_warm)
    increment_coverage(portable "${portable_coverage}")
    increment_coverage(native "${native_coverage}")

    foreach(phase IN ITEMS
            parse compile process_cold_start bytecode portable)
        read_required_json(${phase}_median "${report_document}"
            measurements ${phase} host_wall median_ns)
    endforeach()
    read_required_json(native_status "${report_document}"
        measurements native_warm status)
    if(native_status STREQUAL "measured")
        read_required_json(native_cold_median "${report_document}"
            measurements native_cold host_wall median_ns)
        read_required_json(native_warm_median "${report_document}"
            measurements native_warm host_wall median_ns)
        read_required_json(native_allocation "${report_document}"
            measurements native_warm allocation_activity requested_bytes)
        read_required_json(native_attempts "${report_document}"
            measurements native_warm execution_summary
            typed_region_attempt_count)
        read_required_json(native_executions "${report_document}"
            measurements native_warm execution_summary
            typed_region_execution_count)
        read_required_json(native_fallbacks "${report_document}"
            measurements native_warm execution_summary
            typed_region_fallback_count)
        set(native_cold_json "${native_cold_median}")
        set(native_warm_json "${native_warm_median}")
        set(native_allocation_json "${native_allocation}")
        set(native_execution_json
            "{\"attempts\":${native_attempts},\"executions\":${native_executions},\"fallbacks\":${native_fallbacks}}")
    else()
        set(native_cold_json null)
        set(native_warm_json null)
        set(native_allocation_json null)
        set(native_execution_json null)
    endif()

    read_required_json(bytecode_allocation "${report_document}"
        measurements bytecode allocation_activity requested_bytes)
    read_required_json(portable_allocation "${report_document}"
        measurements portable allocation_activity requested_bytes)
    read_required_json(portable_attempts "${report_document}"
        measurements portable execution_summary
        typed_region_attempt_count)
    read_required_json(portable_executions "${report_document}"
        measurements portable execution_summary
        typed_region_execution_count)
    read_required_json(portable_fallbacks "${report_document}"
        measurements portable execution_summary
        typed_region_fallback_count)
    read_required_json(reference_value "${report_document}"
        correctness reference_value)

    json_quote("${workload_id}" workload_id_json)
    json_quote("${category}" category_json)
    json_quote("${optimization_focus}" optimization_focus_json)
    json_quote("${source_relative}" source_relative_json)
    json_quote("${expected_source_sha256}" source_sha256_json)
    json_quote("${result_variable}" result_variable_json)
    json_quote("${report_file}" report_file_json)
    json_quote("${report_sha256}" report_sha256_json)
    json_quote("${portable_coverage}" portable_coverage_json)
    json_quote("${native_coverage}" native_coverage_json)

    set(entry
        "{\"workload_id\":${workload_id_json},"
        "\"category\":${category_json},"
        "\"optimization_focus\":${optimization_focus_json},"
        "\"source_path\":${source_relative_json},"
        "\"source_sha256\":${source_sha256_json},"
        "\"result_variable\":${result_variable_json},"
        "\"report_file\":${report_file_json},"
        "\"report_sha256\":${report_sha256_json},"
        "\"reference_value\":${reference_value},"
        "\"all_runtime_results_match\":true,"
        "\"typed_coverage\":{"
            "\"portable\":${portable_coverage_json},"
            "\"native\":${native_coverage_json}},"
        "\"timing_median_ns\":{"
            "\"parse\":${parse_median},"
            "\"compile\":${compile_median},"
            "\"process_cold_start\":${process_cold_start_median},"
            "\"bytecode\":${bytecode_median},"
            "\"portable\":${portable_median},"
            "\"native_cold\":${native_cold_json},"
            "\"native_warm\":${native_warm_json}},"
        "\"allocation_requested_bytes\":{"
            "\"bytecode\":${bytecode_allocation},"
            "\"portable\":${portable_allocation},"
            "\"native_warm\":${native_allocation_json}},"
        "\"typed_execution\":{"
            "\"portable\":{"
                "\"attempts\":${portable_attempts},"
                "\"executions\":${portable_executions},"
                "\"fallbacks\":${portable_fallbacks}},"
            "\"native\":${native_execution_json}}}")
    list(JOIN entry "" entry)
    list(APPEND report_entries "${entry}")
    message(STATUS
        "Collected ${workload_id}: portable=${portable_coverage}; "
        "native=${native_coverage}; bytecode_median_ns=${bytecode_median}; "
        "portable_median_ns=${portable_median}; "
        "native_warm_median_ns=${native_warm_json}")
endforeach()

list(JOIN report_entries "," reports_json)
json_quote("${MPARSER_REVISION}" revision_json)
json_quote("${manifest_suite_id}" manifest_id_json)
json_quote("${manifest_relative}" manifest_path_json)
json_quote("${manifest_sha256}" manifest_sha256_json)
if(MPARSER_QUICK)
    set(quick_json true)
else()
    set(quick_json false)
endif()
string(TIMESTAMP generated_at_utc "%Y-%m-%dT%H:%M:%SZ" UTC)
json_quote("${generated_at_utc}" generated_at_json)

set(index_document
    "{\"protocol\":{"
        "\"name\":\"mparser.performance-suite\","
        "\"major\":1,\"minor\":0},"
    "\"generated_at_utc\":${generated_at_json},"
    "\"revision\":${revision_json},"
    "\"manifest\":{"
        "\"id\":${manifest_id_json},"
        "\"version\":${manifest_suite_version},"
        "\"path\":${manifest_path_json},"
        "\"sha256\":${manifest_sha256_json}},"
    "\"environment\":${first_environment},"
    "\"build\":${first_build},"
    "\"settings\":{"
        "\"quick\":${quick_json},"
        "\"report_count\":${workload_count}},"
    "\"coverage_summary\":{"
        "\"portable\":{"
            "\"executed\":${portable_executed},"
            "\"guarded_fallback\":${portable_guarded_fallback},"
            "\"uncovered\":${portable_uncovered},"
            "\"unavailable\":${portable_unavailable}},"
        "\"native\":{"
            "\"executed\":${native_executed},"
            "\"guarded_fallback\":${native_guarded_fallback},"
            "\"uncovered\":${native_uncovered},"
            "\"unavailable\":${native_unavailable}}},"
    "\"reports\":[${reports_json}]}")
list(JOIN index_document "" index_document)

set(index_path "${output_root}/suite-index.json")
file(WRITE "${index_path}" "${index_document}\n")

set(suite_validator_command)
if(DEFINED MPARSER_EMULATOR AND
   NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND suite_validator_command ${MPARSER_EMULATOR})
endif()
list(APPEND suite_validator_command
    "${MPARSER_SUITE_VALIDATOR}"
    "${MPARSER_SUITE_SCHEMA}"
    "${index_path}")
execute_process(
    COMMAND ${suite_validator_command}
    RESULT_VARIABLE suite_validator_result
    OUTPUT_VARIABLE suite_validator_output
    ERROR_VARIABLE suite_validator_error
    TIMEOUT 120)
if(NOT "${suite_validator_result}" STREQUAL "0")
    message(FATAL_ERROR
        "performance suite index validation failed "
        "(${suite_validator_result})\n"
        "stdout:\n${suite_validator_output}\n"
        "stderr:\n${suite_validator_error}")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        "-DMPARSER_SUITE_INDEX=${index_path}"
        "-DMPARSER_SOURCE_ROOT=${source_root}"
        "-DMPARSER_REPORT_ROOT=${output_root}"
        -P "${MPARSER_SUITE_INTEGRITY_VALIDATOR}"
    RESULT_VARIABLE integrity_result
    OUTPUT_VARIABLE integrity_output
    ERROR_VARIABLE integrity_error
    TIMEOUT 120)
if(NOT "${integrity_result}" STREQUAL "0")
    message(FATAL_ERROR
        "performance suite integrity validation failed "
        "(${integrity_result})\nstdout:\n${integrity_output}\n"
        "stderr:\n${integrity_error}")
endif()

message(STATUS "${suite_validator_output}")
message(STATUS "${integrity_output}")
message(STATUS
    "MParser performance suite collected: ${workload_count} reports; "
    "manifest=${manifest_suite_id}; revision=${MPARSER_REVISION}; "
    "quick=${MPARSER_QUICK}; index=${index_path}")
