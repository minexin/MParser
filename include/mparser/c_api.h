#ifndef MPARSER_C_API_H
#define MPARSER_C_API_H

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#if defined(MPARSER_C_API_BUILD)
#define MPARSER_C_API __declspec(dllexport)
#elif defined(MPARSER_C_API_SHARED)
#define MPARSER_C_API __declspec(dllimport)
#else
#define MPARSER_C_API
#endif
#elif defined(__GNUC__) || defined(__clang__)
#define MPARSER_C_API __attribute__((visibility("default")))
#else
#define MPARSER_C_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define MPARSER_C_ABI_VERSION 1u

typedef uint32_t mparser_api_status;
#define MPARSER_API_STATUS_OK 0u
#define MPARSER_API_STATUS_INVALID_ARGUMENT 1u
#define MPARSER_API_STATUS_OUT_OF_RANGE 2u
#define MPARSER_API_STATUS_TYPE_MISMATCH 3u
#define MPARSER_API_STATUS_OWNER_MISMATCH 4u
#define MPARSER_API_STATUS_COMPILATION_FAILED 5u
#define MPARSER_API_STATUS_ALLOCATION_FAILED 6u
#define MPARSER_API_STATUS_INTERNAL_ERROR 7u
#define MPARSER_API_STATUS_ABI_MISMATCH 8u

typedef uint32_t mparser_invocation_status;
#define MPARSER_INVOCATION_SUCCEEDED 0u
#define MPARSER_INVOCATION_COMPILATION_FAILED 1u
#define MPARSER_INVOCATION_REQUEST_REJECTED 2u
#define MPARSER_INVOCATION_RUNTIME_FAILED 3u

typedef uint32_t mparser_backend;
#define MPARSER_BACKEND_AUTOMATIC 0u
#define MPARSER_BACKEND_BYTECODE 1u
#define MPARSER_BACKEND_PORTABLE 2u
#define MPARSER_BACKEND_NATIVE 3u

typedef uint32_t mparser_execution_tier;
#define MPARSER_EXECUTION_TIER_BYTECODE 0u
#define MPARSER_EXECUTION_TIER_PORTABLE 1u
#define MPARSER_EXECUTION_TIER_NATIVE 2u
#define MPARSER_EXECUTION_TIER_MIXED 3u

typedef uint32_t mparser_stop_reason;
#define MPARSER_STOP_NONE 0u
#define MPARSER_STOP_CANCELLED 1u
#define MPARSER_STOP_INSTRUCTION_LIMIT 2u
#define MPARSER_STOP_WALL_TIME_LIMIT 3u
#define MPARSER_STOP_CALL_DEPTH_LIMIT 4u
#define MPARSER_STOP_ARRAY_BYTE_LIMIT 5u
#define MPARSER_STOP_DIAGNOSTIC_LIMIT 6u

typedef uint32_t mparser_diagnostic_phase;
#define MPARSER_DIAGNOSTIC_COMPILATION 0u
#define MPARSER_DIAGNOSTIC_VALIDATION 1u
#define MPARSER_DIAGNOSTIC_EXECUTION 2u

typedef uint32_t mparser_diagnostic_severity;
#define MPARSER_DIAGNOSTIC_ERROR 0u
#define MPARSER_DIAGNOSTIC_WARNING 1u

typedef uint32_t mparser_value_kind;
#define MPARSER_VALUE_MISSING 0u
#define MPARSER_VALUE_NUMERIC 1u
#define MPARSER_VALUE_CHARACTER 2u
#define MPARSER_VALUE_STRING 3u
#define MPARSER_VALUE_CELL 4u
#define MPARSER_VALUE_STRUCT 5u
#define MPARSER_VALUE_OBJECT 6u
#define MPARSER_VALUE_FUNCTION_HANDLE 7u

typedef uint32_t mparser_numeric_class;
#define MPARSER_NUMERIC_DOUBLE 0u
#define MPARSER_NUMERIC_LOGICAL 1u

typedef struct mparser_module mparser_module;
typedef struct mparser_session mparser_session;
typedef struct mparser_result mparser_result;
typedef struct mparser_value mparser_value;
typedef struct mparser_cancel_token mparser_cancel_token;
typedef struct mparser_diagnostic mparser_diagnostic;

typedef struct mparser_utf8_view {
    const char* data;
    size_t size;
} mparser_utf8_view;

typedef struct mparser_utf16_view {
    const uint16_t* data;
    size_t size;
    uint32_t missing;
} mparser_utf16_view;

typedef struct mparser_named_value {
    const char* name;
    size_t name_size;
    const mparser_value* value;
} mparser_named_value;

typedef struct mparser_source_position {
    uint64_t offset;
    int32_t line;
    int32_t column;
} mparser_source_position;

typedef struct mparser_invocation_options {
    uint32_t struct_size;
    uint32_t abi_version;
    const char* entry_name;
    size_t entry_name_size;
    const mparser_value* const* arguments;
    size_t argument_count;
    size_t requested_output_count;
    uint32_t has_requested_output_count;
    const mparser_named_value* initial_workspace;
    size_t initial_workspace_count;
    mparser_backend backend;
    uint32_t collect_profile;
    uint64_t max_instruction_count;
    uint64_t max_wall_time_nanoseconds;
    uint64_t max_call_depth;
    uint64_t max_array_bytes;
    uint64_t max_diagnostic_count;
    const mparser_cancel_token* cancellation_token;
} mparser_invocation_options;

typedef struct mparser_execution_summary {
    uint32_t struct_size;
    uint32_t abi_version;
    mparser_backend requested_backend;
    mparser_execution_tier effective_tier;
    uint32_t profiling_collected;
    uint32_t fallback_occurred;
    uint32_t resource_controls_active;
    uint32_t optimized_execution_suppressed;
    mparser_stop_reason stop_reason;
    uint64_t executed_instruction_count;
    uint64_t typed_region_count;
    uint64_t typed_region_attempt_count;
    uint64_t typed_region_execution_count;
    uint64_t typed_region_fallback_count;
    uint64_t native_compilation_count;
    uint64_t native_cache_hit_count;
    uint64_t maximum_call_depth;
    uint64_t maximum_array_bytes;
    uint64_t maximum_diagnostic_count;
    uint64_t elapsed_nanoseconds;
} mparser_execution_summary;

MPARSER_C_API uint32_t mparser_c_abi_version(void);
MPARSER_C_API uint32_t mparser_version_major(void);
MPARSER_C_API uint32_t mparser_version_minor(void);
MPARSER_C_API uint32_t mparser_version_patch(void);
MPARSER_C_API mparser_utf8_view
mparser_api_status_name(mparser_api_status status);

MPARSER_C_API mparser_api_status
mparser_invocation_options_init(mparser_invocation_options* options);
MPARSER_C_API mparser_api_status
mparser_execution_summary_init(mparser_execution_summary* summary);

MPARSER_C_API mparser_api_status mparser_module_compile_utf8(
    const char* source,
    size_t source_size,
    const char* source_name,
    size_t source_name_size,
    mparser_module** out_module);
MPARSER_C_API void mparser_module_retain(mparser_module* module);
MPARSER_C_API void mparser_module_release(mparser_module* module);
MPARSER_C_API uint32_t
mparser_module_is_valid(const mparser_module* module);
MPARSER_C_API size_t
mparser_module_diagnostic_count(const mparser_module* module);
MPARSER_C_API const mparser_diagnostic*
mparser_module_diagnostic(const mparser_module* module, size_t index);
MPARSER_C_API size_t
mparser_module_function_count(const mparser_module* module);
MPARSER_C_API mparser_utf8_view
mparser_module_function_name(const mparser_module* module, size_t index);
MPARSER_C_API mparser_api_status mparser_module_execute(
    const mparser_module* module,
    const mparser_invocation_options* options,
    mparser_result** out_result);
MPARSER_C_API mparser_api_status mparser_module_create_session(
    const mparser_module* module,
    mparser_session** out_session);

MPARSER_C_API void mparser_session_retain(mparser_session* session);
MPARSER_C_API void mparser_session_release(mparser_session* session);
MPARSER_C_API mparser_api_status mparser_session_execute(
    mparser_session* session,
    const mparser_invocation_options* options,
    mparser_result** out_result);
MPARSER_C_API mparser_api_status
mparser_session_clear_global(mparser_session* session,
                             const char* name,
                             size_t name_size);
MPARSER_C_API mparser_api_status
mparser_session_clear_globals(mparser_session* session);
MPARSER_C_API mparser_api_status
mparser_session_reset(mparser_session* session);

MPARSER_C_API void mparser_result_retain(mparser_result* result);
MPARSER_C_API void mparser_result_release(mparser_result* result);
MPARSER_C_API mparser_invocation_status
mparser_result_status(const mparser_result* result);
MPARSER_C_API uint32_t
mparser_result_succeeded(const mparser_result* result);
MPARSER_C_API mparser_utf8_view
mparser_result_entry_name(const mparser_result* result);
MPARSER_C_API size_t
mparser_result_requested_output_count(const mparser_result* result);
MPARSER_C_API size_t
mparser_result_output_count(const mparser_result* result);
MPARSER_C_API mparser_utf8_view
mparser_result_output_name(const mparser_result* result, size_t index);
MPARSER_C_API mparser_api_status mparser_result_output(
    const mparser_result* result,
    size_t index,
    mparser_value** out_value);
MPARSER_C_API size_t
mparser_result_variable_count(const mparser_result* result);
MPARSER_C_API mparser_api_status mparser_result_variable(
    const mparser_result* result,
    size_t index,
    mparser_utf8_view* out_name,
    mparser_value** out_value);
MPARSER_C_API size_t
mparser_result_diagnostic_count(const mparser_result* result);
MPARSER_C_API const mparser_diagnostic*
mparser_result_diagnostic(const mparser_result* result, size_t index);
MPARSER_C_API mparser_api_status mparser_result_execution_summary(
    const mparser_result* result,
    mparser_execution_summary* out_summary);

MPARSER_C_API mparser_diagnostic_phase
mparser_diagnostic_get_phase(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_diagnostic_severity
mparser_diagnostic_get_severity(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_utf8_view
mparser_diagnostic_identifier(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_utf8_view
mparser_diagnostic_message(const mparser_diagnostic* diagnostic);
MPARSER_C_API uint32_t
mparser_diagnostic_has_source(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_utf8_view
mparser_diagnostic_source_name(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_source_position
mparser_diagnostic_source_begin(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_source_position
mparser_diagnostic_source_end(const mparser_diagnostic* diagnostic);
MPARSER_C_API size_t
mparser_diagnostic_stack_count(const mparser_diagnostic* diagnostic);
MPARSER_C_API mparser_utf8_view mparser_diagnostic_stack_source(
    const mparser_diagnostic* diagnostic, size_t index);
MPARSER_C_API mparser_utf8_view mparser_diagnostic_stack_function(
    const mparser_diagnostic* diagnostic, size_t index);
MPARSER_C_API int32_t mparser_diagnostic_stack_line(
    const mparser_diagnostic* diagnostic, size_t index);
MPARSER_C_API size_t
mparser_diagnostic_cause_count(const mparser_diagnostic* diagnostic);
MPARSER_C_API const mparser_diagnostic*
mparser_diagnostic_cause(const mparser_diagnostic* diagnostic,
                         size_t index);

MPARSER_C_API mparser_api_status
mparser_value_create_missing(mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_scalar(
    double value,
    mparser_numeric_class numeric_class,
    mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_numeric_array(
    mparser_numeric_class numeric_class,
    const size_t* dimensions,
    size_t rank,
    const double* column_major_elements,
    size_t element_count,
    mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_character_array(
    const size_t* dimensions,
    size_t rank,
    const uint16_t* column_major_code_units,
    size_t code_unit_count,
    mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_string_array(
    const size_t* dimensions,
    size_t rank,
    const mparser_utf16_view* column_major_elements,
    size_t element_count,
    mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_cell(
    const size_t* dimensions,
    size_t rank,
    const mparser_value* const* column_major_elements,
    size_t element_count,
    mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_struct(
    const mparser_named_value* fields,
    size_t field_count,
    mparser_value** out_value);
MPARSER_C_API void mparser_value_retain(mparser_value* value);
MPARSER_C_API void mparser_value_release(mparser_value* value);
MPARSER_C_API mparser_value_kind
mparser_value_get_kind(const mparser_value* value);
MPARSER_C_API mparser_numeric_class
mparser_value_get_numeric_class(const mparser_value* value);
MPARSER_C_API uint32_t
mparser_value_is_module_bound(const mparser_value* value);
MPARSER_C_API size_t
mparser_value_rank(const mparser_value* value);
MPARSER_C_API mparser_api_status mparser_value_dimension(
    const mparser_value* value,
    size_t index,
    size_t* out_dimension);
MPARSER_C_API size_t
mparser_value_element_count(const mparser_value* value);
MPARSER_C_API mparser_api_status mparser_value_numeric_data(
    const mparser_value* value,
    const double** out_column_major_elements,
    size_t* out_element_count);
MPARSER_C_API mparser_api_status mparser_value_character_data(
    const mparser_value* value,
    const uint16_t** out_column_major_code_units,
    size_t* out_code_unit_count);
MPARSER_C_API mparser_api_status mparser_value_string_element(
    const mparser_value* value,
    size_t index,
    mparser_utf16_view* out_element);
MPARSER_C_API mparser_api_status mparser_value_cell_element(
    const mparser_value* value,
    size_t index,
    mparser_value** out_element);
MPARSER_C_API size_t
mparser_value_struct_field_count(const mparser_value* value);
MPARSER_C_API mparser_utf8_view mparser_value_struct_field_name(
    const mparser_value* value, size_t field_index);
MPARSER_C_API mparser_api_status mparser_value_struct_field(
    const mparser_value* value,
    size_t element_index,
    size_t field_index,
    mparser_value** out_field);
MPARSER_C_API mparser_utf8_view
mparser_value_class_name(const mparser_value* value);
MPARSER_C_API mparser_utf8_view
mparser_value_function_text(const mparser_value* value);

MPARSER_C_API mparser_api_status
mparser_cancel_token_create(mparser_cancel_token** out_token);
MPARSER_C_API void
mparser_cancel_token_retain(mparser_cancel_token* token);
MPARSER_C_API void
mparser_cancel_token_release(mparser_cancel_token* token);
MPARSER_C_API void
mparser_cancel_token_request(mparser_cancel_token* token);
MPARSER_C_API uint32_t mparser_cancel_token_is_requested(
    const mparser_cancel_token* token);

#ifdef __cplusplus
}
#endif

#endif
