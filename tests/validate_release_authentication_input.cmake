cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_ARCHIVE
        MPARSER_PROVENANCE
        MPARSER_CHECKSUM
        MPARSER_CHECKSUMS
        MPARSER_PROJECT_ROOT
        MPARSER_EXPECTED_REVISION
        MPARSER_SOURCE_REPOSITORY
        MPARSER_PROJECT_VERSION
        MPARSER_EXPECTED_CONFIGURATION
        MPARSER_EXPECTED_NATIVE_JIT
        MPARSER_REQUIRE_CLEAN_SOURCE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing release-authentication variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED MPARSER_REQUIRE_EXACT_PACKAGE_SET)
    set(MPARSER_REQUIRE_EXACT_PACKAGE_SET OFF)
endif()

foreach(required_file IN ITEMS
        "${MPARSER_ARCHIVE}"
        "${MPARSER_PROVENANCE}"
        "${MPARSER_CHECKSUM}"
        "${MPARSER_CHECKSUMS}")
    if(NOT EXISTS "${required_file}")
        message(FATAL_ERROR
            "Release-authentication input is missing: ${required_file}")
    endif()
endforeach()

get_filename_component(archive "${MPARSER_ARCHIVE}" ABSOLUTE)
get_filename_component(provenance "${MPARSER_PROVENANCE}" ABSOLUTE)
get_filename_component(checksum "${MPARSER_CHECKSUM}" ABSOLUTE)
get_filename_component(checksums "${MPARSER_CHECKSUMS}" ABSOLUTE)
get_filename_component(project_root "${MPARSER_PROJECT_ROOT}" ABSOLUTE)
get_filename_component(archive_directory "${archive}" DIRECTORY)
get_filename_component(provenance_directory "${provenance}" DIRECTORY)
get_filename_component(checksum_directory "${checksum}" DIRECTORY)
get_filename_component(checksums_directory "${checksums}" DIRECTORY)
if(NOT provenance_directory STREQUAL archive_directory OR
   NOT checksum_directory STREQUAL archive_directory OR
   NOT checksums_directory STREQUAL archive_directory)
    message(FATAL_ERROR
        "Release-authentication inputs must share one package directory")
endif()

get_filename_component(archive_name "${archive}" NAME)
get_filename_component(provenance_name "${provenance}" NAME)
get_filename_component(checksum_name "${checksum}" NAME)
get_filename_component(checksums_name "${checksums}" NAME)
if(NOT provenance_name STREQUAL "${archive_name}.provenance.json" OR
   NOT checksum_name STREQUAL "${archive_name}.sha256" OR
   NOT checksums_name STREQUAL "SHA256SUMS")
    message(FATAL_ERROR
        "Release-authentication sidecar names do not match the archive")
endif()
if(MPARSER_REQUIRE_EXACT_PACKAGE_SET)
    file(GLOB package_entries
        LIST_DIRECTORIES TRUE
        RELATIVE "${archive_directory}"
        "${archive_directory}/*")
    set(expected_package_entries
        "${archive_name}"
        "${provenance_name}"
        "${checksum_name}"
        "${checksums_name}")
    list(SORT package_entries)
    list(SORT expected_package_entries)
    if(NOT "${package_entries}" STREQUAL "${expected_package_entries}")
        message(FATAL_ERROR
            "Release-authentication package directory must contain exactly "
            "the archive, SHA-256 sidecar, provenance, and SHA256SUMS.\n"
            "actual: ${package_entries}\n"
            "expected: ${expected_package_entries}")
    endif()
endif()
if(archive_name MATCHES "\\.zip$")
    set(expected_media_type "application/zip")
elseif(archive_name MATCHES "\\.tar\\.gz$")
    set(expected_media_type "application/gzip")
else()
    message(FATAL_ERROR
        "Release-authentication archive type is unsupported: ${archive_name}")
endif()

string(TOLOWER "${MPARSER_EXPECTED_REVISION}" expected_revision)
string(LENGTH "${expected_revision}" revision_length)
if(NOT revision_length EQUAL 40 OR
   NOT expected_revision MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR
        "Release-authentication revision must be a full Git commit")
endif()

file(SHA256 "${archive}" archive_sha256)
file(SIZE "${archive}" archive_size)
file(SHA256 "${provenance}" provenance_sha256)
file(READ "${checksum}" checksum_text)
string(REPLACE "\r\n" "\n" checksum_text "${checksum_text}")
set(expected_checksum "${archive_sha256}  ${archive_name}\n")
if(NOT checksum_text STREQUAL expected_checksum)
    message(FATAL_ERROR
        "Release-authentication archive SHA-256 sidecar is invalid")
endif()
file(READ "${checksums}" checksums_text)
string(REPLACE "\r\n" "\n" checksums_text "${checksums_text}")
string(CONCAT expected_checksums
    "${archive_sha256}  ${archive_name}\n"
    "${provenance_sha256}  ${provenance_name}\n")
if(NOT checksums_text STREQUAL expected_checksums)
    message(FATAL_ERROR
        "Release-authentication SHA256SUMS is invalid")
endif()

file(READ "${provenance}" provenance_json)
string(JSON statement_type GET "${provenance_json}" _type)
string(JSON predicate_type GET "${provenance_json}" predicateType)
if(NOT statement_type STREQUAL "https://in-toto.io/Statement/v1" OR
   NOT predicate_type STREQUAL "https://slsa.dev/provenance/v1")
    message(FATAL_ERROR
        "Release-authentication provenance type is invalid")
endif()

string(JSON subject_count LENGTH "${provenance_json}" subject)
if(NOT subject_count EQUAL 1)
    message(FATAL_ERROR
        "Release-authentication provenance must have one subject")
endif()
string(JSON subject_name GET "${provenance_json}" subject 0 name)
string(JSON subject_sha256 GET
    "${provenance_json}" subject 0 digest sha256)
string(JSON subject_media_type GET
    "${provenance_json}" subject 0 mediaType)
string(JSON subject_size_type TYPE
    "${provenance_json}" subject 0 annotations mparser_sizeBytes)
string(JSON subject_size GET
    "${provenance_json}" subject 0 annotations mparser_sizeBytes)
if(NOT subject_name STREQUAL archive_name OR
   NOT subject_sha256 STREQUAL archive_sha256 OR
   NOT subject_media_type STREQUAL expected_media_type OR
   NOT subject_size_type STREQUAL "NUMBER" OR
   NOT subject_size EQUAL archive_size)
    message(FATAL_ERROR
        "Release-authentication provenance subject does not match archive")
endif()

set(build_path predicate buildDefinition)
set(run_path predicate runDetails)
set(expected_build_type
    "${MPARSER_SOURCE_REPOSITORY}/blob/main/docs/release-process.md#cmake-cpack-release-v1")
set(expected_builder_id
    "${MPARSER_SOURCE_REPOSITORY}/blob/main/docs/release-process.md#local-cmake-builder-v1")
string(JSON build_type GET
    "${provenance_json}" ${build_path} buildType)
string(JSON builder_id GET
    "${provenance_json}" ${run_path} builder id)
string(JSON authentication GET
    "${provenance_json}" ${run_path} mparser_authentication)
string(JSON clean_type TYPE
    "${provenance_json}" ${run_path} mparser_sourceTreeClean)
string(JSON clean_value GET
    "${provenance_json}" ${run_path} mparser_sourceTreeClean)
if(NOT build_type STREQUAL expected_build_type OR
   NOT builder_id STREQUAL expected_builder_id OR
   NOT authentication STREQUAL "unsigned" OR
   NOT clean_type STREQUAL "BOOLEAN")
    message(FATAL_ERROR
        "Release-authentication builder boundary is invalid")
endif()
if(MPARSER_REQUIRE_CLEAN_SOURCE AND NOT clean_value)
    message(FATAL_ERROR
        "Release-authentication input is not source-clean")
endif()

string(JSON recorded_repository GET
    "${provenance_json}" ${build_path} externalParameters repository)
string(JSON recorded_revision GET
    "${provenance_json}" ${build_path} externalParameters revision)
string(JSON recorded_configuration GET
    "${provenance_json}" ${build_path} externalParameters configuration)
string(JSON recorded_version GET
    "${provenance_json}" ${build_path} externalParameters projectVersion)
string(JSON native_jit_type TYPE
    "${provenance_json}" ${build_path} externalParameters nativeJit)
string(JSON native_jit_value GET
    "${provenance_json}" ${build_path} externalParameters nativeJit)
string(JSON release_packaging_type TYPE
    "${provenance_json}" ${build_path} externalParameters releasePackaging)
string(JSON release_packaging_value GET
    "${provenance_json}" ${build_path} externalParameters releasePackaging)
if(NOT recorded_repository STREQUAL MPARSER_SOURCE_REPOSITORY OR
   NOT recorded_revision STREQUAL expected_revision OR
   NOT recorded_configuration STREQUAL MPARSER_EXPECTED_CONFIGURATION OR
   NOT recorded_version STREQUAL MPARSER_PROJECT_VERSION OR
   NOT native_jit_type STREQUAL "BOOLEAN" OR
   NOT release_packaging_type STREQUAL "BOOLEAN" OR
   NOT release_packaging_value OR
   (MPARSER_EXPECTED_NATIVE_JIT AND NOT native_jit_value) OR
   (NOT MPARSER_EXPECTED_NATIVE_JIT AND native_jit_value))
    message(FATAL_ERROR
        "Release-authentication build parameters are invalid")
endif()

string(JSON source_name GET
    "${provenance_json}" ${build_path} resolvedDependencies 0 name)
string(JSON source_uri GET
    "${provenance_json}" ${build_path} resolvedDependencies 0 uri)
string(JSON source_digest GET
    "${provenance_json}" ${build_path}
    resolvedDependencies 0 digest gitCommit)
set(expected_source_uri
    "git+${MPARSER_SOURCE_REPOSITORY}@${expected_revision}")
if(NOT source_name STREQUAL "source-base" OR
   NOT source_uri STREQUAL expected_source_uri OR
   NOT source_digest STREQUAL expected_revision)
    message(FATAL_ERROR
        "Release-authentication source identity is invalid")
endif()

set(contract_paths
    CMakeLists.txt
    CMakePresets.json
    .github/workflows/ci.yml
    docs/public-contract-v1.3.json
    docs/public-contract-v1.2.json
    docs/public-contract-v1.json
    docs/cli-contract-v1.json
    docs/machine-result-v1.schema.json
    docs/compatibility-matrix.json
    docs/release-process.md
    docs/release-authentication.md
    tests/validate_release_authentication_input.cmake)
list(LENGTH contract_paths contract_count)
math(EXPR expected_dependency_count "${contract_count} + 1")
string(JSON dependency_count LENGTH
    "${provenance_json}" ${build_path} resolvedDependencies)
if(NOT dependency_count EQUAL expected_dependency_count)
    message(FATAL_ERROR
        "Release-authentication provenance dependency count is invalid")
endif()
set(dependency_index 1)
foreach(contract_path IN LISTS contract_paths)
    set(contract_file "${project_root}/${contract_path}")
    if(NOT EXISTS "${contract_file}")
        message(FATAL_ERROR
            "Release-authentication contract is missing: ${contract_path}")
    endif()
    string(JSON contract_name GET
        "${provenance_json}" ${build_path}
        resolvedDependencies ${dependency_index} name)
    string(JSON contract_sha256 GET
        "${provenance_json}" ${build_path}
        resolvedDependencies ${dependency_index} digest sha256)
    file(SHA256 "${contract_file}" expected_contract_sha256)
    if(NOT contract_name STREQUAL contract_path OR
       NOT contract_sha256 STREQUAL expected_contract_sha256)
        message(FATAL_ERROR
            "Release-authentication contract identity is invalid: "
            "${contract_path}")
    endif()
    math(EXPR dependency_index "${dependency_index} + 1")
endforeach()

message(STATUS
    "MParser release-authentication input validated: ${archive_name}, "
    "${archive_sha256}, source ${expected_revision}, local provenance unsigned")
