cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS
        CONTRACT PROJECT_ROOT EXPECTED_VERSION CONTRACT_STATE)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing v1.2 public-contract variable: ${required_variable}")
    endif()
endforeach()

set(contract_is_current FALSE)
if(CONTRACT_STATE STREQUAL "current")
    set(contract_is_current TRUE)
elseif(NOT CONTRACT_STATE STREQUAL "archived")
    message(FATAL_ERROR
        "Invalid v1.2 contract state: ${CONTRACT_STATE}")
endif()

file(READ "${CONTRACT}" contract_json)

function(require_json expected description)
    string(JSON actual GET "${contract_json}" ${ARGN})
    if(NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${description} changed: expected '${expected}', got '${actual}'")
    endif()
endfunction()

function(normalized_sha256 output path)
    file(READ "${path}" contents)
    string(REPLACE "\r\n" "\n" contents "${contents}")
    string(REPLACE "\r" "\n" contents "${contents}")
    string(SHA256 digest "${contents}")
    set(${output} "${digest}" PARENT_SCOPE)
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
        message(FATAL_ERROR "${description} is missing: ${artifact}")
    endif()
    normalized_sha256(actual_hash "${artifact}")
    if(NOT actual_hash STREQUAL expected_hash)
        message(FATAL_ERROR
            "${description} candidate snapshot changed.\n"
            "Expected normalized SHA-256: ${expected_hash}\n"
            "Actual normalized SHA-256:   ${actual_hash}")
    endif()
endfunction()

function(validate_live_contract_artifact description expected_path)
    if(contract_is_current)
        validate_contract_artifact("${description}" "${expected_path}"
            ${ARGN})
        return()
    endif()
    string(JSON artifact_path GET "${contract_json}" ${ARGN} path)
    string(JSON expected_hash GET "${contract_json}" ${ARGN} sha256_lf)
    if(NOT artifact_path STREQUAL expected_path)
        message(FATAL_ERROR
            "${description} path changed: expected '${expected_path}', "
            "got '${artifact_path}'")
    endif()
    string(LENGTH "${expected_hash}" hash_length)
    if(NOT hash_length EQUAL 64)
        message(FATAL_ERROR
            "${description} archived hash is not a SHA-256 digest")
    endif()
endfunction()

require_json("mparser.public-contract" "contract schema" schema name)
require_json("1" "contract schema major" schema major)
require_json("1" "contract schema minor" schema minor)
require_json("${EXPECTED_VERSION}" "candidate engine version"
    release engine_version)
require_json("v1.2" "candidate milestone" release milestone)
require_json("frozen-candidate" "candidate state" release state)
require_json("Apache-2.0" "candidate license" license spdx)
require_json("Copyright 2026 Wang Xin" "candidate copyright"
    license copyright)

require_json("1" "CLI major" cli major)
require_json("0" "CLI minor" cli minor)
require_json("--run" "production CLI mode" cli production_mode)
require_json("json-v1" "CLI machine format" cli machine_format)
require_json("1" "C source API major" c_api major)
require_json("2" "C source API minor" c_api minor)
require_json("0" "C source API patch" c_api patch)
require_json("2" "C ABI generation" c_abi generation)
require_json("0" "C ABI revision" c_abi revision)
require_json("${EXPECTED_VERSION}" "C library version" c_abi library_version)
require_json("2" "C ABI SOVERSION" c_abi soversion)
require_json("109" "C ABI symbol count" c_abi symbol_count)
require_json("1" "C++ source API major" cpp_api major)
require_json("2" "C++ source API minor" cpp_api minor)
require_json("C++20" "C++ source language" cpp_api language)
require_json("none" "C++ binary ABI declaration" cpp_api binary_abi)
require_json("mparser.result" "machine protocol name"
    machine_result_protocol name)
require_json("1" "machine protocol major" machine_result_protocol major)
require_json("1" "machine protocol minor" machine_result_protocol minor)
require_json("1" "builtin source contract major"
    builtin_source_contract major)
require_json("1" "builtin source contract minor"
    builtin_source_contract minor)
require_json("166" "builtin descriptor count"
    builtin_source_contract descriptor_count)
require_json("168" "builtin registered-name count"
    builtin_source_contract registered_name_count)

validate_contract_artifact("v1.2 milestone" "docs/v1.2.md"
    milestone)
validate_contract_artifact("CLI 1.0 contract" "docs/cli-contract-v1.json"
    cli contract)
validate_live_contract_artifact("C API 1.2 header" "include/mparser/c_api.h"
    c_api header)
validate_contract_artifact("C ABI generation-2 snapshot"
    "tests/public_contract/c_abi/2.0/c_api_snapshot.h" c_abi snapshot)
validate_contract_artifact("C ABI generation-2 symbols"
    "tests/c_api_generation2_symbols.txt" c_abi symbols)
validate_live_contract_artifact("C++ API 1.2 header"
    "include/mparser/cpp_api.hpp" cpp_api header)
validate_contract_artifact("C++ API 1.2 snapshot"
    "tests/public_contract/cpp_api/1.2/mparser/cpp_api.hpp"
    cpp_api snapshot_header)
validate_contract_artifact("C++ API 1.2 C dependency snapshot"
    "tests/public_contract/cpp_api/1.2/mparser/c_api.h"
    cpp_api snapshot_c_header)
validate_contract_artifact("machine protocol producer golden"
    "tests/golden/machine_result_v1.json" machine_result_protocol golden)
validate_contract_artifact("machine protocol emergency golden"
    "tests/golden/machine_result_emergency_v1.json"
    machine_result_protocol emergency)
validate_contract_artifact("machine protocol 1.1 compatibility snapshot"
    "tests/public_contract/protocol/1.1/machine_result_1_1.json"
    machine_result_protocol compatibility_snapshot)
validate_contract_artifact("machine protocol schema"
    "docs/machine-result-v1.schema.json" machine_result_protocol schema)
validate_live_contract_artifact("builtin contract header"
    "src/mparser/builtin_registry.h" builtin_source_contract header)
validate_contract_artifact("builtin 1.1 catalog"
    "tests/public_contract/builtin/1.1/default_catalog.json"
    builtin_source_contract default_catalog)
validate_live_contract_artifact("builtin extension guide"
    "docs/extending-builtins.md" builtin_source_contract author_guide)
validate_live_contract_artifact("versioning policy"
    "docs/versioning-and-deprecation.md" versioning_and_deprecation)
validate_live_contract_artifact("CMake package template"
    "cmake/MParserConfig.cmake.in" cmake_package config_template)

string(JSON c_header_hash GET "${contract_json}" c_api header sha256_lf)
string(JSON c_snapshot_hash GET "${contract_json}" c_abi snapshot sha256_lf)
string(JSON cpp_c_snapshot_hash GET "${contract_json}"
    cpp_api snapshot_c_header sha256_lf)
if(NOT c_header_hash STREQUAL c_snapshot_hash OR
   NOT c_header_hash STREQUAL cpp_c_snapshot_hash)
    message(FATAL_ERROR "C API live and snapshot hashes differ")
endif()
string(JSON cpp_header_hash GET "${contract_json}"
    cpp_api header sha256_lf)
string(JSON cpp_snapshot_hash GET "${contract_json}"
    cpp_api snapshot_header sha256_lf)
if(NOT cpp_header_hash STREQUAL cpp_snapshot_hash)
    message(FATAL_ERROR "C++ API live and snapshot hashes differ")
endif()

if(contract_is_current)
    file(READ "${PROJECT_ROOT}/include/mparser/c_api.h" c_header)
    foreach(version_pair IN ITEMS
            "MPARSER_C_API_VERSION_MAJOR;1"
            "MPARSER_C_API_VERSION_MINOR;2"
            "MPARSER_C_API_VERSION_PATCH;0"
            "MPARSER_C_ABI_GENERATION;2"
            "MPARSER_C_ABI_REVISION;0")
        list(GET version_pair 0 macro)
        list(GET version_pair 1 expected)
        string(REGEX MATCH
            "#define[ \t]+${macro}[ \t]+([0-9]+)u"
            macro_match "${c_header}")
        if(NOT CMAKE_MATCH_1 STREQUAL expected)
            message(FATAL_ERROR
                "C header ${macro} changed: expected ${expected}")
        endif()
    endforeach()

    file(READ "${PROJECT_ROOT}/include/mparser/cpp_api.hpp" cpp_header)
    foreach(version_pair IN ITEMS
            "kSourceApiVersionMajor;1"
            "kSourceApiVersionMinor;2")
        list(GET version_pair 0 constant)
        list(GET version_pair 1 expected)
        string(REGEX MATCH
            "${constant}[ \t]*=[ \t]*([0-9]+)"
            constant_match "${cpp_header}")
        if(NOT CMAKE_MATCH_1 STREQUAL expected)
            message(FATAL_ERROR
                "C++ header ${constant} changed: expected ${expected}")
        endif()
    endforeach()

    file(READ "${PROJECT_ROOT}/src/mparser/builtin_registry.h" builtin_header)
    foreach(version_pair IN ITEMS
            "kBuiltinSourceContractMajor;1"
            "kBuiltinSourceContractMinor;1")
        list(GET version_pair 0 constant)
        list(GET version_pair 1 expected)
        string(REGEX MATCH
            "${constant}[ \t]*=[ \t]*([0-9]+)"
            constant_match "${builtin_header}")
        if(NOT CMAKE_MATCH_1 STREQUAL expected)
            message(FATAL_ERROR
                "Builtin header ${constant} changed: expected ${expected}")
        endif()
    endforeach()
endif()

file(STRINGS "${PROJECT_ROOT}/tests/c_api_generation2_symbols.txt" symbols)
list(FILTER symbols EXCLUDE REGEX "^[ \t]*$")
list(LENGTH symbols symbol_count)
if(NOT symbol_count EQUAL 109)
    message(FATAL_ERROR
        "C ABI generation-2 symbol manifest has ${symbol_count} entries")
endif()

file(READ
    "${PROJECT_ROOT}/tests/public_contract/builtin/1.1/default_catalog.json"
    builtin_catalog)
string(JSON descriptor_count GET "${builtin_catalog}" descriptor_count)
string(JSON catalog_major GET "${builtin_catalog}" source_contract major)
string(JSON catalog_minor GET "${builtin_catalog}" source_contract minor)
if(NOT descriptor_count EQUAL 166 OR
   NOT catalog_major EQUAL 1 OR NOT catalog_minor EQUAL 1)
    message(FATAL_ERROR "Builtin 1.1 catalog identity changed")
endif()
set(registered_name_count 0)
math(EXPR descriptor_last "${descriptor_count} - 1")
foreach(descriptor_index RANGE 0 ${descriptor_last})
    math(EXPR registered_name_count "${registered_name_count} + 1")
    string(JSON alias_count LENGTH "${builtin_catalog}"
        descriptors ${descriptor_index} aliases)
    math(EXPR registered_name_count
        "${registered_name_count} + ${alias_count}")
endforeach()
if(NOT registered_name_count EQUAL 168)
    message(FATAL_ERROR
        "Builtin catalog has ${registered_name_count} registered names")
endif()

file(READ "${PROJECT_ROOT}/tests/golden/machine_result_v1.json"
    protocol_golden)
string(JSON protocol_name GET "${protocol_golden}" protocol name)
string(JSON protocol_major GET "${protocol_golden}" protocol major)
string(JSON protocol_minor GET "${protocol_golden}" protocol minor)
if(NOT protocol_name STREQUAL "mparser.result" OR
   NOT protocol_major EQUAL 1 OR NOT protocol_minor EQUAL 1)
    message(FATAL_ERROR "Machine protocol producer golden changed")
endif()

if(contract_is_current)
    file(READ "${PROJECT_ROOT}/cmake/MParserConfig.cmake.in" package_template)
    foreach(required_text IN ITEMS
            "set(MParser_BUILTIN_SOURCE_CONTRACT_MINOR \"1\")"
            "@PACKAGE_CMAKE_INSTALL_DOCDIR@/public-contract-v1.2.json"
            "@PACKAGE_CMAKE_INSTALL_DOCDIR@/default_catalog.json")
        string(FIND "${package_template}" "${required_text}" found_at)
        if(found_at EQUAL -1)
            message(FATAL_ERROR
                "CMake package metadata is missing: ${required_text}")
        endif()
    endforeach()
endif()

message(STATUS
    "MParser v1.2 candidate contract validated: C API 1.2, ABI generation "
    "2 revision 0, 109 symbols, C++ API 1.2, mparser.result 1.1, builtin "
    "source contract 1.1, 166 descriptors and 168 names")
