cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_ARCHIVE
        MPARSER_PROVENANCE
        MPARSER_PROJECT_ROOT
        MPARSER_GIT_COMMAND
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
        MPARSER_ARCHIVE_MEDIA_TYPE
        MPARSER_REQUIRE_CLEAN_SOURCE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing provenance-validation variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED MPARSER_SYSTEM_VERSION)
    set(MPARSER_SYSTEM_VERSION "")
endif()
if(NOT EXISTS "${MPARSER_ARCHIVE}" OR
   NOT EXISTS "${MPARSER_PROVENANCE}")
    message(FATAL_ERROR
        "Release archive or provenance statement is missing")
endif()

file(READ "${MPARSER_PROVENANCE}" provenance_json)
string(JSON statement_type GET "${provenance_json}" _type)
string(JSON predicate_type GET
    "${provenance_json}" predicateType)
if(NOT statement_type STREQUAL "https://in-toto.io/Statement/v1" OR
   NOT predicate_type STREQUAL "https://slsa.dev/provenance/v1")
    message(FATAL_ERROR
        "Release provenance uses an unexpected statement or predicate type")
endif()

string(JSON subject_count LENGTH "${provenance_json}" subject)
if(NOT subject_count EQUAL 1)
    message(FATAL_ERROR
        "Release provenance must describe exactly one archive subject")
endif()
get_filename_component(archive_name "${MPARSER_ARCHIVE}" NAME)
file(SHA256 "${MPARSER_ARCHIVE}" archive_sha256)
file(SIZE "${MPARSER_ARCHIVE}" archive_size)
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
   NOT subject_media_type STREQUAL MPARSER_ARCHIVE_MEDIA_TYPE OR
   NOT subject_size_type STREQUAL "NUMBER" OR
   NOT subject_size EQUAL archive_size)
    message(FATAL_ERROR
        "Release provenance subject does not match the archive")
endif()

set(build_path predicate buildDefinition)
string(JSON build_type GET
    "${provenance_json}" ${build_path} buildType)
if(NOT build_type STREQUAL MPARSER_BUILD_TYPE_URI)
    message(FATAL_ERROR "Release provenance buildType changed")
endif()
foreach(expectation IN ITEMS
        "repository;${MPARSER_SOURCE_REPOSITORY}"
        "configuration;${MPARSER_CONFIG}"
        "sourceDateEpoch;${MPARSER_SOURCE_DATE_EPOCH}"
        "projectVersion;${MPARSER_PROJECT_VERSION}")
    list(GET expectation 0 key)
    list(GET expectation 1 expected_value)
    string(JSON actual_value GET
        "${provenance_json}" ${build_path}
        externalParameters "${key}")
    if(NOT actual_value STREQUAL expected_value)
        message(FATAL_ERROR
            "Release provenance external parameter ${key} changed")
    endif()
endforeach()
string(JSON native_type TYPE
    "${provenance_json}" ${build_path} externalParameters nativeJit)
string(JSON native_value GET
    "${provenance_json}" ${build_path} externalParameters nativeJit)
string(JSON packaging_type TYPE
    "${provenance_json}" ${build_path}
    externalParameters releasePackaging)
string(JSON packaging_value GET
    "${provenance_json}" ${build_path}
    externalParameters releasePackaging)
if(NOT native_type STREQUAL "BOOLEAN" OR
   NOT packaging_type STREQUAL "BOOLEAN" OR
   NOT packaging_value)
    message(FATAL_ERROR
        "Release provenance boolean build parameters are invalid")
endif()
if((MPARSER_NATIVE_JIT AND NOT native_value) OR
   (NOT MPARSER_NATIVE_JIT AND native_value))
    message(FATAL_ERROR
        "Release provenance native-JIT parameter changed")
endif()

foreach(expectation IN ITEMS
        "generator;${MPARSER_GENERATOR}"
        "compilerId;${MPARSER_COMPILER_ID}"
        "compilerVersion;${MPARSER_COMPILER_VERSION}"
        "systemName;${MPARSER_SYSTEM_NAME}"
        "systemVersion;${MPARSER_SYSTEM_VERSION}"
        "systemProcessor;${MPARSER_SYSTEM_PROCESSOR}")
    list(GET expectation 0 key)
    list(GET expectation 1 expected_value)
    string(JSON actual_value GET
        "${provenance_json}" ${build_path}
        internalParameters "${key}")
    if(NOT actual_value STREQUAL expected_value)
        message(FATAL_ERROR
            "Release provenance internal parameter ${key} changed")
    endif()
endforeach()

execute_process(
    COMMAND "${MPARSER_GIT_COMMAND}" -C "${MPARSER_PROJECT_ROOT}"
        rev-parse --verify HEAD
    RESULT_VARIABLE revision_status
    OUTPUT_VARIABLE expected_revision
    ERROR_VARIABLE revision_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT revision_status EQUAL 0)
    message(FATAL_ERROR
        "Unable to verify provenance source revision: ${revision_error}")
endif()
string(TOLOWER "${expected_revision}" expected_revision)
string(JSON recorded_revision GET
    "${provenance_json}" ${build_path} externalParameters revision)
string(JSON source_name GET
    "${provenance_json}" ${build_path} resolvedDependencies 0 name)
string(JSON source_uri GET
    "${provenance_json}" ${build_path} resolvedDependencies 0 uri)
string(JSON source_digest GET
    "${provenance_json}" ${build_path}
    resolvedDependencies 0 digest gitCommit)
set(expected_source_uri
    "git+${MPARSER_SOURCE_REPOSITORY}@${expected_revision}")
if(NOT recorded_revision STREQUAL expected_revision OR
   NOT source_name STREQUAL "source-base" OR
   NOT source_uri STREQUAL expected_source_uri OR
   NOT source_digest STREQUAL expected_revision)
    message(FATAL_ERROR
        "Release provenance source dependency changed")
endif()

set(contract_paths
    CMakeLists.txt
    CMakePresets.json
    .github/workflows/ci.yml
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
        "Release provenance dependency set changed")
endif()
set(seen_contracts)
math(EXPR dependency_last "${dependency_count} - 1")
foreach(dependency_index RANGE 1 ${dependency_last})
    string(JSON contract_name GET
        "${provenance_json}" ${build_path}
        resolvedDependencies ${dependency_index} name)
    string(JSON contract_sha256 GET
        "${provenance_json}" ${build_path}
        resolvedDependencies ${dependency_index} digest sha256)
    list(FIND contract_paths "${contract_name}" contract_index)
    list(FIND seen_contracts "${contract_name}" duplicate_index)
    if(contract_index EQUAL -1 OR NOT duplicate_index EQUAL -1)
        message(FATAL_ERROR
            "Release provenance has an unexpected contract input: "
            "${contract_name}")
    endif()
    file(SHA256
        "${MPARSER_PROJECT_ROOT}/${contract_name}"
        expected_contract_sha256)
    if(NOT contract_sha256 STREQUAL expected_contract_sha256)
        message(FATAL_ERROR
            "Release provenance contract digest changed: ${contract_name}")
    endif()
    list(APPEND seen_contracts "${contract_name}")
endforeach()

set(run_path predicate runDetails)
string(JSON builder_id GET
    "${provenance_json}" ${run_path} builder id)
string(JSON cmake_version GET
    "${provenance_json}" ${run_path} builder version cmake)
string(JSON compiler_version GET
    "${provenance_json}" ${run_path} builder version compiler)
set(expected_compiler_version
    "${MPARSER_COMPILER_ID} ${MPARSER_COMPILER_VERSION}")
string(JSON authentication GET
    "${provenance_json}" ${run_path} mparser_authentication)
string(JSON clean_type TYPE
    "${provenance_json}" ${run_path} mparser_sourceTreeClean)
string(JSON clean_value GET
    "${provenance_json}" ${run_path} mparser_sourceTreeClean)
if(NOT builder_id STREQUAL MPARSER_BUILDER_ID OR
   NOT cmake_version STREQUAL MPARSER_CMAKE_VERSION OR
   NOT compiler_version STREQUAL expected_compiler_version OR
   NOT authentication STREQUAL "unsigned" OR
   NOT clean_type STREQUAL "BOOLEAN")
    message(FATAL_ERROR
        "Release provenance builder contract changed")
endif()

execute_process(
    COMMAND "${MPARSER_GIT_COMMAND}" -C "${MPARSER_PROJECT_ROOT}"
        status --porcelain --untracked-files=normal
    RESULT_VARIABLE status_result
    OUTPUT_VARIABLE worktree_status
    ERROR_VARIABLE status_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT status_result EQUAL 0)
    message(FATAL_ERROR
        "Unable to verify provenance worktree state: ${status_error}")
endif()
if(worktree_status STREQUAL "")
    set(expected_clean true)
else()
    set(expected_clean false)
endif()
if((expected_clean AND NOT clean_value) OR
   (NOT expected_clean AND clean_value))
    message(FATAL_ERROR
        "Release provenance worktree state is inaccurate")
endif()
if(MPARSER_REQUIRE_CLEAN_SOURCE AND NOT clean_value)
    message(FATAL_ERROR
        "Publishable release provenance is not source-clean")
endif()

message(STATUS
    "MParser release provenance validated: "
    "${archive_name}, ${archive_sha256}, source ${expected_revision}, "
    "authentication unsigned")
