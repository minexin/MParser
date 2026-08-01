cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_CMAKE_COMMAND
        MPARSER_CPACK_COMMAND
        MPARSER_CPACK_CONFIG
        MPARSER_BUILD_DIR
        MPARSER_OUTPUT_DIR
        MPARSER_PROJECT_ROOT
        MPARSER_PROVENANCE_CREATOR
        MPARSER_PROVENANCE_VALIDATOR
        MPARSER_GIT_COMMAND
        MPARSER_PACKAGE_BASENAME
        MPARSER_PACKAGE_EXTENSION
        MPARSER_ARCHIVE_MEDIA_TYPE
        MPARSER_SOURCE_REPOSITORY
        MPARSER_BUILD_TYPE_URI
        MPARSER_BUILDER_ID
        MPARSER_PROJECT_VERSION
        MPARSER_CONFIG
        MPARSER_NATIVE_JIT
        MPARSER_SOURCE_DATE_EPOCH
        MPARSER_GENERATOR
        MPARSER_CMAKE_VERSION
        MPARSER_COMPILER_ID
        MPARSER_COMPILER_VERSION
        MPARSER_SYSTEM_NAME
        MPARSER_SYSTEM_PROCESSOR
        MPARSER_REQUIRE_CLEAN_SOURCE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing release-package variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED MPARSER_SYSTEM_VERSION)
    set(MPARSER_SYSTEM_VERSION "")
endif()

if(MPARSER_REQUIRE_CLEAN_SOURCE)
    execute_process(
        COMMAND "${MPARSER_GIT_COMMAND}" -C "${MPARSER_PROJECT_ROOT}"
            status --porcelain --untracked-files=normal
        RESULT_VARIABLE source_status
        OUTPUT_VARIABLE source_changes
        ERROR_VARIABLE source_error
        OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT source_status EQUAL 0)
        message(FATAL_ERROR
            "Unable to inspect publication source: ${source_error}")
    endif()
    if(NOT source_changes STREQUAL "")
        message(FATAL_ERROR
            "Publication package requires a clean Git worktree.\n"
            "${source_changes}")
    endif()
endif()

get_filename_component(
    mparser_build_dir "${MPARSER_BUILD_DIR}" ABSOLUTE)
get_filename_component(
    mparser_output_dir "${MPARSER_OUTPUT_DIR}" ABSOLUTE)
file(RELATIVE_PATH
    mparser_output_relative
    "${mparser_build_dir}" "${mparser_output_dir}")
if(mparser_output_relative STREQUAL "" OR
   IS_ABSOLUTE "${mparser_output_relative}" OR
   mparser_output_relative MATCHES "^\\.\\.")
    message(FATAL_ERROR
        "Release-package output must stay inside the build tree: "
        "${mparser_output_dir}")
endif()

file(REMOVE_RECURSE "${mparser_output_dir}")
file(MAKE_DIRECTORY "${mparser_output_dir}")
set(package_command
    "${MPARSER_CMAKE_COMMAND}" -E env
    "SOURCE_DATE_EPOCH=${MPARSER_SOURCE_DATE_EPOCH}"
    "${MPARSER_CPACK_COMMAND}"
    --config "${MPARSER_CPACK_CONFIG}"
    -B "${mparser_output_dir}")
if(DEFINED MPARSER_CONFIG AND NOT MPARSER_CONFIG STREQUAL "")
    list(APPEND package_command -C "${MPARSER_CONFIG}")
endif()
execute_process(
    COMMAND ${package_command}
    RESULT_VARIABLE package_status
    OUTPUT_VARIABLE package_output
    ERROR_VARIABLE package_error)
if(NOT package_status EQUAL 0)
    message(FATAL_ERROR
        "CPack failed (${package_status})\n"
        "stdout:\n${package_output}\n"
        "stderr:\n${package_error}")
endif()

set(archive
    "${mparser_output_dir}/${MPARSER_PACKAGE_BASENAME}${MPARSER_PACKAGE_EXTENSION}")
set(sidecar "${archive}.sha256")
if(NOT EXISTS "${archive}" OR NOT EXISTS "${sidecar}")
    message(FATAL_ERROR
        "Expected release archive or SHA-256 sidecar is missing: "
        "${archive}")
endif()
file(SHA256 "${archive}" archive_hash)
get_filename_component(archive_name "${archive}" NAME)
set(provenance "${archive}.provenance.json")
set(provenance_arguments
    "-DMPARSER_ARCHIVE=${archive}"
    "-DMPARSER_PROJECT_ROOT=${MPARSER_PROJECT_ROOT}"
    "-DMPARSER_GIT_COMMAND=${MPARSER_GIT_COMMAND}"
    "-DMPARSER_SOURCE_REPOSITORY=${MPARSER_SOURCE_REPOSITORY}"
    "-DMPARSER_BUILD_TYPE_URI=${MPARSER_BUILD_TYPE_URI}"
    "-DMPARSER_BUILDER_ID=${MPARSER_BUILDER_ID}"
    "-DMPARSER_PROJECT_VERSION=${MPARSER_PROJECT_VERSION}"
    "-DMPARSER_CONFIG=${MPARSER_CONFIG}"
    "-DMPARSER_NATIVE_JIT=${MPARSER_NATIVE_JIT}"
    "-DMPARSER_SOURCE_DATE_EPOCH=${MPARSER_SOURCE_DATE_EPOCH}"
    "-DMPARSER_GENERATOR=${MPARSER_GENERATOR}"
    "-DMPARSER_CMAKE_VERSION=${MPARSER_CMAKE_VERSION}"
    "-DMPARSER_COMPILER_ID=${MPARSER_COMPILER_ID}"
    "-DMPARSER_COMPILER_VERSION=${MPARSER_COMPILER_VERSION}"
    "-DMPARSER_SYSTEM_NAME=${MPARSER_SYSTEM_NAME}"
    "-DMPARSER_SYSTEM_VERSION=${MPARSER_SYSTEM_VERSION}"
    "-DMPARSER_SYSTEM_PROCESSOR=${MPARSER_SYSTEM_PROCESSOR}"
    "-DMPARSER_ARCHIVE_MEDIA_TYPE=${MPARSER_ARCHIVE_MEDIA_TYPE}"
    "-DMPARSER_REQUIRE_CLEAN_SOURCE=${MPARSER_REQUIRE_CLEAN_SOURCE}")
execute_process(
    COMMAND "${MPARSER_CMAKE_COMMAND}"
        ${provenance_arguments}
        "-DMPARSER_PROVENANCE_OUTPUT=${provenance}"
        -P "${MPARSER_PROVENANCE_CREATOR}"
    RESULT_VARIABLE provenance_status
    OUTPUT_VARIABLE provenance_output
    ERROR_VARIABLE provenance_error)
if(NOT provenance_status EQUAL 0)
    message(FATAL_ERROR
        "Release provenance generation failed (${provenance_status})\n"
        "stdout:\n${provenance_output}\n"
        "stderr:\n${provenance_error}")
endif()
execute_process(
    COMMAND "${MPARSER_CMAKE_COMMAND}"
        ${provenance_arguments}
        "-DMPARSER_PROVENANCE=${provenance}"
        -P "${MPARSER_PROVENANCE_VALIDATOR}"
    RESULT_VARIABLE validation_status
    OUTPUT_VARIABLE validation_output
    ERROR_VARIABLE validation_error)
if(NOT validation_status EQUAL 0)
    message(FATAL_ERROR
        "Release provenance validation failed (${validation_status})\n"
        "stdout:\n${validation_output}\n"
        "stderr:\n${validation_error}")
endif()
file(SHA256 "${provenance}" provenance_hash)
get_filename_component(provenance_name "${provenance}" NAME)
file(WRITE "${mparser_output_dir}/SHA256SUMS"
    "${archive_hash}  ${archive_name}\n"
    "${provenance_hash}  ${provenance_name}\n")
message(STATUS
    "MParser release package: ${archive_name} ${archive_hash}; "
    "${provenance_name} ${provenance_hash}")
