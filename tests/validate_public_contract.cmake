cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        CONTRACT
        PROJECT_ROOT
        EXPECTED_VERSION
        EXPECTED_C_ABI_MAJOR
        EXPECTED_C_ABI_REVISION
        EXPECTED_C_SYMBOL_COUNT
        EXPECTED_PROTOCOL_MAJOR
        EXPECTED_PROTOCOL_MINOR)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing public-contract variable: ${required_variable}")
    endif()
endforeach()

file(READ "${CONTRACT}" contract_json)

function(require_json expected description)
    string(JSON actual GET "${contract_json}" ${ARGN})
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${description} changed: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(validate_contract_artifact description expected_path)
    string(JSON artifact_path GET "${contract_json}" ${ARGN} path)
    string(JSON expected_hash GET "${contract_json}" ${ARGN} sha256_lf)
    if(NOT artifact_path STREQUAL expected_path)
        message(FATAL_ERROR
            "${description} path changed: expected '${expected_path}', "
            "got '${artifact_path}'")
    endif()
    set(artifact "${PROJECT_ROOT}/${artifact_path}")
    if(NOT EXISTS "${artifact}")
        message(FATAL_ERROR
            "${description} is missing: ${artifact}")
    endif()
    file(READ "${artifact}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(REPLACE "\r" "\n" contents "${contents}")
    string(SHA256 actual_hash "${contents}")
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR
            "${description} contract changed.\n"
            "Expected normalized SHA-256: ${expected_hash}\n"
            "Actual normalized SHA-256:   ${actual_hash}\n"
            "Perform the documented compatibility review before updating "
            "docs/public-contract-v1.json.")
    endif()
endfunction()

require_json("mparser.public-contract"
    "public contract schema" schema name)
require_json("1" "public contract schema major" schema major)
require_json("0" "public contract schema minor" schema minor)
require_json("${EXPECTED_VERSION}"
    "public contract engine version" release engine_version)
require_json("frozen-v1"
    "public contract release state" release state)
require_json("Apache-2.0"
    "public contract license" license spdx)

require_json("1" "CLI contract major" cli major)
require_json("0" "CLI contract minor" cli minor)
require_json("--run" "production CLI mode" cli production_mode)
require_json("json-v1" "CLI machine format" cli machine_format)
require_json("${EXPECTED_C_ABI_MAJOR}"
    "C ABI major" c_abi major)
require_json("${EXPECTED_C_ABI_REVISION}"
    "C ABI revision" c_abi revision)
require_json("${EXPECTED_C_ABI_MAJOR}.${EXPECTED_C_ABI_REVISION}.0"
    "C ABI shared-library version" c_abi library_version)
require_json("${EXPECTED_C_ABI_MAJOR}"
    "C ABI SOVERSION" c_abi soversion)
require_json("${EXPECTED_C_SYMBOL_COUNT}"
    "C ABI symbol count" c_abi symbol_count)
require_json("1" "C++ source API major" cpp_api major)
require_json("0" "C++ source API minor" cpp_api minor)
require_json("C++20" "C++ API language" cpp_api language)
require_json("none" "C++ binary ABI declaration" cpp_api binary_abi)
require_json("mparser.result"
    "machine protocol name" machine_result_protocol name)
require_json("${EXPECTED_PROTOCOL_MAJOR}"
    "machine protocol major" machine_result_protocol major)
require_json("${EXPECTED_PROTOCOL_MINOR}"
    "machine protocol minor" machine_result_protocol minor)
require_json("json-v1"
    "machine protocol CLI format" machine_result_protocol cli_format)
require_json("1" "builtin source contract major"
    builtin_source_contract major)
require_json("0" "builtin source contract minor"
    builtin_source_contract minor)
require_json("none" "builtin source binary ABI"
    builtin_source_contract binary_abi)

validate_contract_artifact(
    "CLI 1.0 contract" "docs/cli-contract-v1.json" cli contract)
validate_contract_artifact(
    "C ABI header" "include/mparser/c_api.h" c_abi header)
validate_contract_artifact(
    "C ABI 1.1 snapshot"
    "tests/public_contract/c_abi/1.1/c_api_snapshot.h"
    c_abi snapshot)
validate_contract_artifact(
    "C ABI 1.0 legacy snapshot"
    "tests/c_api_v1_snapshot.h"
    c_abi legacy_1_0_snapshot)
validate_contract_artifact(
    "C ABI symbol manifest" "tests/c_api_abi1_symbols.txt" c_abi symbols)
validate_contract_artifact(
    "C++ API header" "include/mparser/cpp_api.hpp" cpp_api header)
validate_contract_artifact(
    "C++ API 1.0 snapshot"
    "tests/public_contract/cpp_api/1.0/mparser/cpp_api.hpp"
    cpp_api snapshot_header)
validate_contract_artifact(
    "C++ API 1.0 C dependency snapshot"
    "tests/public_contract/cpp_api/1.0/mparser/c_api.h"
    cpp_api snapshot_c_header)
validate_contract_artifact(
    "machine protocol golden" "tests/golden/machine_result_v1.json"
    machine_result_protocol golden)
validate_contract_artifact(
    "machine protocol emergency golden"
    "tests/golden/machine_result_emergency_v1.json"
    machine_result_protocol emergency)
validate_contract_artifact(
    "machine protocol 1.0 snapshot"
    "tests/public_contract/protocol/1.0/machine_result_1_0.json"
    machine_result_protocol snapshot)
validate_contract_artifact(
    "machine protocol JSON Schema"
    "docs/machine-result-v1.schema.json"
    machine_result_protocol schema)
validate_contract_artifact(
    "builtin source-contract header"
    "src/mparser/builtin_registry.h"
    builtin_source_contract header)
validate_contract_artifact(
    "builtin default catalog snapshot"
    "tests/public_contract/builtin/1.0/default_catalog.json"
    builtin_source_contract default_catalog)
validate_contract_artifact(
    "builtin extension author guide"
    "docs/extending-builtins.md"
    builtin_source_contract author_guide)
validate_contract_artifact(
    "versioning and deprecation policy"
    "docs/versioning-and-deprecation.md"
    versioning_and_deprecation)
validate_contract_artifact(
    "CMake package config template"
    "cmake/MParserConfig.cmake.in"
    cmake_package config_template)
require_json("SameMinorVersion"
    "pre-v1 CMake compatibility"
    cmake_package pre_v1_find_compatibility)
require_json("SameMajorVersion"
    "v1 CMake compatibility"
    cmake_package v1_find_compatibility)

file(READ "${PROJECT_ROOT}/docs/cli-contract-v1.json" cli_contract)
string(JSON cli_schema_name GET "${cli_contract}" schema name)
string(JSON cli_schema_major GET "${cli_contract}" schema major)
string(JSON cli_schema_minor GET "${cli_contract}" schema minor)
string(JSON cli_production_mode GET "${cli_contract}" production mode)
string(JSON cli_machine_option GET "${cli_contract}"
    production machine_option)
string(JSON cli_machine_exit_four GET "${cli_contract}"
    channels json-v1 exit_codes 4)
if(NOT cli_schema_name STREQUAL "mparser.cli-contract" OR
   NOT cli_schema_major STREQUAL "1" OR
   NOT cli_schema_minor STREQUAL "0" OR
   NOT cli_production_mode STREQUAL "--run" OR
   NOT cli_machine_option STREQUAL "--result-format=json-v1" OR
   NOT cli_machine_exit_four STREQUAL
       "emergency serialization or output-transport failure")
    message(FATAL_ERROR
        "CLI contract content does not match the frozen 1.0 boundary")
endif()

file(READ
    "${PROJECT_ROOT}/tests/public_contract/builtin/1.0/default_catalog.json"
    builtin_catalog)
string(JSON builtin_schema_name GET "${builtin_catalog}" schema name)
string(JSON builtin_contract_major GET "${builtin_catalog}"
    source_contract major)
string(JSON builtin_contract_minor GET "${builtin_catalog}"
    source_contract minor)
string(JSON builtin_descriptor_count GET "${builtin_catalog}"
    descriptor_count)
if(NOT builtin_schema_name STREQUAL "mparser.builtin-catalog" OR
   NOT builtin_contract_major STREQUAL "1" OR
   NOT builtin_contract_minor STREQUAL "0" OR
   NOT builtin_descriptor_count STREQUAL "118")
    message(FATAL_ERROR
        "Builtin catalog does not match source contract 1.0")
endif()

file(READ
    "${PROJECT_ROOT}/src/mparser/builtin_registry.h"
    builtin_header)
string(REGEX MATCH
    "kBuiltinSourceContractMajor[ \t]*=[ \t]*([0-9]+)"
    builtin_major_match "${builtin_header}")
set(builtin_header_major "${CMAKE_MATCH_1}")
string(REGEX MATCH
    "kBuiltinSourceContractMinor[ \t]*=[ \t]*([0-9]+)"
    builtin_minor_match "${builtin_header}")
set(builtin_header_minor "${CMAKE_MATCH_1}")
if(NOT builtin_header_major STREQUAL "1" OR
   NOT builtin_header_minor STREQUAL "0")
    message(FATAL_ERROR
        "Builtin registry source-contract constants changed")
endif()

file(READ
    "${PROJECT_ROOT}/cmake/MParserConfig.cmake.in"
    cmake_package_template)
foreach(required_metadata IN ITEMS
        "MParser_C_ABI_VERSION"
        "MParser_CPP_API_VERSION_MAJOR"
        "MParser_CLI_CONTRACT_MAJOR"
        "MParser_MACHINE_PROTOCOL_MAJOR"
        "MParser_BUILTIN_SOURCE_CONTRACT_MAJOR"
        "MParser_PUBLIC_CONTRACT_FILE"
        "MParser_CLI_CONTRACT_FILE"
        "MParser_MACHINE_PROTOCOL_SCHEMA_FILE"
        "MParser_BUILTIN_CATALOG_FILE"
        "MParser_VERSIONING_POLICY_FILE")
    string(FIND
        "${cmake_package_template}" "${required_metadata}"
        metadata_position)
    if(metadata_position EQUAL -1)
        message(FATAL_ERROR
            "CMake package template is missing ${required_metadata}")
    endif()
endforeach()

file(READ "${PROJECT_ROOT}/CMakeLists.txt" project_cmake)
string(REGEX MATCH "^([0-9]+)" expected_version_major_match
    "${EXPECTED_VERSION}")
set(expected_version_major "${CMAKE_MATCH_1}")
if(expected_version_major STREQUAL "")
    message(FATAL_ERROR
        "Unable to read the engine major from ${EXPECTED_VERSION}")
endif()
if(expected_version_major LESS 1)
    set(expected_package_compatibility "SameMinorVersion")
else()
    set(expected_package_compatibility "SameMajorVersion")
endif()
string(FIND
    "${project_cmake}"
    "COMPATIBILITY ${expected_package_compatibility}"
    package_compatibility_position)
if(package_compatibility_position EQUAL -1)
    message(FATAL_ERROR
        "CMake package compatibility for ${EXPECTED_VERSION} no longer "
        "matches ${expected_package_compatibility}")
endif()

file(READ "${PROJECT_ROOT}/include/mparser/c_api.h" c_header)
string(REGEX MATCH
    "#define[ \t]+MPARSER_C_ABI_VERSION_MAJOR[ \t]+([0-9]+)u"
    c_major_match "${c_header}")
set(c_header_major "${CMAKE_MATCH_1}")
string(REGEX MATCH
    "#define[ \t]+MPARSER_C_ABI_REVISION[ \t]+([0-9]+)u"
    c_revision_match "${c_header}")
set(c_header_revision "${CMAKE_MATCH_1}")
if(NOT c_header_major STREQUAL EXPECTED_C_ABI_MAJOR OR
   NOT c_header_revision STREQUAL EXPECTED_C_ABI_REVISION)
    message(FATAL_ERROR
        "C header ABI macros do not match the frozen contract")
endif()

file(STRINGS
    "${PROJECT_ROOT}/tests/c_api_abi1_symbols.txt" c_symbols)
list(FILTER c_symbols EXCLUDE REGEX "^[ \t]*$")
list(LENGTH c_symbols c_symbol_count)
if(NOT c_symbol_count EQUAL EXPECTED_C_SYMBOL_COUNT)
    message(FATAL_ERROR
        "C ABI symbol manifest contains ${c_symbol_count} entries, "
        "expected ${EXPECTED_C_SYMBOL_COUNT}")
endif()

file(READ
    "${PROJECT_ROOT}/tests/golden/machine_result_v1.json"
    protocol_golden)
string(JSON protocol_name GET "${protocol_golden}" protocol name)
string(JSON protocol_major GET "${protocol_golden}" protocol major)
string(JSON protocol_minor GET "${protocol_golden}" protocol minor)
if(NOT protocol_name STREQUAL "mparser.result" OR
   NOT protocol_major STREQUAL EXPECTED_PROTOCOL_MAJOR OR
   NOT protocol_minor STREQUAL EXPECTED_PROTOCOL_MINOR)
    message(FATAL_ERROR
        "Machine protocol golden does not match the frozen contract")
endif()

message(STATUS
    "MParser public contract validated: CLI 1.0, C ABI "
    "${EXPECTED_C_ABI_MAJOR}.${EXPECTED_C_ABI_REVISION}, "
    "${EXPECTED_C_SYMBOL_COUNT} symbols, C++20 source API, "
    "mparser.result ${EXPECTED_PROTOCOL_MAJOR}.${EXPECTED_PROTOCOL_MINOR}, "
    "builtin source contract 1.0")
