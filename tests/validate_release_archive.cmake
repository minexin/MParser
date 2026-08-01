cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_CMAKE_COMMAND
        MPARSER_CTEST_COMMAND
        MPARSER_CPACK_COMMAND
        MPARSER_CPACK_CONFIG
        MPARSER_BUILD_DIR
        MPARSER_PROJECT_ROOT
        MPARSER_TEST_ROOT
        MPARSER_RELEASE_PACKAGE_SCRIPT
        MPARSER_PROVENANCE_CREATOR
        MPARSER_PROVENANCE_VALIDATOR
        MPARSER_GIT_COMMAND
        MPARSER_PACKAGE_BASENAME
        MPARSER_PACKAGE_EXTENSION
        MPARSER_ARCHIVE_MEDIA_TYPE
        MPARSER_SOURCE_REPOSITORY
        MPARSER_BUILD_TYPE_URI
        MPARSER_BUILDER_ID
        MPARSER_NATIVE_JIT
        MPARSER_SOURCE_DATE_EPOCH
        MPARSER_INSTALL_INCLUDEDIR
        MPARSER_INSTALL_BINDIR
        MPARSER_INSTALL_LIBDIR
        MPARSER_INSTALL_DATADIR
        MPARSER_INSTALL_DOCDIR
        MPARSER_INSTALL_CMAKEDIR
        MPARSER_GENERATOR
        MPARSER_CMAKE_VERSION
        MPARSER_COMPILER_ID
        MPARSER_COMPILER_VERSION
        MPARSER_SYSTEM_NAME
        MPARSER_SYSTEM_PROCESSOR
        MPARSER_PROJECT_VERSION
        MPARSER_C_CONSUMER_SOURCE_DIR
        MPARSER_CPP_CONSUMER_SOURCE_DIR)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing release-archive variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED MPARSER_EXECUTABLE_SUFFIX)
    message(FATAL_ERROR
        "Missing release-archive variable: MPARSER_EXECUTABLE_SUFFIX")
endif()
if(NOT DEFINED MPARSER_TEST_CONFIG)
    set(MPARSER_TEST_CONFIG "")
endif()
if(NOT DEFINED MPARSER_SYSTEM_VERSION)
    set(MPARSER_SYSTEM_VERSION "")
endif()
set(mparser_package_config "${MPARSER_TEST_CONFIG}")
if(mparser_package_config STREQUAL "")
    set(mparser_package_config "${MPARSER_BUILD_TYPE}")
endif()

get_filename_component(
    mparser_build_dir "${MPARSER_BUILD_DIR}" ABSOLUTE)
get_filename_component(
    mparser_test_root "${MPARSER_TEST_ROOT}" ABSOLUTE)
file(RELATIVE_PATH
    mparser_test_relative
    "${mparser_build_dir}" "${mparser_test_root}")
if(mparser_test_relative STREQUAL "" OR
   IS_ABSOLUTE "${mparser_test_relative}" OR
   mparser_test_relative MATCHES "^\\.\\.")
    message(FATAL_ERROR
        "Release-archive test root must stay inside the build tree: "
        "${mparser_test_root}")
endif()

function(run_checked description)
    execute_process(
        COMMAND ${ARGN}
        RESULT_VARIABLE command_status
        OUTPUT_VARIABLE command_output
        ERROR_VARIABLE command_error)
    if(NOT command_status EQUAL 0)
        message(FATAL_ERROR
            "${description} failed (${command_status})\n"
            "stdout:\n${command_output}\n"
            "stderr:\n${command_error}")
    endif()
endfunction()

function(run_cpack output_directory)
    set(package_command
        "${MPARSER_CMAKE_COMMAND}"
        "-DMPARSER_CMAKE_COMMAND=${MPARSER_CMAKE_COMMAND}"
        "-DMPARSER_CPACK_COMMAND=${MPARSER_CPACK_COMMAND}"
        "-DMPARSER_CPACK_CONFIG=${MPARSER_CPACK_CONFIG}"
        "-DMPARSER_BUILD_DIR=${mparser_build_dir}"
        "-DMPARSER_OUTPUT_DIR=${output_directory}"
        "-DMPARSER_PROJECT_ROOT=${MPARSER_PROJECT_ROOT}"
        "-DMPARSER_PROVENANCE_CREATOR=${MPARSER_PROVENANCE_CREATOR}"
        "-DMPARSER_PROVENANCE_VALIDATOR=${MPARSER_PROVENANCE_VALIDATOR}"
        "-DMPARSER_GIT_COMMAND=${MPARSER_GIT_COMMAND}"
        "-DMPARSER_PACKAGE_BASENAME=${MPARSER_PACKAGE_BASENAME}"
        "-DMPARSER_PACKAGE_EXTENSION=${MPARSER_PACKAGE_EXTENSION}"
        "-DMPARSER_ARCHIVE_MEDIA_TYPE=${MPARSER_ARCHIVE_MEDIA_TYPE}"
        "-DMPARSER_SOURCE_REPOSITORY=${MPARSER_SOURCE_REPOSITORY}"
        "-DMPARSER_BUILD_TYPE_URI=${MPARSER_BUILD_TYPE_URI}"
        "-DMPARSER_BUILDER_ID=${MPARSER_BUILDER_ID}"
        "-DMPARSER_PROJECT_VERSION=${MPARSER_PROJECT_VERSION}"
        "-DMPARSER_CONFIG=${mparser_package_config}"
        "-DMPARSER_NATIVE_JIT=${MPARSER_NATIVE_JIT}"
        "-DMPARSER_SOURCE_DATE_EPOCH=${MPARSER_SOURCE_DATE_EPOCH}"
        "-DMPARSER_GENERATOR=${MPARSER_GENERATOR}"
        "-DMPARSER_CMAKE_VERSION=${MPARSER_CMAKE_VERSION}"
        "-DMPARSER_COMPILER_ID=${MPARSER_COMPILER_ID}"
        "-DMPARSER_COMPILER_VERSION=${MPARSER_COMPILER_VERSION}"
        "-DMPARSER_SYSTEM_NAME=${MPARSER_SYSTEM_NAME}"
        "-DMPARSER_SYSTEM_VERSION=${MPARSER_SYSTEM_VERSION}"
        "-DMPARSER_SYSTEM_PROCESSOR=${MPARSER_SYSTEM_PROCESSOR}"
        -DMPARSER_REQUIRE_CLEAN_SOURCE=OFF
        -P "${MPARSER_RELEASE_PACKAGE_SCRIPT}")
    run_checked("CPack ${output_directory}" ${package_command})
endfunction()

function(expect_provenance_rejection
         description archive provenance expected_pattern)
    execute_process(
        COMMAND "${MPARSER_CMAKE_COMMAND}"
            "-DMPARSER_ARCHIVE=${archive}"
            "-DMPARSER_PROVENANCE=${provenance}"
            "-DMPARSER_PROJECT_ROOT=${MPARSER_PROJECT_ROOT}"
            "-DMPARSER_GIT_COMMAND=${MPARSER_GIT_COMMAND}"
            "-DMPARSER_SOURCE_REPOSITORY=${MPARSER_SOURCE_REPOSITORY}"
            "-DMPARSER_BUILD_TYPE_URI=${MPARSER_BUILD_TYPE_URI}"
            "-DMPARSER_BUILDER_ID=${MPARSER_BUILDER_ID}"
            "-DMPARSER_PROJECT_VERSION=${MPARSER_PROJECT_VERSION}"
            "-DMPARSER_CONFIG=${mparser_package_config}"
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
            -DMPARSER_REQUIRE_CLEAN_SOURCE=OFF
            -P "${MPARSER_PROVENANCE_VALIDATOR}"
        RESULT_VARIABLE validation_status
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error)
    if(validation_status EQUAL 0)
        message(FATAL_ERROR
            "${description} was accepted by the provenance validator")
    endif()
    set(validation_log "${validation_output}\n${validation_error}")
    if(NOT validation_log MATCHES "${expected_pattern}")
        message(FATAL_ERROR
            "${description} failed for an unexpected reason\n"
            "${validation_log}")
    endif()
endfunction()

function(run_unpacked_consumer
         name source_directory test_root require_cpp)
    set(command
        "${MPARSER_CMAKE_COMMAND}"
        "-DMPARSER_CMAKE_COMMAND=${MPARSER_CMAKE_COMMAND}"
        "-DMPARSER_CTEST_COMMAND=${MPARSER_CTEST_COMMAND}"
        "-DMPARSER_BUILD_DIR=${mparser_build_dir}"
        "-DMPARSER_CONSUMER_SOURCE_DIR=${source_directory}"
        "-DMPARSER_TEST_ROOT=${test_root}"
        "-DMPARSER_EXISTING_PREFIX=${mparser_relocated_prefix}"
        "-DMPARSER_INSTALL_INCLUDEDIR=${MPARSER_INSTALL_INCLUDEDIR}"
        "-DMPARSER_INSTALL_BINDIR=${MPARSER_INSTALL_BINDIR}"
        "-DMPARSER_INSTALL_DATADIR=${MPARSER_INSTALL_DATADIR}"
        "-DMPARSER_INSTALL_DOCDIR=${MPARSER_INSTALL_DOCDIR}"
        "-DMPARSER_INSTALL_CMAKEDIR=${MPARSER_INSTALL_CMAKEDIR}"
        "-DMPARSER_GENERATOR=${MPARSER_GENERATOR}"
        "-DMPARSER_GENERATOR_PLATFORM=${MPARSER_GENERATOR_PLATFORM}"
        "-DMPARSER_GENERATOR_TOOLSET=${MPARSER_GENERATOR_TOOLSET}"
        "-DMPARSER_MAKE_PROGRAM=${MPARSER_MAKE_PROGRAM}"
        "-DMPARSER_C_COMPILER=${MPARSER_C_COMPILER}"
        "-DMPARSER_CXX_COMPILER=${MPARSER_CXX_COMPILER}"
        "-DMPARSER_RC_COMPILER=${MPARSER_RC_COMPILER}"
        "-DMPARSER_MT=${MPARSER_MT}"
        "-DMPARSER_TEST_CONFIG=${MPARSER_TEST_CONFIG}"
        "-DMPARSER_BUILD_TYPE=${MPARSER_BUILD_TYPE}"
        "-DMPARSER_PROJECT_VERSION=${MPARSER_PROJECT_VERSION}")
    if(require_cpp)
        list(APPEND command -DMPARSER_REQUIRE_CPP_SDK=ON)
    endif()
    list(APPEND command
        -P "${MPARSER_PROJECT_ROOT}/tests/run_installed_c_consumer.cmake")
    run_checked("${name} unpacked consumer" ${command})
endfunction()

file(REMOVE_RECURSE "${mparser_test_root}")
file(MAKE_DIRECTORY "${mparser_test_root}")
set(first_package_dir "${mparser_test_root}/package-1")
set(second_package_dir "${mparser_test_root}/package-2")
run_cpack("${first_package_dir}")
run_cpack("${second_package_dir}")

set(first_archive
    "${first_package_dir}/${MPARSER_PACKAGE_BASENAME}${MPARSER_PACKAGE_EXTENSION}")
set(second_archive
    "${second_package_dir}/${MPARSER_PACKAGE_BASENAME}${MPARSER_PACKAGE_EXTENSION}")
set(first_provenance "${first_archive}.provenance.json")
set(second_provenance "${second_archive}.provenance.json")
foreach(archive IN ITEMS "${first_archive}" "${second_archive}")
    if(NOT EXISTS "${archive}" OR
       NOT EXISTS "${archive}.sha256" OR
       NOT EXISTS "${archive}.provenance.json")
        message(FATAL_ERROR
            "Release archive, digest, or provenance is missing: ${archive}")
    endif()
endforeach()
file(SHA256 "${first_archive}" first_hash)
file(SHA256 "${second_archive}" second_hash)
if(NOT first_hash STREQUAL second_hash)
    message(FATAL_ERROR
        "Fixed-payload release archives are not reproducible.\n"
        "first:  ${first_hash}\nsecond: ${second_hash}")
endif()
file(SHA256 "${first_provenance}" first_provenance_hash)
file(SHA256 "${second_provenance}" second_provenance_hash)
if(NOT first_provenance_hash STREQUAL second_provenance_hash)
    message(FATAL_ERROR
        "Fixed-input release provenance is not reproducible.\n"
        "first:  ${first_provenance_hash}\n"
        "second: ${second_provenance_hash}")
endif()
file(READ "${first_archive}.sha256" sidecar)
get_filename_component(first_archive_name "${first_archive}" NAME)
string(REPLACE "\r\n" "\n" sidecar "${sidecar}")
string(STRIP "${sidecar}" sidecar)
set(expected_sidecar "${first_hash}  ${first_archive_name}")
if(NOT sidecar STREQUAL expected_sidecar)
    message(FATAL_ERROR
        "CPack SHA-256 sidecar has unexpected content: ${sidecar}")
endif()
get_filename_component(first_provenance_name
    "${first_provenance}" NAME)
file(READ "${first_package_dir}/SHA256SUMS" checksum_manifest)
string(REPLACE "\r\n" "\n"
    checksum_manifest "${checksum_manifest}")
file(READ "${second_package_dir}/SHA256SUMS" second_checksum_manifest)
string(REPLACE "\r\n" "\n"
    second_checksum_manifest "${second_checksum_manifest}")
string(CONCAT expected_checksum_manifest
    "${first_hash}  ${first_archive_name}\n"
    "${first_provenance_hash}  ${first_provenance_name}\n")
if(NOT checksum_manifest STREQUAL expected_checksum_manifest OR
   NOT second_checksum_manifest STREQUAL expected_checksum_manifest)
    message(FATAL_ERROR
        "SHA256SUMS does not bind the archive and provenance statement")
endif()

set(tampered_archive_dir "${mparser_test_root}/tampered-archive")
file(MAKE_DIRECTORY "${tampered_archive_dir}")
set(tampered_archive
    "${tampered_archive_dir}/${first_archive_name}")
configure_file("${first_archive}" "${tampered_archive}" COPYONLY)
file(APPEND "${tampered_archive}" "mparser-tamper-test")
expect_provenance_rejection(
    "Tampered release archive"
    "${tampered_archive}"
    "${first_provenance}"
    "subject does not match the archive")

file(READ "${first_provenance}" tampered_provenance_json)
string(JSON tampered_provenance_json SET
    "${tampered_provenance_json}"
    predicate runDetails mparser_authentication "\"signed\"")
set(tampered_provenance
    "${mparser_test_root}/tampered.provenance.json")
file(WRITE "${tampered_provenance}"
    "${tampered_provenance_json}\n")
expect_provenance_rejection(
    "Tampered release provenance"
    "${first_archive}"
    "${tampered_provenance}"
    "builder contract changed")

execute_process(
    COMMAND "${MPARSER_CMAKE_COMMAND}" -E tar tf "${first_archive}"
    RESULT_VARIABLE listing_status
    OUTPUT_VARIABLE archive_listing
    ERROR_VARIABLE listing_error)
if(NOT listing_status EQUAL 0)
    message(FATAL_ERROR
        "Unable to list release archive: ${listing_error}")
endif()
string(REPLACE "\r\n" "\n" archive_listing "${archive_listing}")
string(REPLACE "\n" ";" archive_entries "${archive_listing}")
set(entry_count 0)
foreach(entry IN LISTS archive_entries)
    if(entry STREQUAL "")
        continue()
    endif()
    math(EXPR entry_count "${entry_count} + 1")
    string(REPLACE "\\" "/" normalized_entry "${entry}")
    set(expected_entry_prefix
        "${MPARSER_PACKAGE_BASENAME}/")
    string(FIND
        "${normalized_entry}" "${expected_entry_prefix}"
        entry_prefix_offset)
    if(normalized_entry MATCHES "^/" OR
       normalized_entry MATCHES "^[A-Za-z]:" OR
       normalized_entry MATCHES "(^|/)\\.\\.(/|$)" OR
       (NOT normalized_entry STREQUAL
            "${MPARSER_PACKAGE_BASENAME}" AND
        NOT entry_prefix_offset EQUAL 0))
        message(FATAL_ERROR
            "Unsafe or unexpected archive path: ${entry}")
    endif()
endforeach()
if(entry_count LESS 10)
    message(FATAL_ERROR
        "Release archive contains too few entries: ${entry_count}")
endif()

set(extract_root "${mparser_test_root}/extract")
file(MAKE_DIRECTORY "${extract_root}")
file(ARCHIVE_EXTRACT
    INPUT "${first_archive}"
    DESTINATION "${extract_root}")
set(extracted_prefix
    "${extract_root}/${MPARSER_PACKAGE_BASENAME}")
if(NOT IS_DIRECTORY "${extracted_prefix}")
    message(FATAL_ERROR
        "Release archive does not contain its single top-level directory")
endif()
set(mparser_relocated_prefix
    "${mparser_test_root}/relocated-unpacked-sdk")
file(RENAME
    "${extracted_prefix}" "${mparser_relocated_prefix}")

set(required_paths
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_BINDIR}/mparser${MPARSER_EXECUTABLE_SUFFIX}"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_INCLUDEDIR}/mparser/c_api.h"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_INCLUDEDIR}/mparser/cpp_api.hpp"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/LICENSE"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/NOTICE"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/README.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/documentation-index.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/user-manual.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/build-and-install.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/support-matrix.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/cli-reference.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/jit-and-fallback.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/runtime-boundaries.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/migration-v1.0.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/release-process.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/release-notes-v1.0.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/roadmap-v1.x.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/v1.0-documentation.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/v1.0-cross-platform-validation.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/public-contract-v1.json"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/cli-contract-v1.json"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/machine-result-v1.schema.json"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/performance-baseline-v1.schema.json"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/v1.0-performance-baseline.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/v1.0-jit-scope-decision.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/versioning-and-deprecation.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/default_catalog.json"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/compatibility-matrix.json"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/roadmap-v1.0.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/v0.90.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/v1.0-contract-freeze.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/architecture.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DOCDIR}/extending-builtins.md"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_CMAKEDIR}/MParserConfig.cmake"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DATADIR}/mparser/examples/machine_protocol_demo.m"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DATADIR}/mparser/examples/performance_scalar_loop.m"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DATADIR}/mparser/examples/performance_array_workload.m")
foreach(path IN LISTS required_paths)
    if(NOT EXISTS "${path}")
        message(FATAL_ERROR
            "Required unpacked release artifact is missing: ${path}")
    endif()
endforeach()
file(GLOB shared_libraries
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_BINDIR}/*mparser_c*"
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_LIBDIR}/*mparser_c*")
if(NOT shared_libraries)
    message(FATAL_ERROR
        "Unpacked release contains no C shared library")
endif()

run_unpacked_consumer(
    "C11" "${MPARSER_C_CONSUMER_SOURCE_DIR}"
    "${mparser_test_root}/c-consumer" FALSE)
run_unpacked_consumer(
    "C++20" "${MPARSER_CPP_CONSUMER_SOURCE_DIR}"
    "${mparser_test_root}/cpp-consumer" TRUE)

set(unpacked_cli
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_BINDIR}/mparser${MPARSER_EXECUTABLE_SUFFIX}")
set(unpacked_sample
    "${mparser_relocated_prefix}/${MPARSER_INSTALL_DATADIR}/mparser/examples/machine_protocol_demo.m")
execute_process(
    COMMAND "${unpacked_cli}" --run --jit=off
        --result-format=json-v1 "${unpacked_sample}"
    RESULT_VARIABLE cli_status
    OUTPUT_VARIABLE cli_output
    ERROR_VARIABLE cli_error)
if(NOT cli_status EQUAL 0 OR NOT cli_error STREQUAL "")
    message(FATAL_ERROR
        "Unpacked CLI protocol execution failed\n"
        "exit: ${cli_status}\nstdout: ${cli_output}\n"
        "stderr: ${cli_error}")
endif()
string(LENGTH "${cli_output}" cli_length)
math(EXPR cli_terminal_index "${cli_length} - 1")
string(SUBSTRING "${cli_output}" ${cli_terminal_index} 1 cli_terminal)
string(SUBSTRING "${cli_output}" 0 ${cli_terminal_index} cli_document)
if(NOT cli_terminal STREQUAL "\n" OR cli_document MATCHES "[\r\n]")
    message(FATAL_ERROR
        "Unpacked CLI did not emit one JSON line followed by one LF byte")
endif()
string(JSON cli_protocol GET "${cli_document}" protocol name)
string(JSON cli_version GET "${cli_document}" engine version)
string(JSON cli_result_status GET "${cli_document}" status)
if(NOT cli_protocol STREQUAL "mparser.result" OR
   NOT cli_version STREQUAL MPARSER_PROJECT_VERSION OR
   NOT cli_result_status STREQUAL "succeeded")
    message(FATAL_ERROR
        "Unpacked CLI protocol contract changed")
endif()

message(STATUS
    "MParser release archive validated: "
    "${MPARSER_PACKAGE_BASENAME}${MPARSER_PACKAGE_EXTENSION}, "
    "SHA-256 ${first_hash}, reproducible fixed payload, "
    "tamper-rejecting unsigned SLSA provenance, "
    "relocated C11/C++20/CLI consumers")
