if(NOT DEFINED MPARSER_CLI OR
   NOT DEFINED SCHEMA_VALIDATOR OR
   NOT DEFINED SCHEMA OR
   NOT DEFINED SCHEMA_VALIDATION_DIR OR
   NOT DEFINED SUCCESS_SOURCE OR
   NOT DEFINED COMPILE_FAILURE_SOURCE OR
   NOT DEFINED RUNTIME_FAILURE_SOURCE OR
   NOT DEFINED EXPECTED_VERSION)
    message(FATAL_ERROR "machine protocol test inputs are incomplete")
endif()

set(mparser_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND mparser_command ${MPARSER_EMULATOR})
endif()
list(APPEND mparser_command "${MPARSER_CLI}")

set(schema_validator_command)
if(DEFINED MPARSER_EMULATOR AND NOT MPARSER_EMULATOR STREQUAL "")
    list(APPEND schema_validator_command ${MPARSER_EMULATOR})
endif()
list(APPEND schema_validator_command "${SCHEMA_VALIDATOR}")
file(MAKE_DIRECTORY "${SCHEMA_VALIDATION_DIR}")

function(validate_machine_schema name document)
    set(document_path
        "${SCHEMA_VALIDATION_DIR}/${name}.json")
    file(WRITE "${document_path}" "${document}")
    execute_process(
        COMMAND ${schema_validator_command}
            "${SCHEMA}" "${document_path}"
        RESULT_VARIABLE validation_exit
        OUTPUT_VARIABLE validation_output
        ERROR_VARIABLE validation_error)
    if(NOT validation_exit EQUAL 0)
        message(FATAL_ERROR
            "${name}: machine result failed JSON Schema validation\n"
            "${validation_output}${validation_error}")
    endif()
endfunction()

function(require_machine_document name raw_output out_variable)
    set(output "${raw_output}")
    string(LENGTH "${output}" output_length)
    if(output_length LESS 3)
        message(FATAL_ERROR
            "${name}: machine stdout is too short: '${output}'")
    endif()
    math(EXPR terminal_index "${output_length} - 1")
    string(SUBSTRING "${output}" ${terminal_index} 1 terminal)
    if(NOT terminal STREQUAL "\n")
        message(FATAL_ERROR
            "${name}: machine stdout must end in exactly one newline")
    endif()
    string(SUBSTRING "${output}" 0 ${terminal_index} document)
    if(document MATCHES "[\r\n]")
        message(FATAL_ERROR
            "${name}: machine stdout must contain one single-line "
            "document followed by one LF byte")
    endif()
    string(STRIP "${document}" stripped_document)
    if(NOT document STREQUAL stripped_document)
        message(FATAL_ERROR
            "${name}: machine stdout has leading or trailing whitespace")
    endif()
    string(JSON protocol_name ERROR_VARIABLE json_error
        GET "${document}" protocol name)
    if(json_error OR NOT protocol_name STREQUAL "mparser.result")
        message(FATAL_ERROR
            "${name}: stdout is not a v1 result document: ${document}\n"
            "${json_error}")
    endif()
    set("${out_variable}" "${document}" PARENT_SCOPE)
endfunction()

function(run_machine_case name expected_exit source)
    execute_process(
        COMMAND ${mparser_command} --run --jit=off
            --result-format=json-v1 ${ARGN} "${source}"
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output)
    if(NOT "${actual_exit}" STREQUAL "${expected_exit}")
        message(FATAL_ERROR
            "${name}: expected exit ${expected_exit}, got ${actual_exit}\n"
            "stdout: ${output}\nstderr: ${error_output}")
    endif()
    if(NOT error_output STREQUAL "")
        message(FATAL_ERROR
            "${name}: machine mode wrote to stderr: ${error_output}")
    endif()
    require_machine_document("${name}" "${output}" document)
    validate_machine_schema("${name}" "${document}")
    set("${name}_JSON" "${document}" PARENT_SCOPE)
endfunction()

function(require_json_equal name json expected)
    string(JSON actual ERROR_VARIABLE json_error GET "${json}" ${ARGN})
    if(json_error OR NOT "${actual}" STREQUAL "${expected}")
        message(FATAL_ERROR
            "${name}: expected '${expected}', got '${actual}': ${json_error}")
    endif()
endfunction()

function(run_backend_case name jit_mode expected_backend)
    execute_process(
        COMMAND ${mparser_command} --run "--jit=${jit_mode}"
            --result-format=json-v1 "${SUCCESS_SOURCE}"
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output)
    if(NOT "${actual_exit}" STREQUAL "0" OR
       NOT error_output STREQUAL "")
        message(FATAL_ERROR
            "${name}: backend case failed\nexit: ${actual_exit}\n"
            "stdout: ${output}\nstderr: ${error_output}")
    endif()
    require_machine_document("${name}" "${output}" document)
    validate_machine_schema("${name}" "${document}")
    require_json_equal("${name}_status" "${document}"
        succeeded status)
    require_json_equal("${name}_backend" "${document}"
        "${expected_backend}" execution requested_backend)
endfunction()

function(run_cli_machine_rejection name)
    execute_process(
        COMMAND ${mparser_command} ${ARGN}
        RESULT_VARIABLE actual_exit
        OUTPUT_VARIABLE output
        ERROR_VARIABLE error_output)
    if(NOT "${actual_exit}" STREQUAL "2" OR
       NOT error_output STREQUAL "")
        message(FATAL_ERROR
            "${name}: CLI rejection corrupted the machine channel\n"
            "exit: ${actual_exit}\nstdout: ${output}\n"
            "stderr: ${error_output}")
    endif()
    require_machine_document("${name}" "${output}" document)
    validate_machine_schema("${name}" "${document}")
    require_json_equal("${name}_status" "${document}"
        request-rejected status)
    require_json_equal("${name}_identifier" "${document}"
        MParser:CliError diagnostics 0 identifier)
endfunction()

run_machine_case(success 0 "${SUCCESS_SOURCE}")
require_json_equal(success_protocol_major "${success_JSON}" 1
    protocol major)
require_json_equal(success_engine_version "${success_JSON}"
    "${EXPECTED_VERSION}" engine version)
require_json_equal(success_status "${success_JSON}" succeeded status)
require_json_equal(success_backend "${success_JSON}" bytecode
    execution requested_backend)
require_json_equal(success_tier "${success_JSON}" bytecode
    execution effective_tier)

run_backend_case(backend_auto auto automatic)
run_backend_case(backend_off off bytecode)
run_backend_case(backend_portable portable portable)
run_backend_case(backend_native native native)

string(JSON workspace_length ERROR_VARIABLE json_error
    LENGTH "${success_JSON}" workspace)
if(json_error OR workspace_length LESS 7)
    message(FATAL_ERROR
        "success: expected the demo workspace in the result: ${json_error}")
endif()

set(found_matrix FALSE)
set(found_string FALSE)
set(found_structure FALSE)
math(EXPR workspace_last "${workspace_length} - 1")
foreach(index RANGE 0 ${workspace_last})
    string(JSON variable_name GET "${success_JSON}" workspace ${index} name)
    if(variable_name STREQUAL "matrix_value")
        require_json_equal(matrix_kind "${success_JSON}" numeric
            workspace ${index} value kind)
        require_json_equal(matrix_rows "${success_JSON}" 2
            workspace ${index} value dimensions 0)
        require_json_equal(matrix_column_major "${success_JSON}" 3
            workspace ${index} value data 1)
        set(found_matrix TRUE)
    elseif(variable_name STREQUAL "string_value")
        require_json_equal(string_kind "${success_JSON}" string
            workspace ${index} value kind)
        require_json_equal(string_column_major "${success_JSON}" gamma
            workspace ${index} value data 1)
        set(found_string TRUE)
    elseif(variable_name STREQUAL "struct_value")
        require_json_equal(struct_kind "${success_JSON}" struct
            workspace ${index} value kind)
        require_json_equal(struct_field "${success_JSON}" count
            workspace ${index} value fields 0)
        set(found_structure TRUE)
    endif()
endforeach()
if(NOT found_matrix OR NOT found_string OR NOT found_structure)
    message(FATAL_ERROR
        "success: expected matrix, string, and structure projections")
endif()

run_machine_case(compilation 1 "${COMPILE_FAILURE_SOURCE}")
require_json_equal(compilation_status "${compilation_JSON}"
    compilation-failed status)
require_json_equal(compilation_phase "${compilation_JSON}" compilation
    diagnostics 0 phase)

run_machine_case(rejection 2 "${SUCCESS_SOURCE}"
    --entry-function=missing_entry)
require_json_equal(rejection_status "${rejection_JSON}"
    request-rejected status)
require_json_equal(rejection_phase "${rejection_JSON}" validation
    diagnostics 0 phase)
require_json_equal(rejection_identifier "${rejection_JSON}"
    MParser:EntryFunctionNotFound diagnostics 0 identifier)

run_machine_case(runtime 3 "${RUNTIME_FAILURE_SOURCE}")
require_json_equal(runtime_status "${runtime_JSON}" runtime-failed status)
require_json_equal(runtime_phase "${runtime_JSON}" execution
    diagnostics 0 phase)
require_json_equal(runtime_identifier "${runtime_JSON}"
    MParserTest:MachineProtocolFailure diagnostics 0 identifier)

set(missing_source "${SUCCESS_SOURCE}.does-not-exist")
run_machine_case(source_load 2 "${missing_source}")
require_json_equal(source_load_identifier "${source_load_JSON}"
    MParser:SourceLoadFailed diagnostics 0 identifier)

execute_process(
    COMMAND ${mparser_command} --run --result-format=json-v1
    RESULT_VARIABLE cli_exit
    OUTPUT_VARIABLE cli_output
    ERROR_VARIABLE cli_error)
if(NOT "${cli_exit}" STREQUAL "2" OR NOT cli_error STREQUAL "")
    message(FATAL_ERROR
        "CLI failure did not preserve the machine channel contract")
endif()
require_machine_document("missing_input" "${cli_output}" cli_document)
validate_machine_schema("missing_input" "${cli_document}")
require_json_equal(cli_error_identifier "${cli_document}"
    MParser:CliError diagnostics 0 identifier)

execute_process(
    COMMAND ${mparser_command} --run --native-cache-stats
        --result-format=json-v1 "${SUCCESS_SOURCE}"
    RESULT_VARIABLE stats_exit
    OUTPUT_VARIABLE stats_output
    ERROR_VARIABLE stats_error)
if(NOT "${stats_exit}" STREQUAL "2" OR NOT stats_error STREQUAL "")
    message(FATAL_ERROR
        "native-cache rejection corrupted the machine output channel")
endif()
require_machine_document("native_cache_stats" "${stats_output}"
    stats_document)
validate_machine_schema("native_cache_stats" "${stats_document}")
require_json_equal(stats_error_identifier "${stats_document}"
    MParser:CliError diagnostics 0 identifier)

run_cli_machine_rejection(
    help_before_format --help --result-format=json-v1)
run_cli_machine_rejection(
    format_before_help --result-format=json-v1 --help)
run_cli_machine_rejection(
    version_before_format --version --result-format=json-v1)
run_cli_machine_rejection(
    format_before_version --result-format=json-v1 --version)
run_cli_machine_rejection(
    duplicate_format --run --result-format=json-v1
    --result-format=json-v1 "${SUCCESS_SOURCE}")
run_cli_machine_rejection(
    production_typed_selector --run --typed-backend=portable
    --result-format=json-v1 "${SUCCESS_SOURCE}")
