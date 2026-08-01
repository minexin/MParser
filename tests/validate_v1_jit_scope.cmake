cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        PROJECT_ROOT
        MATRIX
        JIT_SCOPE_DECISION
        NATIVE_SCALAR_REPORT
        NATIVE_ARRAY_REPORT
        NOJIT_ARRAY_REPORT
        EXPECTED_VERSION)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing v1 JIT-scope variable: ${required_variable}")
    endif()
endforeach()

foreach(required_file IN ITEMS
        "${MATRIX}"
        "${JIT_SCOPE_DECISION}"
        "${NATIVE_SCALAR_REPORT}"
        "${NATIVE_ARRAY_REPORT}"
        "${NOJIT_ARRAY_REPORT}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "v1 JIT-scope input is missing: ${required_file}")
    endif()
endforeach()

file(READ "${MATRIX}" jit_scope_matrix)
file(READ "${JIT_SCOPE_DECISION}" jit_scope_decision)
file(READ "${NATIVE_SCALAR_REPORT}" jit_scope_scalar_report)
file(READ "${NATIVE_ARRAY_REPORT}" jit_scope_array_report)
file(READ "${NOJIT_ARRAY_REPORT}" jit_scope_nojit_report)

function(mparser_jit_json_get output_variable document_variable)
    string(JSON json_value GET "${${document_variable}}" ${ARGN})
    set(${output_variable} "${json_value}" PARENT_SCOPE)
endfunction()

function(mparser_jit_require_text needle description)
    string(FIND "${jit_scope_decision}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR
            "v1 JIT-scope decision is missing ${description}: ${needle}")
    endif()
endfunction()

function(mparser_jit_validate_report
         document_variable expected_workload expected_source
         expect_native)
    mparser_jit_json_get(protocol_name
        ${document_variable} protocol name)
    mparser_jit_json_get(protocol_major
        ${document_variable} protocol major)
    mparser_jit_json_get(protocol_minor
        ${document_variable} protocol minor)
    if(NOT protocol_name STREQUAL "mparser.performance-baseline" OR
       NOT protocol_major EQUAL 1 OR NOT protocol_minor EQUAL 0)
        message(FATAL_ERROR
            "v1 JIT-scope report does not use performance protocol 1.0")
    endif()

    mparser_jit_json_get(project_version
        ${document_variable} build project_version)
    mparser_jit_json_get(native_available
        ${document_variable} build native_jit_available)
    mparser_jit_json_get(emulated
        ${document_variable} environment emulated)
    mparser_jit_json_get(workload_id
        ${document_variable} workload id)
    mparser_jit_json_get(source_path
        ${document_variable} workload source_path)
    mparser_jit_json_get(source_sha256
        ${document_variable} workload source_sha256)
    if(NOT project_version STREQUAL EXPECTED_VERSION OR
       NOT workload_id STREQUAL expected_workload OR
       NOT source_path STREQUAL expected_source OR emulated)
        message(FATAL_ERROR
            "v1 JIT-scope report identity or environment drifted: "
            "${workload_id}, ${source_path}, v${project_version}")
    endif()
    if(expect_native AND NOT native_available)
        message(FATAL_ERROR
            "native v1 JIT-scope report says native JIT is unavailable")
    elseif(NOT expect_native AND native_available)
        message(FATAL_ERROR
            "no-JIT v1 scope report says native JIT is available")
    endif()

    file(SHA256 "${PROJECT_ROOT}/${expected_source}" expected_source_sha256)
    if(NOT source_sha256 STREQUAL expected_source_sha256)
        message(FATAL_ERROR
            "v1 JIT-scope report source hash drifted for ${expected_source}")
    endif()

    foreach(correctness_field IN ITEMS
            bytecode_matches
            portable_matches
            native_matches
            all_runtime_results_match)
        mparser_jit_json_get(correctness_value
            ${document_variable} correctness ${correctness_field})
        if(NOT correctness_value)
            message(FATAL_ERROR
                "v1 JIT-scope report failed ${correctness_field}")
        endif()
    endforeach()

    foreach(measured_phase IN ITEMS bytecode portable)
        mparser_jit_json_get(phase_status
            ${document_variable} measurements ${measured_phase} status)
        if(NOT phase_status STREQUAL "measured")
            message(FATAL_ERROR
                "v1 JIT-scope ${measured_phase} phase is not measured")
        endif()
    endforeach()

    mparser_jit_json_get(portable_executions
        ${document_variable} measurements portable execution_summary
        typed_region_execution_count)
    mparser_jit_json_get(portable_fallbacks
        ${document_variable} measurements portable execution_summary
        typed_region_fallback_count)
    if(NOT portable_executions GREATER 0 OR NOT portable_fallbacks EQUAL 0)
        message(FATAL_ERROR
            "portable v1 JIT-scope evidence did not execute without fallback")
    endif()

    if(expect_native)
        foreach(native_phase IN ITEMS native_cold native_warm)
            mparser_jit_json_get(phase_status
                ${document_variable} measurements ${native_phase} status)
            if(NOT phase_status STREQUAL "measured")
                message(FATAL_ERROR
                    "v1 JIT-scope ${native_phase} phase is not measured")
            endif()
        endforeach()
        mparser_jit_json_get(native_executions
            ${document_variable} measurements native_warm execution_summary
            typed_region_execution_count)
        mparser_jit_json_get(native_fallbacks
            ${document_variable} measurements native_warm execution_summary
            typed_region_fallback_count)
        mparser_jit_json_get(cold_compilations
            ${document_variable} measurements native_cold execution_summary
            native_compilation_count)
        mparser_jit_json_get(warm_cache_hits
            ${document_variable} measurements native_warm execution_summary
            native_cache_hit_count)
        mparser_jit_json_get(cache_cold_compilations
            ${document_variable} resources native_cache after_cold
            compilation_count)
        mparser_jit_json_get(cache_warm_hits
            ${document_variable} resources native_cache after_warm hit_count)
        if(NOT native_executions GREATER 0 OR
           NOT native_fallbacks EQUAL 0 OR
           NOT cold_compilations EQUAL 1 OR
           NOT warm_cache_hits GREATER 0 OR
           NOT cache_cold_compilations EQUAL 1 OR
           NOT cache_warm_hits GREATER 0)
            message(FATAL_ERROR
                "native v1 JIT-scope cold/warm cache contract drifted")
        endif()
    else()
        foreach(native_phase IN ITEMS native_cold native_warm)
            mparser_jit_json_get(phase_status
                ${document_variable} measurements ${native_phase} status)
            if(NOT phase_status STREQUAL "unavailable")
                message(FATAL_ERROR
                    "no-JIT report unexpectedly measured ${native_phase}")
            endif()
        endforeach()
        mparser_jit_json_get(cache_compilations
            ${document_variable} resources native_cache after_warm
            compilation_count)
        mparser_jit_json_get(cache_hits
            ${document_variable} resources native_cache after_warm hit_count)
        if(NOT cache_compilations EQUAL 0 OR NOT cache_hits EQUAL 0)
            message(FATAL_ERROR
                "no-JIT report contains native-cache activity")
        endif()
    endif()
endfunction()

function(mparser_jit_require_worst_sample_speedup
         document_variable slow_phase fast_phase multiplier description)
    mparser_jit_json_get(slow_minimum
        ${document_variable} measurements ${slow_phase} host_wall minimum_ns)
    mparser_jit_json_get(fast_maximum
        ${document_variable} measurements ${fast_phase} host_wall maximum_ns)
    math(EXPR required_slow_minimum "${fast_maximum} * ${multiplier}")
    if(NOT slow_minimum GREATER required_slow_minimum)
        message(FATAL_ERROR
            "fixed ${description} evidence no longer exceeds ${multiplier}x "
            "at the conservative worst-sample boundary")
    endif()
endfunction()

set(jit_gap_index -1)
string(JSON jit_gap_count LENGTH "${jit_scope_matrix}" gaps)
math(EXPR jit_gap_last "${jit_gap_count} - 1")
foreach(gap_index RANGE 0 ${jit_gap_last})
    string(JSON gap_id GET "${jit_scope_matrix}" gaps ${gap_index} id)
    if(gap_id STREQUAL "G-JIT-001")
        set(jit_gap_index ${gap_index})
        break()
    endif()
endforeach()
if(jit_gap_index EQUAL -1)
    message(FATAL_ERROR "v1 JIT-scope gap G-JIT-001 is missing")
endif()

foreach(field IN ITEMS priority state target framework_impact)
    string(JSON "jit_gap_${field}" GET
        "${jit_scope_matrix}" gaps ${jit_gap_index} ${field})
endforeach()
if(NOT jit_gap_priority STREQUAL "should-have" OR
   NOT jit_gap_state STREQUAL "deferred" OR
   NOT jit_gap_target STREQUAL "v1.x" OR
   NOT jit_gap_framework_impact STREQUAL "additive")
    message(FATAL_ERROR
        "G-JIT-001 must remain an additive Should-have deferred to v1.x")
endif()

mparser_jit_require_text(
    "Decision: **defer broader typed/JIT coverage to v1.x**."
    "the accepted decision")
mparser_jit_require_text(
    "No v1.0 execution-engine change is authorized by this decision."
    "the v1.0 change boundary")
mparser_jit_require_text(
    "This is a scope decision, not a universal performance threshold"
    "the evidence limitation")

mparser_jit_validate_report(
    jit_scope_scalar_report
    "scalar-loop-v1" "samples/performance_scalar_loop.m" TRUE)
mparser_jit_validate_report(
    jit_scope_array_report
    "linear-array-v1" "samples/performance_array_workload.m" TRUE)
mparser_jit_validate_report(
    jit_scope_nojit_report
    "linear-array-v1" "samples/performance_array_workload.m" FALSE)

foreach(document_variable IN ITEMS
        jit_scope_scalar_report
        jit_scope_array_report
        jit_scope_nojit_report)
    mparser_jit_json_get(report_os
        ${document_variable} environment os)
    mparser_jit_json_get(report_os_version
        ${document_variable} environment os_version)
    mparser_jit_json_get(report_architecture
        ${document_variable} environment architecture)
    mparser_jit_json_get(report_cpu_model
        ${document_variable} environment cpu_model)
    mparser_jit_json_get(report_logical_cpu_count
        ${document_variable} environment logical_cpu_count)
    mparser_jit_json_get(report_compiler_id
        ${document_variable} build compiler_id)
    mparser_jit_json_get(report_compiler_version
        ${document_variable} build compiler_version)
    mparser_jit_json_get(report_build_type
        ${document_variable} build build_type)
    if(NOT report_os STREQUAL "Windows" OR
       NOT report_architecture STREQUAL "x86_64" OR
       NOT report_compiler_id STREQUAL "MSVC" OR
       NOT report_build_type STREQUAL "Release")
        message(FATAL_ERROR
            "v1 JIT-scope report is not the reviewed Windows x86-64 "
            "MSVC Release evidence")
    endif()
    set(report_environment_identity
        "${report_os}|${report_os_version}|${report_architecture}|"
        "${report_cpu_model}|${report_logical_cpu_count}|"
        "${report_compiler_id}|${report_compiler_version}|${report_build_type}")
    if(NOT DEFINED shared_environment_identity)
        set(shared_environment_identity "${report_environment_identity}")
    elseif(NOT report_environment_identity STREQUAL
           shared_environment_identity)
        message(FATAL_ERROR
            "v1 JIT-scope reports do not share one reviewed host/toolchain")
    endif()
endforeach()

foreach(artifact_name IN ITEMS baseline_tool mparser_cli mparser_c_api)
    mparser_jit_json_get(scalar_artifact_sha256
        jit_scope_scalar_report resources binary_artifacts
        ${artifact_name} sha256)
    mparser_jit_json_get(array_artifact_sha256
        jit_scope_array_report resources binary_artifacts
        ${artifact_name} sha256)
    if(NOT scalar_artifact_sha256 STREQUAL array_artifact_sha256)
        message(FATAL_ERROR
            "native scalar/array reports do not share ${artifact_name}")
    endif()
endforeach()

foreach(document_variable IN ITEMS
        jit_scope_scalar_report
        jit_scope_array_report
        jit_scope_nojit_report)
    mparser_jit_json_get(report_revision ${document_variable} revision)
    string(LENGTH "${report_revision}" revision_length)
    if(NOT revision_length EQUAL 40 OR
       NOT report_revision MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR
            "v1 JIT-scope report has an invalid implementation revision")
    endif()
    if(NOT DEFINED shared_report_revision)
        set(shared_report_revision "${report_revision}")
    elseif(NOT report_revision STREQUAL shared_report_revision)
        message(FATAL_ERROR
            "v1 JIT-scope reports do not share one implementation revision")
    endif()
endforeach()

mparser_jit_json_get(native_array_reference
    jit_scope_array_report correctness reference_value)
mparser_jit_json_get(nojit_array_reference
    jit_scope_nojit_report correctness reference_value)
if(NOT native_array_reference STREQUAL nojit_array_reference)
    message(FATAL_ERROR
        "native and no-JIT array reports use different reference values")
endif()

mparser_jit_require_worst_sample_speedup(
    jit_scope_scalar_report bytecode native_warm 100 "scalar native")
mparser_jit_require_worst_sample_speedup(
    jit_scope_array_report bytecode native_warm 100 "array native")
mparser_jit_require_worst_sample_speedup(
    jit_scope_nojit_report bytecode portable 80 "no-JIT portable array")

message(STATUS
    "MParser v1 JIT scope validated: G-JIT-001 deferred to v1.x; "
    "fixed scalar/array native and no-JIT portable evidence retained")
