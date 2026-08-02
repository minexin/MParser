cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_SCHEMA_VALIDATOR
        MPARSER_SCHEMA
        MPARSER_EVIDENCE_ROOT
        MPARSER_SCALAR_SOURCE
        MPARSER_ARRAY_SOURCE
        MPARSER_EXPECTED_REVISION
        MPARSER_EXPECTED_VERSION)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "cross-platform performance evidence requires "
            "${required_variable}")
    endif()
endforeach()

foreach(required_file IN ITEMS
        "${MPARSER_SCHEMA_VALIDATOR}"
        "${MPARSER_SCHEMA}"
        "${MPARSER_SCALAR_SOURCE}"
        "${MPARSER_ARRAY_SOURCE}"
        "${MPARSER_EVIDENCE_ROOT}/SHA256SUMS")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "cross-platform performance input is missing: "
            "${required_file}")
    endif()
endforeach()

file(SHA256 "${MPARSER_SCALAR_SOURCE}" scalar_source_sha256)
file(SHA256 "${MPARSER_ARRAY_SOURCE}" array_source_sha256)
file(STRINGS "${MPARSER_EVIDENCE_ROOT}/SHA256SUMS" manifest_lines)

if(DEFINED MPARSER_PLATFORM_RECORDS AND
   NOT "${MPARSER_PLATFORM_RECORDS}" STREQUAL "")
    set(platform_records ${MPARSER_PLATFORM_RECORDS})
else()
    set(platform_records
        "mparser-performance-0.90.0-windows-x86_64|Windows|x86_64|MSVC"
        "mparser-performance-0.90.0-linux-x86_64|Linux|x86_64|GNU"
        "mparser-performance-0.90.0-macos-x86_64|Darwin|x86_64|AppleClang"
        "mparser-performance-0.90.0-macos-arm64|Darwin|aarch64|AppleClang")
endif()
if(NOT DEFINED MPARSER_EXPECTED_REPORT_COUNT)
    set(MPARSER_EXPECTED_REPORT_COUNT 8)
endif()
if(NOT DEFINED MPARSER_EXPECTED_PLATFORM_COUNT)
    set(MPARSER_EXPECTED_PLATFORM_COUNT 4)
endif()
if(NOT DEFINED MPARSER_EVIDENCE_LABEL OR
   "${MPARSER_EVIDENCE_LABEL}" STREQUAL "")
    set(MPARSER_EVIDENCE_LABEL
        "MParser cross-platform performance evidence")
endif()
set(report_records
    "native-scalar-loop-v1|scalar-loop-v1|${scalar_source_sha256}"
    "native-linear-array-v1|linear-array-v1|${array_source_sha256}")
set(expected_report_paths)

foreach(platform_record IN LISTS platform_records)
    string(REPLACE "|" ";" platform_fields "${platform_record}")
    list(GET platform_fields 0 platform_directory)
    list(GET platform_fields 1 expected_os)
    list(GET platform_fields 2 expected_architecture)
    list(GET platform_fields 3 expected_compiler)

    set(pair_environment)
    set(pair_build)
    set(pair_binary_artifacts)
    foreach(report_record IN LISTS report_records)
        string(REPLACE "|" ";" report_fields "${report_record}")
        list(GET report_fields 0 report_name)
        list(GET report_fields 1 expected_workload)
        list(GET report_fields 2 expected_source_sha256)
        set(relative_report_path
            "${platform_directory}/${report_name}.json")
        set(report_path
            "${MPARSER_EVIDENCE_ROOT}/${relative_report_path}")
        list(APPEND expected_report_paths "${relative_report_path}")
        if(NOT EXISTS "${report_path}")
            message(FATAL_ERROR
                "cross-platform performance report is missing: "
                "${report_path}")
        endif()

        file(SHA256 "${report_path}" report_sha256)
        list(FIND manifest_lines
            "${report_sha256}  ${relative_report_path}"
            manifest_index)
        if(manifest_index EQUAL -1)
            message(FATAL_ERROR
                "cross-platform performance manifest mismatch: "
                "${relative_report_path}")
        endif()

        execute_process(
            COMMAND "${MPARSER_SCHEMA_VALIDATOR}"
                --require-native-cache
                "${MPARSER_SCHEMA}"
                "${report_path}"
            RESULT_VARIABLE validator_result
            OUTPUT_VARIABLE validator_output
            ERROR_VARIABLE validator_error
            TIMEOUT 60)
        if(NOT validator_result EQUAL 0)
            message(FATAL_ERROR
                "cross-platform performance schema/semantic validation "
                "failed for ${relative_report_path} "
                "(${validator_result})\n"
                "stdout:\n${validator_output}\n"
                "stderr:\n${validator_error}")
        endif()

        file(READ "${report_path}" report_json)
        string(JSON reported_revision GET "${report_json}" revision)
        string(JSON reported_version GET
            "${report_json}" build project_version)
        string(JSON reported_build_type GET
            "${report_json}" build build_type)
        string(JSON reported_os GET
            "${report_json}" environment os)
        string(JSON reported_os_version GET
            "${report_json}" environment os_version)
        string(JSON reported_architecture GET
            "${report_json}" environment architecture)
        string(JSON reported_cpu_model GET
            "${report_json}" environment cpu_model)
        string(JSON reported_logical_cpus GET
            "${report_json}" environment logical_cpu_count)
        string(JSON reported_physical_memory GET
            "${report_json}" environment physical_memory_bytes)
        string(JSON reported_emulated GET
            "${report_json}" environment emulated)
        string(JSON reported_compiler GET
            "${report_json}" build compiler_id)
        string(JSON reported_compiler_version GET
            "${report_json}" build compiler_version)
        string(JSON native_available GET
            "${report_json}" build native_jit_available)
        string(JSON native_platform GET
            "${report_json}" build native_jit_platform)
        string(JSON reported_workload GET
            "${report_json}" workload id)
        string(JSON reported_source_sha256 GET
            "${report_json}" workload source_sha256)
        if(NOT reported_revision STREQUAL MPARSER_EXPECTED_REVISION OR
           NOT reported_version STREQUAL MPARSER_EXPECTED_VERSION OR
           NOT reported_build_type STREQUAL "Release" OR
           NOT reported_os STREQUAL expected_os OR
           NOT reported_architecture STREQUAL expected_architecture OR
           reported_cpu_model STREQUAL "" OR
           reported_cpu_model STREQUAL "unknown" OR
           reported_emulated OR
           NOT reported_compiler STREQUAL expected_compiler OR
           NOT native_available OR
           NOT reported_workload STREQUAL expected_workload OR
           NOT reported_source_sha256 STREQUAL expected_source_sha256)
            message(FATAL_ERROR
                "cross-platform performance identity mismatch for "
                "${relative_report_path}\n"
                "revision: ${reported_revision}\n"
                "version/build: ${reported_version} "
                "${reported_build_type}\n"
                "platform: ${reported_os} ${reported_architecture} "
                "(emulated=${reported_emulated})\n"
                "compiler/native: ${reported_compiler} "
                "${native_available}\n"
                "workload/source: ${reported_workload} "
                "${reported_source_sha256}\n"
                "expected revision: ${MPARSER_EXPECTED_REVISION}\n"
                "expected version/build: ${MPARSER_EXPECTED_VERSION} "
                "Release\n"
                "expected platform: ${expected_os} "
                "${expected_architecture} (emulated=OFF)\n"
                "expected compiler/native: ${expected_compiler} ON\n"
                "expected workload/source: ${expected_workload} "
                "${expected_source_sha256}")
        endif()

        foreach(phase IN ITEMS
                parse
                compile
                process_cold_start
                bytecode
                portable
                native_cold
                native_warm)
            string(JSON phase_status GET
                "${report_json}" measurements ${phase} status)
            string(JSON phase_median GET
                "${report_json}" measurements ${phase}
                host_wall median_ns)
            if(NOT phase_status STREQUAL "measured" OR
               phase_median LESS_EQUAL 0)
                message(FATAL_ERROR
                    "cross-platform performance phase is not measured: "
                    "${relative_report_path} ${phase}")
            endif()
        endforeach()

        foreach(runtime_phase IN ITEMS
                bytecode portable native_cold native_warm)
            string(JSON fallback_iterations GET
                "${report_json}" measurements ${runtime_phase}
                execution_summary fallback_iterations)
            string(JSON typed_fallbacks GET
                "${report_json}" measurements ${runtime_phase}
                execution_summary typed_region_fallback_count)
            if(NOT fallback_iterations EQUAL 0 OR
               NOT typed_fallbacks EQUAL 0)
                message(FATAL_ERROR
                    "cross-platform performance report contains fallback: "
                    "${relative_report_path} ${runtime_phase}")
            endif()
        endforeach()

        string(JSON cold_compilations GET
            "${report_json}" measurements native_cold
            execution_summary native_compilation_count)
        string(JSON warm_cache_hits GET
            "${report_json}" measurements native_warm
            execution_summary native_cache_hit_count)
        string(JSON cache_entries_before GET
            "${report_json}" resources native_cache before entry_count)
        string(JSON cache_entries_after_cold GET
            "${report_json}" resources native_cache after_cold entry_count)
        string(JSON cache_entries_after_warm GET
            "${report_json}" resources native_cache after_warm entry_count)
        string(JSON results_match GET
            "${report_json}" correctness all_runtime_results_match)
        string(JSON process_successes GET
            "${report_json}" correctness process_exit_success_count)
        string(JSON process_iterations GET
            "${report_json}" settings process_iterations)
        if(cold_compilations LESS 1 OR
           warm_cache_hits LESS 1 OR
           NOT cache_entries_before EQUAL 0 OR
           cache_entries_after_cold LESS 1 OR
           NOT cache_entries_after_cold EQUAL
               cache_entries_after_warm OR
           NOT results_match OR
           NOT process_successes EQUAL process_iterations)
            message(FATAL_ERROR
                "cross-platform performance correctness/cache invariant "
                "failed: ${relative_report_path}")
        endif()

        set(report_environment
            "${reported_os}|${reported_os_version}|"
            "${reported_architecture}|${reported_cpu_model}|"
            "${reported_logical_cpus}|${reported_physical_memory}|"
            "${reported_emulated}")
        set(report_build
            "${reported_version}|${reported_build_type}|"
            "${reported_compiler}|${reported_compiler_version}|"
            "${native_available}|${native_platform}")
        set(report_binary_artifacts)
        foreach(artifact_name IN ITEMS
                baseline_tool mparser_cli mparser_c_api)
            string(JSON artifact_path GET
                "${report_json}" resources binary_artifacts
                ${artifact_name} path)
            string(JSON artifact_size GET
                "${report_json}" resources binary_artifacts
                ${artifact_name} size_bytes)
            string(JSON artifact_sha256 GET
                "${report_json}" resources binary_artifacts
                ${artifact_name} sha256)
            string(APPEND report_binary_artifacts
                "${artifact_name}|${artifact_path}|${artifact_size}|"
                "${artifact_sha256}|")
        endforeach()
        if("${pair_environment}" STREQUAL "")
            set(pair_environment "${report_environment}")
            set(pair_build "${report_build}")
            set(pair_binary_artifacts "${report_binary_artifacts}")
        elseif(NOT "${report_environment}" STREQUAL
                   "${pair_environment}")
            message(FATAL_ERROR
                "cross-platform performance report pair does not share "
                "one environment identity: ${platform_directory}\n"
                "first: ${pair_environment}\n"
                "current: ${report_environment}")
        elseif(NOT "${report_build}" STREQUAL "${pair_build}")
            message(FATAL_ERROR
                "cross-platform performance report pair does not share "
                "one build identity: ${platform_directory}")
        elseif(NOT "${report_binary_artifacts}" STREQUAL
                   "${pair_binary_artifacts}")
            message(FATAL_ERROR
                "cross-platform performance report pair does not share "
                "one binary identity: ${platform_directory}")
        endif()
    endforeach()
endforeach()

file(GLOB_RECURSE actual_report_paths
    RELATIVE "${MPARSER_EVIDENCE_ROOT}"
    "${MPARSER_EVIDENCE_ROOT}/*.json")
list(SORT actual_report_paths)
list(SORT expected_report_paths)
list(SORT manifest_lines)
list(LENGTH expected_report_paths expected_report_count)
list(LENGTH actual_report_paths actual_report_count)
list(LENGTH manifest_lines manifest_count)
list(LENGTH platform_records actual_platform_count)
if(NOT actual_report_paths STREQUAL expected_report_paths OR
   NOT actual_report_count EQUAL MPARSER_EXPECTED_REPORT_COUNT OR
   NOT expected_report_count EQUAL MPARSER_EXPECTED_REPORT_COUNT OR
   NOT manifest_count EQUAL MPARSER_EXPECTED_REPORT_COUNT OR
   NOT actual_platform_count EQUAL MPARSER_EXPECTED_PLATFORM_COUNT)
    message(FATAL_ERROR
        "cross-platform performance evidence set drifted\n"
        "expected: ${expected_report_paths}\n"
        "actual: ${actual_report_paths}\n"
        "manifest entries: ${manifest_count}")
endif()

message(STATUS
    "${MPARSER_EVIDENCE_LABEL} validated: "
    "${MPARSER_EXPECTED_REPORT_COUNT} reports, "
    "${MPARSER_EXPECTED_PLATFORM_COUNT} native non-emulated "
    "platform/compiler identities, "
    "revision=${MPARSER_EXPECTED_REVISION}")
