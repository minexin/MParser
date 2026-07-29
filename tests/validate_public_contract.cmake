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
    "public contract engine version" candidate engine_version)
require_json("frozen-v1-candidate"
    "public contract candidate state" candidate state)
require_json("Apache-2.0"
    "public contract license" license spdx)

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

validate_contract_artifact(
    "C ABI header" "include/mparser/c_api.h" c_abi header)
validate_contract_artifact(
    "C ABI 1.1 snapshot"
    "tests/public_contract/c_abi/1.1/c_api_snapshot.h"
    c_abi snapshot)
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
    "MParser public contract validated: C ABI "
    "${EXPECTED_C_ABI_MAJOR}.${EXPECTED_C_ABI_REVISION}, "
    "${EXPECTED_C_SYMBOL_COUNT} symbols, C++20 source API, "
    "mparser.result ${EXPECTED_PROTOCOL_MAJOR}.${EXPECTED_PROTOCOL_MINOR}")
