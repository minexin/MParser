cmake_minimum_required(VERSION 3.20)

foreach(required IN ITEMS
        PROJECT_ROOT
        CLI_CONTRACT
        MPARSER_EXECUTABLE
        ENTRY_SOURCE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR
            "release documentation validator requires ${required}")
    endif()
endforeach()

set(required_documents
    docs/README.md
    docs/user-manual.md
    docs/build-and-install.md
    docs/support-matrix.md
    docs/cli-reference.md
    docs/jit-and-fallback.md
    docs/runtime-boundaries.md
    docs/migration-v1.0.md
    docs/v1.0-documentation.md
    docs/embedding-c-api.md
    docs/embedding-cpp-api.md
    docs/c-abi-compatibility.md
    docs/machine-result-protocol.md
    docs/extending-builtins.md
    docs/versioning-and-deprecation.md)

foreach(relative_path IN LISTS required_documents)
    set(document_path "${PROJECT_ROOT}/${relative_path}")
    if(NOT EXISTS "${document_path}")
        message(FATAL_ERROR
            "required release document is missing: ${relative_path}")
    endif()
    file(READ "${document_path}" document_text)
    string(LENGTH "${document_text}" document_length)
    if(document_length LESS 200)
        message(FATAL_ERROR
            "release document is unexpectedly short: ${relative_path}")
    endif()
    if(document_text MATCHES "(^|[^A-Za-z])(TODO|TBD)([^A-Za-z]|$)")
        message(FATAL_ERROR
            "release document retains a TODO/TBD marker: ${relative_path}")
    endif()
endforeach()

file(READ "${PROJECT_ROOT}/docs/README.md" documentation_index)
file(READ "${PROJECT_ROOT}/docs/user-manual.md" user_manual)
file(READ "${PROJECT_ROOT}/docs/build-and-install.md" build_guide)
file(READ "${PROJECT_ROOT}/docs/support-matrix.md" support_matrix)
file(READ "${PROJECT_ROOT}/docs/cli-reference.md" cli_reference)
file(READ "${PROJECT_ROOT}/docs/jit-and-fallback.md" jit_guide)
file(READ "${PROJECT_ROOT}/docs/runtime-boundaries.md" runtime_guide)
file(READ "${PROJECT_ROOT}/docs/migration-v1.0.md" migration_guide)
file(READ "${PROJECT_ROOT}/README.md" project_readme)
file(READ "${CLI_CONTRACT}" cli_contract)

function(require_text variable needle description)
    string(FIND "${${variable}}" "${needle}" found_at)
    if(found_at EQUAL -1)
        message(FATAL_ERROR
            "${description} is missing required text: ${needle}")
    endif()
endfunction()

set(indexed_documents ${required_documents})
list(REMOVE_ITEM indexed_documents docs/README.md)
foreach(relative_path IN LISTS indexed_documents)
    string(REPLACE "docs/" "" document_name "${relative_path}")
    require_text(documentation_index
        "(${document_name})"
        "documentation index")
endforeach()

require_text(project_readme
    "(docs/README.md)"
    "project README")
require_text(project_readme
    "(docs/user-manual.md)"
    "project README")
require_text(project_readme
    "(docs/support-matrix.md)"
    "project README")
require_text(project_readme
    "(docs/migration-v1.0.md)"
    "project README")

require_text(user_manual
    "complete MATLAB or toolbox compatibility"
    "user manual scope")
require_text(support_matrix
    "compatibility-matrix.json"
    "support matrix authority")
require_text(support_matrix
    "Post-v1.0"
    "support matrix deferred scope")
require_text(jit_guide
    "bytecode VM remains the semantic authority"
    "JIT fallback contract")
require_text(runtime_guide
    "Zero means unlimited"
    "runtime resource contract")
require_text(runtime_guide
    "not an operating-system sandbox"
    "runtime security boundary")
require_text(migration_guide
    "The undocumented pre-v1 `--run-interpreter` alias is removed"
    "migration removed-mode rule")
require_text(build_guide
    "third_party/sljit"
    "offline native-JIT build rule")

string(JSON cli_schema_major GET "${cli_contract}" schema major)
string(JSON cli_schema_minor GET "${cli_contract}" schema minor)
if(NOT cli_schema_major EQUAL 1 OR NOT cli_schema_minor EQUAL 0)
    message(FATAL_ERROR
        "release documentation expects CLI contract 1.0")
endif()

string(JSON production_mode GET "${cli_contract}" production mode)
string(JSON production_jit GET "${cli_contract}" production jit_option)
string(JSON production_machine GET
    "${cli_contract}" production machine_option)
require_text(cli_reference "${production_mode}" "CLI reference")
require_text(cli_reference "${production_jit}" "CLI reference")
require_text(cli_reference "${production_machine}" "CLI reference")

set(documented_contract_options)
string(JSON stable_option_count LENGTH
    "${cli_contract}" production stable_options)
math(EXPR stable_option_last "${stable_option_count} - 1")
foreach(index RANGE 0 ${stable_option_last})
    string(JSON option GET
        "${cli_contract}" production stable_options ${index})
    require_text(cli_reference "${option}" "CLI stable option table")
    list(APPEND documented_contract_options "${option}")
endforeach()

string(JSON diagnostic_mode_count LENGTH
    "${cli_contract}" diagnostic_modes modes)
math(EXPR diagnostic_mode_last "${diagnostic_mode_count} - 1")
foreach(index RANGE 0 ${diagnostic_mode_last})
    string(JSON mode GET
        "${cli_contract}" diagnostic_modes modes ${index})
    require_text(cli_reference "${mode}" "CLI diagnostic mode table")
endforeach()

string(JSON backend_option GET
    "${cli_contract}" diagnostic_modes backend_option)
require_text(cli_reference "${backend_option}" "CLI backend option")

foreach(rule IN ITEMS repeatable single_occurrence)
    string(JSON option_count LENGTH
        "${cli_contract}" option_rules ${rule})
    math(EXPR option_last "${option_count} - 1")
    foreach(index RANGE 0 ${option_last})
        string(JSON option GET
            "${cli_contract}" option_rules ${rule} ${index})
        if(option MATCHES "^--")
            require_text(cli_reference "${option}" "CLI option rules")
        endif()
    endforeach()
endforeach()

string(JSON utility_count LENGTH "${cli_contract}" utility_commands)
math(EXPR utility_last "${utility_count} - 1")
foreach(index RANGE 0 ${utility_last})
    string(JSON utility MEMBER
        "${cli_contract}" utility_commands ${index})
    require_text(cli_reference "${utility}" "CLI utility commands")
endforeach()

string(JSON removed_count LENGTH "${cli_contract}" removed_before_v1)
math(EXPR removed_last "${removed_count} - 1")
foreach(index RANGE 0 ${removed_last})
    string(JSON removed_name GET
        "${cli_contract}" removed_before_v1 ${index} name)
    string(JSON replacement GET
        "${cli_contract}" removed_before_v1 ${index} replacement)
    require_text(cli_reference "${removed_name}" "CLI removed-mode migration")
    require_text(cli_reference "${replacement}" "CLI removed-mode migration")
    require_text(migration_guide "${removed_name}" "v1 migration guide")
    require_text(migration_guide "${replacement}" "v1 migration guide")
endforeach()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_EXECUTABLE}")

execute_process(
    COMMAND ${mparser_command} --help
    RESULT_VARIABLE help_result
    OUTPUT_VARIABLE help_output
    ERROR_VARIABLE help_error)
if(NOT "${help_result}" STREQUAL "0" OR NOT help_error STREQUAL "")
    message(FATAL_ERROR
        "mparser --help failed (${help_result})\n"
        "stdout:\n${help_output}\n"
        "stderr:\n${help_error}")
endif()

set(live_contract_tokens
    "${production_mode}"
    "${production_jit}"
    "${production_machine}"
    "${backend_option}")
list(APPEND live_contract_tokens ${documented_contract_options})
foreach(index RANGE 0 ${diagnostic_mode_last})
    string(JSON mode GET
        "${cli_contract}" diagnostic_modes modes ${index})
    list(APPEND live_contract_tokens "${mode}")
endforeach()
foreach(index RANGE 0 ${utility_last})
    string(JSON utility MEMBER
        "${cli_contract}" utility_commands ${index})
    list(APPEND live_contract_tokens "${utility}")
endforeach()
list(REMOVE_DUPLICATES live_contract_tokens)

foreach(token IN LISTS live_contract_tokens)
    string(REGEX REPLACE "=.*$" "" option_name "${token}")
    string(FIND "${help_output}" "${option_name}" option_at)
    if(option_at EQUAL -1)
        message(FATAL_ERROR
            "live --help is missing CLI contract option: ${option_name}")
    endif()
endforeach()

execute_process(
    COMMAND ${mparser_command}
        --run --jit=off "${ENTRY_SOURCE}"
    RESULT_VARIABLE sample_result
    OUTPUT_VARIABLE sample_output
    ERROR_VARIABLE sample_error)
if(NOT "${sample_result}" STREQUAL "0" OR
   NOT sample_output MATCHES "summary = 705" OR
   NOT sample_error STREQUAL "")
    message(FATAL_ERROR
        "documented production sample failed (${sample_result})\n"
        "stdout:\n${sample_output}\n"
        "stderr:\n${sample_error}")
endif()

execute_process(
    COMMAND ${mparser_command}
        --run --jit=off --result-format=json-v1 "${ENTRY_SOURCE}"
    RESULT_VARIABLE machine_result
    OUTPUT_VARIABLE machine_output
    ERROR_VARIABLE machine_error)
if(NOT "${machine_result}" STREQUAL "0" OR
   NOT machine_error STREQUAL "")
    message(FATAL_ERROR
        "documented machine sample failed (${machine_result})\n"
        "stdout:\n${machine_output}\n"
        "stderr:\n${machine_error}")
endif()
string(JSON machine_schema_name GET "${machine_output}" protocol name)
string(JSON machine_schema_major GET "${machine_output}" protocol major)
string(JSON machine_status GET "${machine_output}" status)
if(NOT machine_schema_name STREQUAL "mparser.result" OR
   NOT machine_schema_major EQUAL 1 OR
   NOT machine_status STREQUAL "succeeded")
    message(FATAL_ERROR
        "documented machine sample returned an invalid protocol result")
endif()

message(STATUS
    "MParser release documentation validated: CLI 1.0, manuals, "
    "production sample, and mparser.result 1.x")
