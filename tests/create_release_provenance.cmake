cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        MPARSER_ARCHIVE
        MPARSER_PROVENANCE_OUTPUT
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
            "Missing release-provenance variable: ${required_variable}")
    endif()
endforeach()
if(NOT DEFINED MPARSER_SYSTEM_VERSION)
    set(MPARSER_SYSTEM_VERSION "")
endif()

function(mparser_json_quote output value)
    set(escaped "${value}")
    string(REPLACE "\\" "\\\\" escaped "${escaped}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    string(REPLACE "\r" "\\r" escaped "${escaped}")
    string(REPLACE "\n" "\\n" escaped "${escaped}")
    string(REPLACE "\t" "\\t" escaped "${escaped}")
    set(${output} "\"${escaped}\"" PARENT_SCOPE)
endfunction()

get_filename_component(archive "${MPARSER_ARCHIVE}" ABSOLUTE)
get_filename_component(
    provenance_output "${MPARSER_PROVENANCE_OUTPUT}" ABSOLUTE)
get_filename_component(archive_directory "${archive}" DIRECTORY)
get_filename_component(provenance_directory
    "${provenance_output}" DIRECTORY)
if(NOT EXISTS "${archive}")
    message(FATAL_ERROR "Release archive is missing: ${archive}")
endif()
if(NOT provenance_directory STREQUAL archive_directory)
    message(FATAL_ERROR
        "Release provenance must be written beside its archive")
endif()

execute_process(
    COMMAND "${MPARSER_GIT_COMMAND}" -C "${MPARSER_PROJECT_ROOT}"
        rev-parse --verify HEAD
    RESULT_VARIABLE revision_status
    OUTPUT_VARIABLE source_revision
    ERROR_VARIABLE revision_error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT revision_status EQUAL 0)
    message(FATAL_ERROR
        "Unable to resolve release source revision: ${revision_error}")
endif()
string(TOLOWER "${source_revision}" source_revision)
string(LENGTH "${source_revision}" revision_length)
if(NOT source_revision MATCHES "^[0-9a-f]+$" OR
   revision_length LESS 40)
    message(FATAL_ERROR
        "Unexpected Git source revision: ${source_revision}")
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
        "Unable to inspect release source worktree: ${status_error}")
endif()
if(worktree_status STREQUAL "")
    set(source_tree_clean true)
else()
    set(source_tree_clean false)
endif()
if(MPARSER_REQUIRE_CLEAN_SOURCE AND NOT source_tree_clean)
    message(FATAL_ERROR
        "Publication provenance requires a clean Git worktree.\n"
        "${worktree_status}")
endif()

file(SHA256 "${archive}" archive_sha256)
file(SIZE "${archive}" archive_size)
get_filename_component(archive_name "${archive}" NAME)

set(subject_digest "{}")
mparser_json_quote(quoted_archive_hash "${archive_sha256}")
string(JSON subject_digest SET
    "${subject_digest}" sha256 "${quoted_archive_hash}")
set(subject_annotations "{}")
string(JSON subject_annotations SET
    "${subject_annotations}" mparser_sizeBytes "${archive_size}")
set(subject "{}")
mparser_json_quote(quoted_archive_name "${archive_name}")
mparser_json_quote(quoted_media_type "${MPARSER_ARCHIVE_MEDIA_TYPE}")
string(JSON subject SET "${subject}" name "${quoted_archive_name}")
string(JSON subject SET "${subject}" digest "${subject_digest}")
string(JSON subject SET "${subject}" mediaType "${quoted_media_type}")
string(JSON subject SET
    "${subject}" annotations "${subject_annotations}")
set(subjects "[]")
string(JSON subjects SET "${subjects}" 0 "${subject}")

set(external_parameters "{}")
foreach(pair IN ITEMS
        "repository;${MPARSER_SOURCE_REPOSITORY}"
        "revision;${source_revision}"
        "configuration;${MPARSER_CONFIG}"
        "sourceDateEpoch;${MPARSER_SOURCE_DATE_EPOCH}"
        "projectVersion;${MPARSER_PROJECT_VERSION}")
    list(GET pair 0 key)
    list(GET pair 1 value)
    mparser_json_quote(quoted_value "${value}")
    string(JSON external_parameters SET
        "${external_parameters}" "${key}" "${quoted_value}")
endforeach()
if(MPARSER_NATIVE_JIT)
    string(JSON external_parameters SET
        "${external_parameters}" nativeJit true)
else()
    string(JSON external_parameters SET
        "${external_parameters}" nativeJit false)
endif()
string(JSON external_parameters SET
    "${external_parameters}" releasePackaging true)

set(internal_parameters "{}")
foreach(pair IN ITEMS
        "generator;${MPARSER_GENERATOR}"
        "compilerId;${MPARSER_COMPILER_ID}"
        "compilerVersion;${MPARSER_COMPILER_VERSION}"
        "systemName;${MPARSER_SYSTEM_NAME}"
        "systemVersion;${MPARSER_SYSTEM_VERSION}"
        "systemProcessor;${MPARSER_SYSTEM_PROCESSOR}")
    list(GET pair 0 key)
    list(GET pair 1 value)
    mparser_json_quote(quoted_value "${value}")
    string(JSON internal_parameters SET
        "${internal_parameters}" "${key}" "${quoted_value}")
endforeach()

set(resolved_dependencies "[]")
set(source_digest "{}")
mparser_json_quote(quoted_revision "${source_revision}")
string(JSON source_digest SET
    "${source_digest}" gitCommit "${quoted_revision}")
set(source_dependency "{}")
mparser_json_quote(quoted_source_name "source-base")
set(source_uri
    "git+${MPARSER_SOURCE_REPOSITORY}@${source_revision}")
mparser_json_quote(quoted_source_uri "${source_uri}")
string(JSON source_dependency SET
    "${source_dependency}" name "${quoted_source_name}")
string(JSON source_dependency SET
    "${source_dependency}" uri "${quoted_source_uri}")
string(JSON source_dependency SET
    "${source_dependency}" digest "${source_digest}")
string(JSON resolved_dependencies SET
    "${resolved_dependencies}" 0 "${source_dependency}")

set(contract_paths
    CMakeLists.txt
    CMakePresets.json
    .github/workflows/ci.yml
    docs/public-contract-v1.2.json
    docs/public-contract-v1.json
    docs/cli-contract-v1.json
    docs/machine-result-v1.schema.json
    docs/compatibility-matrix.json
    docs/release-process.md
    docs/release-authentication.md
    tests/validate_release_authentication_input.cmake)
set(dependency_index 1)
foreach(contract_path IN LISTS contract_paths)
    set(contract_file
        "${MPARSER_PROJECT_ROOT}/${contract_path}")
    if(NOT EXISTS "${contract_file}")
        message(FATAL_ERROR
            "Release provenance input is missing: ${contract_path}")
    endif()
    file(SHA256 "${contract_file}" contract_sha256)
    set(contract_digest "{}")
    mparser_json_quote(quoted_contract_hash "${contract_sha256}")
    string(JSON contract_digest SET
        "${contract_digest}" sha256 "${quoted_contract_hash}")
    set(contract_dependency "{}")
    mparser_json_quote(quoted_contract_name "${contract_path}")
    string(JSON contract_dependency SET
        "${contract_dependency}" name "${quoted_contract_name}")
    string(JSON contract_dependency SET
        "${contract_dependency}" digest "${contract_digest}")
    string(JSON resolved_dependencies SET
        "${resolved_dependencies}" ${dependency_index}
        "${contract_dependency}")
    math(EXPR dependency_index "${dependency_index} + 1")
endforeach()

set(build_definition "{}")
mparser_json_quote(quoted_build_type "${MPARSER_BUILD_TYPE_URI}")
string(JSON build_definition SET
    "${build_definition}" buildType "${quoted_build_type}")
string(JSON build_definition SET
    "${build_definition}" externalParameters
    "${external_parameters}")
string(JSON build_definition SET
    "${build_definition}" internalParameters
    "${internal_parameters}")
string(JSON build_definition SET
    "${build_definition}" resolvedDependencies
    "${resolved_dependencies}")

set(builder_versions "{}")
mparser_json_quote(quoted_cmake_version "${MPARSER_CMAKE_VERSION}")
set(compiler_version
    "${MPARSER_COMPILER_ID} ${MPARSER_COMPILER_VERSION}")
mparser_json_quote(quoted_compiler_version "${compiler_version}")
string(JSON builder_versions SET
    "${builder_versions}" cmake "${quoted_cmake_version}")
string(JSON builder_versions SET
    "${builder_versions}" compiler "${quoted_compiler_version}")
set(builder "{}")
mparser_json_quote(quoted_builder_id "${MPARSER_BUILDER_ID}")
string(JSON builder SET "${builder}" id "${quoted_builder_id}")
string(JSON builder SET "${builder}" version "${builder_versions}")

set(run_details "{}")
string(JSON run_details SET "${run_details}" builder "${builder}")
mparser_json_quote(quoted_authentication "unsigned")
string(JSON run_details SET
    "${run_details}" mparser_authentication "${quoted_authentication}")
string(JSON run_details SET
    "${run_details}" mparser_sourceTreeClean "${source_tree_clean}")

set(predicate "{}")
string(JSON predicate SET
    "${predicate}" buildDefinition "${build_definition}")
string(JSON predicate SET "${predicate}" runDetails "${run_details}")

set(statement "{}")
mparser_json_quote(
    quoted_statement_type "https://in-toto.io/Statement/v1")
mparser_json_quote(
    quoted_predicate_type "https://slsa.dev/provenance/v1")
string(JSON statement SET
    "${statement}" _type "${quoted_statement_type}")
string(JSON statement SET "${statement}" subject "${subjects}")
string(JSON statement SET
    "${statement}" predicateType "${quoted_predicate_type}")
string(JSON statement SET "${statement}" predicate "${predicate}")

file(WRITE "${provenance_output}" "${statement}\n")
message(STATUS
    "MParser unsigned release provenance: ${provenance_output}")
