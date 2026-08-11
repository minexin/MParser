cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS SCHEMA GOLDEN EMERGENCY SNAPSHOT)
    if(NOT DEFINED ${required_variable} OR
       "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR
            "Missing machine-protocol contract variable: ${required_variable}")
    endif()
endforeach()

function(protocol_envelope_valid json out_variable)
    set(valid TRUE)
    string(JSON root_type ERROR_VARIABLE json_error TYPE "${json}")
    if(json_error OR NOT root_type STREQUAL "OBJECT")
        set(valid FALSE)
    endif()

    foreach(field IN ITEMS
            protocol engine status entry_function
            requested_output_count outputs workspace
            diagnostics execution)
        string(JSON field_type ERROR_VARIABLE json_error
            TYPE "${json}" "${field}")
        if(json_error)
            set(valid FALSE)
        endif()
    endforeach()

    string(JSON protocol_name ERROR_VARIABLE json_error
        GET "${json}" protocol name)
    if(json_error OR NOT protocol_name STREQUAL "mparser.result")
        set(valid FALSE)
    endif()
    string(JSON protocol_major ERROR_VARIABLE json_error
        GET "${json}" protocol major)
    if(json_error OR NOT protocol_major STREQUAL "1")
        set(valid FALSE)
    endif()
    string(JSON protocol_minor ERROR_VARIABLE json_error
        GET "${json}" protocol minor)
    if(json_error OR NOT protocol_minor MATCHES "^[0-9]+$")
        set(valid FALSE)
    endif()
    string(JSON engine_name ERROR_VARIABLE json_error
        GET "${json}" engine name)
    if(json_error OR NOT engine_name STREQUAL "MParser")
        set(valid FALSE)
    endif()
    string(JSON status ERROR_VARIABLE json_error
        GET "${json}" status)
    if(json_error OR
       NOT status MATCHES
           "^(succeeded|compilation-failed|request-rejected|runtime-failed)$")
        set(valid FALSE)
    endif()
    string(JSON output_count_type ERROR_VARIABLE json_error
        TYPE "${json}" requested_output_count)
    if(json_error OR NOT output_count_type STREQUAL "NUMBER")
        set(valid FALSE)
    endif()
    foreach(field IN ITEMS outputs workspace diagnostics)
        string(JSON array_type ERROR_VARIABLE json_error
            TYPE "${json}" "${field}")
        if(json_error OR NOT array_type STREQUAL "ARRAY")
            set(valid FALSE)
        endif()
    endforeach()

    foreach(field IN ITEMS
            profiling_collected fallback_occurred
            resource_controls_active optimized_execution_suppressed)
        string(JSON field_type ERROR_VARIABLE json_error
            TYPE "${json}" execution "${field}")
        if(json_error OR NOT field_type STREQUAL "BOOLEAN")
            set(valid FALSE)
        endif()
    endforeach()
    foreach(field IN ITEMS
            executed_instruction_count typed_region_count
            typed_region_attempt_count typed_region_execution_count
            typed_region_fallback_count native_compilation_count
            native_cache_hit_count maximum_call_depth
            maximum_array_bytes maximum_diagnostic_count
            elapsed_nanoseconds)
        string(JSON field_type ERROR_VARIABLE json_error
            TYPE "${json}" execution "${field}")
        if(json_error OR NOT field_type STREQUAL "NUMBER")
            set(valid FALSE)
        endif()
    endforeach()
    string(JSON requested_backend ERROR_VARIABLE json_error
        GET "${json}" execution requested_backend)
    if(json_error OR
       NOT requested_backend MATCHES
           "^(automatic|bytecode|portable|native)$")
        set(valid FALSE)
    endif()
    string(JSON effective_tier ERROR_VARIABLE json_error
        GET "${json}" execution effective_tier)
    if(json_error OR
       NOT effective_tier MATCHES
           "^(bytecode|portable|native|mixed)$")
        set(valid FALSE)
    endif()
    string(JSON stop_reason ERROR_VARIABLE json_error
        GET "${json}" execution stop_reason)
    if(json_error OR
       NOT stop_reason MATCHES
           "^(none|cancelled|instruction-limit|wall-time-limit|call-depth-limit|array-byte-limit|diagnostic-limit)$")
        set(valid FALSE)
    endif()

    set("${out_variable}" "${valid}" PARENT_SCOPE)
endfunction()

file(READ "${SCHEMA}" schema_json)
string(JSON schema_id ERROR_VARIABLE schema_error
    GET "${schema_json}" "$id")
string(JSON schema_draft ERROR_VARIABLE schema_draft_error
    GET "${schema_json}" "$schema")
string(JSON schema_major ERROR_VARIABLE schema_major_error
    GET "${schema_json}" properties protocol properties major const)
string(JSON schema_uint64_max ERROR_VARIABLE schema_uint64_error
    GET "${schema_json}" definitions uint64 x-mparser-maximum)
if(schema_error OR schema_draft_error OR schema_major_error OR
   schema_uint64_error OR
   NOT schema_id STREQUAL
       "https://mparser.dev/schema/machine-result-v1.json" OR
   NOT schema_draft STREQUAL
       "http://json-schema.org/draft-07/schema#" OR
   NOT schema_major STREQUAL "1" OR
   NOT schema_uint64_max STREQUAL "18446744073709551615")
    message(FATAL_ERROR
        "Machine protocol JSON Schema changed or is invalid")
endif()

file(READ "${GOLDEN}" golden_json)
file(READ "${EMERGENCY}" emergency_json)
file(READ "${SNAPSHOT}" snapshot_json)
protocol_envelope_valid("${golden_json}" golden_valid)
protocol_envelope_valid("${emergency_json}" emergency_valid)
protocol_envelope_valid("${snapshot_json}" snapshot_valid)
if(NOT golden_valid OR NOT emergency_valid OR NOT snapshot_valid)
    message(FATAL_ERROR
        "Machine protocol golden or historical snapshot is invalid")
endif()
string(JSON output_event_count ERROR_VARIABLE output_event_error
    LENGTH "${golden_json}" output_events)
string(JSON expression_count ERROR_VARIABLE expression_error
    LENGTH "${golden_json}" top_level_expressions)
string(JSON first_event_kind ERROR_VARIABLE first_event_error
    GET "${golden_json}" output_events 0 kind)
string(JSON first_expression_suppressed
    ERROR_VARIABLE first_expression_error
    GET "${golden_json}" top_level_expressions 0 output_suppressed)
if(output_event_error OR expression_error OR first_event_error OR
   first_expression_error OR NOT output_event_count EQUAL 2 OR
   NOT expression_count EQUAL 2 OR
   NOT first_event_kind STREQUAL "display" OR
   first_expression_suppressed)
    message(FATAL_ERROR
        "Current machine protocol golden lacks host-integration fields")
endif()
string(JSON emergency_version GET "${emergency_json}" engine version)
string(JSON emergency_status GET "${emergency_json}" status)
string(JSON emergency_identifier
    GET "${emergency_json}" diagnostics 0 identifier)
if(NOT emergency_version STREQUAL "unknown" OR
   NOT emergency_status STREQUAL "request-rejected" OR
   NOT emergency_identifier STREQUAL "MParser:ProtocolFailure")
    message(FATAL_ERROR
        "Machine protocol emergency contract changed")
endif()
string(FIND "${golden_json}"
    "\"executed_instruction_count\":18446744073709551615"
    uint64_offset)
if(uint64_offset LESS 0)
    message(FATAL_ERROR
        "Machine protocol no longer proves exact uint64 JSON integers")
endif()

set(valid_minimal
    "{\"protocol\":{\"name\":\"mparser.result\",\"major\":1,\"minor\":0},\"engine\":{\"name\":\"MParser\",\"version\":\"test\"},\"status\":\"succeeded\",\"entry_function\":\"\",\"requested_output_count\":0,\"outputs\":[],\"workspace\":[],\"diagnostics\":[],\"execution\":{\"requested_backend\":\"automatic\",\"effective_tier\":\"bytecode\",\"profiling_collected\":false,\"fallback_occurred\":false,\"resource_controls_active\":false,\"optimized_execution_suppressed\":false,\"stop_reason\":\"none\",\"executed_instruction_count\":0,\"typed_region_count\":0,\"typed_region_attempt_count\":0,\"typed_region_execution_count\":0,\"typed_region_fallback_count\":0,\"native_compilation_count\":0,\"native_cache_hit_count\":0,\"maximum_call_depth\":0,\"maximum_array_bytes\":0,\"maximum_diagnostic_count\":0,\"elapsed_nanoseconds\":0}}")
set(valid_additive
    "{\"protocol\":{\"name\":\"mparser.result\",\"major\":1,\"minor\":7,\"future\":true},\"engine\":{\"name\":\"MParser\",\"version\":\"test\"},\"status\":\"succeeded\",\"entry_function\":\"\",\"requested_output_count\":0,\"outputs\":[],\"workspace\":[],\"diagnostics\":[],\"execution\":{\"requested_backend\":\"automatic\",\"effective_tier\":\"bytecode\",\"profiling_collected\":false,\"fallback_occurred\":false,\"resource_controls_active\":false,\"optimized_execution_suppressed\":false,\"stop_reason\":\"none\",\"executed_instruction_count\":0,\"typed_region_count\":0,\"typed_region_attempt_count\":0,\"typed_region_execution_count\":0,\"typed_region_fallback_count\":0,\"native_compilation_count\":0,\"native_cache_hit_count\":0,\"maximum_call_depth\":0,\"maximum_array_bytes\":0,\"maximum_diagnostic_count\":0,\"elapsed_nanoseconds\":0,\"future_counter\":1},\"future_root\":{}}")
set(invalid_major
    "{\"protocol\":{\"name\":\"mparser.result\",\"major\":2,\"minor\":0},\"engine\":{\"name\":\"MParser\",\"version\":\"test\"},\"status\":\"succeeded\",\"entry_function\":\"\",\"requested_output_count\":0,\"outputs\":[],\"workspace\":[],\"diagnostics\":[],\"execution\":{}}")
set(invalid_missing_status
    "{\"protocol\":{\"name\":\"mparser.result\",\"major\":1,\"minor\":0},\"engine\":{\"name\":\"MParser\",\"version\":\"test\"},\"entry_function\":\"\",\"requested_output_count\":0,\"outputs\":[],\"workspace\":[],\"diagnostics\":[],\"execution\":{}}")
string(REPLACE
    "\"requested_output_count\":0"
    "\"requested_output_count\":\"0\""
    invalid_count_type "${valid_minimal}")
string(REPLACE
    "\"status\":\"succeeded\""
    "\"status\":\"future-status\""
    invalid_status "${valid_minimal}")
string(REPLACE
    ",\"elapsed_nanoseconds\":0"
    ""
    invalid_execution "${valid_minimal}")

protocol_envelope_valid("${valid_minimal}" valid_minimal_result)
protocol_envelope_valid("${valid_additive}" valid_additive_result)
protocol_envelope_valid("${invalid_major}" invalid_major_result)
protocol_envelope_valid(
    "${invalid_missing_status}" invalid_missing_status_result)
protocol_envelope_valid(
    "${invalid_count_type}" invalid_count_type_result)
protocol_envelope_valid("${invalid_status}" invalid_status_result)
protocol_envelope_valid(
    "${invalid_execution}" invalid_execution_result)
if(NOT valid_minimal_result OR NOT valid_additive_result OR
   invalid_major_result OR invalid_missing_status_result OR
   invalid_count_type_result OR invalid_status_result OR
   invalid_execution_result)
    message(FATAL_ERROR
        "Machine protocol reference-consumer positive/negative cases failed")
endif()

message(STATUS
    "MParser machine protocol contract validated: schema 1, "
    "exact uint64, emergency, additive minor, five negative cases")
