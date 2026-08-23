cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_SUITE_INDEX
        MPARSER_SOURCE_ROOT
        MPARSER_REPORT_ROOT)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "performance suite integrity requires ${required_variable}")
    endif()
endforeach()

if(NOT EXISTS "${MPARSER_SUITE_INDEX}")
    message(FATAL_ERROR
        "performance suite index is missing: ${MPARSER_SUITE_INDEX}")
endif()
if(NOT IS_DIRECTORY "${MPARSER_SOURCE_ROOT}" OR
   NOT IS_DIRECTORY "${MPARSER_REPORT_ROOT}")
    message(FATAL_ERROR
        "performance suite integrity roots must be directories")
endif()

function(read_required_json output document)
    string(JSON value ERROR_VARIABLE json_error GET
        "${document}" ${ARGN})
    if(NOT json_error STREQUAL "NOTFOUND")
        list(JOIN ARGN "/" json_path)
        message(FATAL_ERROR
            "performance suite JSON is missing ${json_path}: ${json_error}")
    endif()
    set(${output} "${value}" PARENT_SCOPE)
endfunction()

function(resolve_below_root output root relative label)
    if(IS_ABSOLUTE "${relative}" OR
       relative MATCHES "(^|/)\\.\\.(/|$)" OR
       relative MATCHES "(^|\\\\)\\.\\.(\\\\|$)")
        message(FATAL_ERROR
            "${label} must be a relative path below its declared root: "
            "${relative}")
    endif()
    file(REAL_PATH "${relative}" resolved BASE_DIRECTORY "${root}")
    file(TO_CMAKE_PATH "${resolved}" resolved)
    string(FIND "${resolved}" "${root}/" root_prefix)
    if(NOT root_prefix EQUAL 0 OR NOT EXISTS "${resolved}")
        message(FATAL_ERROR
            "${label} is missing or outside its declared root: ${relative}")
    endif()
    set(${output} "${resolved}" PARENT_SCOPE)
endfunction()

file(REAL_PATH "${MPARSER_SOURCE_ROOT}" source_root)
file(REAL_PATH "${MPARSER_REPORT_ROOT}" report_root)
file(TO_CMAKE_PATH "${source_root}" source_root)
file(TO_CMAKE_PATH "${report_root}" report_root)
file(READ "${MPARSER_SUITE_INDEX}" index_document)

read_required_json(manifest_relative "${index_document}" manifest path)
read_required_json(manifest_sha256 "${index_document}" manifest sha256)
resolve_below_root(manifest_path "${source_root}"
    "${manifest_relative}" "performance manifest")
file(SHA256 "${manifest_path}" actual_manifest_sha256)
if(NOT actual_manifest_sha256 STREQUAL manifest_sha256)
    message(FATAL_ERROR
        "performance manifest SHA-256 does not match the suite index")
endif()

string(JSON report_count LENGTH "${index_document}" reports)
if(report_count LESS 1)
    message(FATAL_ERROR "performance suite index has no reports")
endif()
math(EXPR last_report "${report_count} - 1")
foreach(report_index RANGE 0 ${last_report})
    read_required_json(workload_id "${index_document}"
        reports ${report_index} workload_id)
    read_required_json(result_variable "${index_document}"
        reports ${report_index} result_variable)
    read_required_json(source_relative "${index_document}"
        reports ${report_index} source_path)
    read_required_json(source_sha256 "${index_document}"
        reports ${report_index} source_sha256)
    read_required_json(report_relative "${index_document}"
        reports ${report_index} report_file)
    read_required_json(report_sha256 "${index_document}"
        reports ${report_index} report_sha256)

    resolve_below_root(source_path "${source_root}"
        "${source_relative}" "performance workload source")
    resolve_below_root(report_path "${report_root}"
        "${report_relative}" "performance workload report")
    file(SHA256 "${source_path}" actual_source_sha256)
    file(SHA256 "${report_path}" actual_report_sha256)
    if(NOT actual_source_sha256 STREQUAL source_sha256 OR
       NOT actual_report_sha256 STREQUAL report_sha256)
        message(FATAL_ERROR
            "performance workload SHA-256 mismatch: ${workload_id}")
    endif()

    file(READ "${report_path}" report_document)
    read_required_json(reported_workload "${report_document}" workload id)
    read_required_json(reported_result_variable "${report_document}"
        workload result_variable)
    read_required_json(reported_source_sha256 "${report_document}"
        workload source_sha256)
    read_required_json(all_results_match "${report_document}"
        correctness all_runtime_results_match)
    if(NOT reported_workload STREQUAL workload_id OR
       NOT reported_result_variable STREQUAL result_variable OR
       NOT reported_source_sha256 STREQUAL source_sha256 OR
       NOT all_results_match)
        message(FATAL_ERROR
            "performance workload identity or correctness mismatch: "
            "${workload_id}")
    endif()
endforeach()

message(STATUS
    "MParser performance suite integrity validated: manifest and "
    "${report_count} source/report SHA-256 bindings")
