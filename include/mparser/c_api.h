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

#define MPARSER_C_API_VERSION_MAJOR 1u
#define MPARSER_C_API_VERSION_MINOR 15u
#define MPARSER_C_API_VERSION_PATCH 0u
#define MPARSER_C_ABI_GENERATION 2u
#define MPARSER_C_ABI_REVISION 9u

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
#define MPARSER_API_STATUS_SOURCE_LOAD_FAILED 9u
#define MPARSER_API_STATUS_SYSTEM_CONTEXT_FAILED 10u

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

typedef uint32_t mparser_source_kind;
#define MPARSER_SOURCE_UNKNOWN 0u
#define MPARSER_SOURCE_SCRIPT 1u
#define MPARSER_SOURCE_FUNCTION 2u
#define MPARSER_SOURCE_CLASS 3u

typedef uint32_t mparser_output_kind;
#define MPARSER_OUTPUT_DISPLAY 0u
#define MPARSER_OUTPUT_STANDARD 1u

typedef uint32_t mparser_output_disposition;
#define MPARSER_OUTPUT_ACCEPT 0u
#define MPARSER_OUTPUT_REJECT 1u

typedef uint32_t mparser_system_capability;
#define MPARSER_SYSTEM_CAPABILITY_NONE 0u
#define MPARSER_SYSTEM_CAPABILITY_CURRENT_DIRECTORY (1u << 0u)
#define MPARSER_SYSTEM_CAPABILITY_SEARCH_PATHS (1u << 1u)
#define MPARSER_SYSTEM_CAPABILITY_ENVIRONMENT_READ (1u << 2u)
#define MPARSER_SYSTEM_CAPABILITY_FILESYSTEM_READ (1u << 3u)
#define MPARSER_SYSTEM_CAPABILITY_PROCESS (1u << 4u)
#define MPARSER_SYSTEM_CAPABILITY_CLOCK (1u << 5u)
#define MPARSER_SYSTEM_CAPABILITY_SLEEP (1u << 6u)
#define MPARSER_SYSTEM_CAPABILITY_RANDOM (1u << 7u)
#define MPARSER_SYSTEM_CAPABILITY_DYNAMIC_EVALUATION (1u << 8u)
#define MPARSER_SYSTEM_CAPABILITY_FILESYSTEM_WRITE (1u << 9u)

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
#define MPARSER_NUMERIC_SINGLE 2u
#define MPARSER_NUMERIC_INT8 3u
#define MPARSER_NUMERIC_UINT8 4u
#define MPARSER_NUMERIC_INT16 5u
#define MPARSER_NUMERIC_UINT16 6u
#define MPARSER_NUMERIC_INT32 7u
#define MPARSER_NUMERIC_UINT32 8u
#define MPARSER_NUMERIC_INT64 9u
#define MPARSER_NUMERIC_UINT64 10u

typedef struct mparser_numeric_buffer {
    mparser_numeric_class numeric_class;
    uint32_t is_complex;
    const void* real_data;
    const void* imaginary_data;
    size_t element_count;
} mparser_numeric_buffer;

/*
 * Threading and lifetime contract:
 * - Retain/release is atomic while at least one live reference remains.
 * - A thread must own its retained reference before another thread may
 *   release the same handle.
 * - Immutable handles and borrowed views may be read through independent
 *   retained references.
 * - Stateless executions without module-bound values may run concurrently.
 * - Module-bound mutable graphs and all sessions from one module are
 *   serialized by that module. Operations on one session are also ordered.
 * - A shared runtime serializes all attached module executions and is the
 *   only boundary that permits module-bound values to cross modules.
 * - Do not concurrently overwrite, release, or otherwise mutate one host
 *   handle variable. Retain a separate reference for each thread.
 */
typedef struct mparser_module mparser_module;
typedef struct mparser_session mparser_session;
typedef struct mparser_runtime mparser_runtime;
typedef struct mparser_result mparser_result;
typedef struct mparser_value mparser_value;
typedef struct mparser_cancel_token mparser_cancel_token;
typedef struct mparser_diagnostic mparser_diagnostic;
typedef struct mparser_system_context mparser_system_context;
typedef struct mparser_debugger mparser_debugger;
typedef struct mparser_debug_event mparser_debug_event;

typedef struct mparser_utf8_view {
    const char* data;
    size_t size;
} mparser_utf8_view;

typedef struct mparser_utf16_view {
    const uint16_t* data;
    size_t size;
    uint32_t missing;
} mparser_utf16_view;

typedef struct mparser_source_unit {
    uint32_t struct_size;
    uint32_t abi_generation;
    const char* source_name;
    size_t source_name_size;
    const char* source;
    size_t source_size;
} mparser_source_unit;

typedef struct mparser_source_load_options {
    uint32_t struct_size;
    uint32_t abi_generation;
    const mparser_utf8_view* search_paths;
    size_t search_path_count;
} mparser_source_load_options;

/*
 * Creates a native context whose path-oriented operations are constrained to
 * root_directory. Environment and process capabilities are host-wide and
 * must be enabled explicitly. The root is not a security boundary against a
 * hostile local process racing filesystem links while a call is in flight.
 */
typedef struct mparser_system_context_options {
    uint32_t struct_size;
    uint32_t abi_generation;
    mparser_system_capability capabilities;
    mparser_utf8_view root_directory;
    mparser_utf8_view current_directory;
    mparser_utf8_view temporary_directory;
    const mparser_utf8_view* search_paths;
    size_t search_path_count;
    uint64_t random_seed;
    uint64_t maximum_open_files;
    uint64_t maximum_file_read_bytes;
} mparser_system_context_options;

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

typedef uint32_t mparser_definition_kind;
#define MPARSER_DEFINITION_PRIMARY_FUNCTION 0u
#define MPARSER_DEFINITION_LOCAL_FUNCTION 1u
#define MPARSER_DEFINITION_NESTED_FUNCTION 2u
#define MPARSER_DEFINITION_METHOD 3u
#define MPARSER_DEFINITION_NO_PARENT ((size_t)-1)

typedef struct mparser_definition_info {
    mparser_utf8_view name;
    mparser_utf8_view class_name;
    mparser_utf8_view source_name;
    size_t source_index;
    size_t parent_function;
    mparser_definition_kind kind;
    uint32_t declaration_only;
    mparser_source_position source_begin;
    mparser_source_position source_end;
} mparser_definition_info;

/* Definition indices are module-local, in syntax traversal order. Parent
 * indices refer to this directory, not the callable-function list. Strings
 * are borrowed until module release. Positions use zero-based UTF-8 byte
 * offsets, one-based lines/byte columns, and exclusive end positions.
 * No execution occurs. Invalid modules may expose only a partial directory.
 * Errors leave out_info untouched; out_size must cover the current struct. */
MPARSER_C_API size_t mparser_module_definition_count(const mparser_module* module);
MPARSER_C_API mparser_api_status mparser_module_definition_info(
    const mparser_module* module, size_t index,
    mparser_definition_info* out_info, size_t out_size);

typedef uint32_t mparser_debug_action;
#define MPARSER_DEBUG_CONTINUE 0u
#define MPARSER_DEBUG_STEP_INTO 1u
#define MPARSER_DEBUG_STEP_OVER 2u
#define MPARSER_DEBUG_STEP_OUT 3u
#define MPARSER_DEBUG_STOP 4u

typedef uint32_t mparser_debug_reason;
#define MPARSER_DEBUG_BREAKPOINT 0u
#define MPARSER_DEBUG_PAUSE_REQUEST 1u
#define MPARSER_DEBUG_STEP 2u

typedef uint32_t mparser_debug_frame_kind;
#define MPARSER_DEBUG_FRAME_SCRIPT 0u
#define MPARSER_DEBUG_FRAME_FUNCTION 1u
#define MPARSER_DEBUG_FRAME_ANONYMOUS 2u
#define MPARSER_DEBUG_FRAME_INITIALIZER 3u

typedef struct mparser_breakpoint {
    mparser_utf8_view source_name;
    int32_t line;
} mparser_breakpoint;

typedef struct mparser_conditional_breakpoint {
    mparser_utf8_view source_name;
    int32_t line;
    mparser_utf8_view condition;
} mparser_conditional_breakpoint;

typedef struct mparser_debug_frame_info {
    mparser_debug_frame_kind kind;
    mparser_utf8_view function_name;
    mparser_utf8_view source_name;
    mparser_source_position source_begin;
    mparser_source_position source_end;
    size_t supplied_argument_count;
    size_t requested_output_count;
    size_t variable_count;
} mparser_debug_frame_info;

/* The event and its views are borrowed on this thread until the callback returns. The host
 * may block the execution thread here, inspect the event, then return a resume
 * action. Only mparser_debug_event_evaluate may execute in the paused workspace.
 * Do not otherwise execute or mutate the paused module/session/runtime from the
 * callback, or wait for an operation requiring their execution locks. */
typedef mparser_debug_action (*mparser_debug_sink_callback)(
    void* user_data, const mparser_debug_event* event);

typedef mparser_output_disposition (*mparser_output_sink_callback)(
    void* user_data,
    uint64_t sequence,
    mparser_output_kind kind,
    const char* text,
    size_t text_size,
    const char* source_name,
    size_t source_name_size,
    mparser_source_position source_begin,
    mparser_source_position source_end);

typedef uint32_t mparser_input_status;
#define MPARSER_INPUT_READY 0u
#define MPARSER_INPUT_PENDING 1u
#define MPARSER_INPUT_END 2u
#define MPARSER_INPUT_ERROR 3u

typedef uint32_t mparser_input_mode;
#define MPARSER_INPUT_EXPRESSION 0u
#define MPARSER_INPUT_TEXT 1u
#define MPARSER_INPUT_COMMAND 2u

/* Return promptly; Pending allows cancellation and timeout checks. The prompt
 * is borrowed for this call. Returned text/error buffers must remain valid
 * until the next callback or completion of execution, whichever comes first. */
typedef mparser_input_status (*mparser_input_source_callback)(
    void* user_data, mparser_input_mode mode, mparser_utf8_view prompt,
    mparser_utf8_view* text, mparser_utf8_view* error);

/* Optional result collection, not a live callback. Budgets bound the returned
 * collection's accounted payload, not execution-time journal/legacy/workspace
 * memory or allocator overhead. Zero is an exact zero budget. Truncation does
 * not stop execution or alter evalc output; check the returned summary. */
typedef struct mparser_event_capture_options {
    uint32_t struct_size;
    uint32_t abi_generation;
    uint64_t maximum_events;
    uint64_t maximum_bytes;
    uint64_t maximum_value_bytes;
    uint64_t maximum_value_nodes;
    uint64_t maximum_value_depth;
} mparser_event_capture_options;

#define MPARSER_EVENT_CAPTURE_OPTIONS_SIZE ((uint32_t)sizeof(mparser_event_capture_options))
#define MPARSER_EVENT_CAPTURE_OPTIONS_INIT(options) \
    mparser_event_capture_options_init_sized((options),sizeof(*(options)),MPARSER_C_ABI_GENERATION)
MPARSER_C_API mparser_api_status mparser_event_capture_options_init(mparser_event_capture_options* options);
MPARSER_C_API mparser_api_status mparser_event_capture_options_init_sized(
    void* storage,size_t storage_size,uint32_t abi_generation);

typedef struct mparser_invocation_options {
    uint32_t struct_size;
    uint32_t abi_generation;
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
    mparser_output_sink_callback output_sink;
    void* output_user_data;
    /* Optional ABI revision 3 tail; ignored when absent from struct_size. */
    const mparser_debugger* debugger;
    mparser_input_source_callback input_source;
    void* input_user_data;
    /* Optional ABI revision 9 tail. Null/absent preserves legacy behavior. */
    const mparser_event_capture_options* event_capture;
} mparser_invocation_options;

typedef uint32_t mparser_execution_event_kind;
#define MPARSER_EVENT_ASSIGNMENT 0u
#define MPARSER_EVENT_EXPRESSION 1u
#define MPARSER_EVENT_TEXT 2u
#define MPARSER_EVENT_WARNING 3u
#define MPARSER_EVENT_ERROR 4u
typedef uint32_t mparser_execution_event_disposition;
#define MPARSER_EVENT_VISIBLE 0u
#define MPARSER_EVENT_RAISED 1u
#define MPARSER_EVENT_CAUGHT 2u
#define MPARSER_EVENT_SUPPRESSED 3u
#define MPARSER_EVENT_UNHANDLED 4u
typedef uint32_t mparser_event_truncation_reason;
#define MPARSER_EVENT_TRUNCATION_NONE 0u
#define MPARSER_EVENT_TRUNCATION_EVENT_COUNT 1u
#define MPARSER_EVENT_TRUNCATION_BYTE_LIMIT 2u

typedef struct mparser_execution_event_info {
    uint32_t struct_size;
    uint32_t abi_generation;
    mparser_execution_event_kind kind;
    mparser_execution_event_disposition disposition;
    uint64_t sequence;
    uint64_t source_context;
    uint64_t capture_depth;
    uint32_t output_suppressed;
    uint32_t line_spacing; /* 0 loose, 1 compact */
    uint32_t source_available;
    mparser_utf8_view source_name;
    mparser_source_position source_begin;
    mparser_source_position source_end;
    mparser_utf8_view label;
    mparser_utf8_view text;
    const mparser_diagnostic* diagnostic;
    /* Empty on non-value events or unavailable snapshots. This is a version-1
     * mparser.value-snapshot graph, never a writable/invocable SDK Value. */
    mparser_utf8_view value_snapshot_json;
    /* 0 none, 1 invalid value, 2 depth, 3 nodes, 4 bytes. */
    uint32_t value_failure;
    mparser_utf8_view value_unavailable;
} mparser_execution_event_info;

typedef struct mparser_event_capture_summary {
    uint32_t struct_size;
    uint32_t abi_generation;
    uint32_t enabled;
    uint32_t started;
    uint32_t truncated;
    mparser_event_truncation_reason truncation_reason;
    uint64_t generated_count;
    uint64_t retained_count;
    uint64_t retained_bytes;
} mparser_event_capture_summary;

/* All views are borrowed until result release; getters perform no execution,
 * invoke no getters/user display and never mutate a snapshot. Failed sized
 * queries preserve caller output. Source positions are UTF-8 byte based,
 * one-based lines/columns and exclusive ends. Capture depth isolates evalc;
 * retained audit events at nonzero depth are not ordinary console output. */
MPARSER_C_API size_t mparser_result_execution_event_count(const mparser_result* result);
MPARSER_C_API mparser_api_status mparser_result_execution_event_info(
    const mparser_result* result,size_t index,mparser_execution_event_info* out_info,size_t out_size);
MPARSER_C_API mparser_api_status mparser_result_event_capture_summary(
    const mparser_result* result,mparser_event_capture_summary* out_summary,size_t out_size);

typedef struct mparser_execution_summary {
    uint32_t struct_size;
    uint32_t abi_generation;
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

/*
 * ABI-generation-2 structure sizes. The extensible structures may gain
 * tail fields without changing their minimum sizes. mparser_source_unit is
 * sealed because arrays use sizeof(mparser_source_unit) as their stride.
 */
#define MPARSER_INVOCATION_OPTIONS_SIZE                                 \
    ((uint32_t)(offsetof(mparser_invocation_options, output_user_data) + \
                sizeof(((mparser_invocation_options*)0)->output_user_data)))
#define MPARSER_EXECUTION_SUMMARY_SIZE                                  \
    ((uint32_t)(offsetof(mparser_execution_summary, elapsed_nanoseconds) + \
                sizeof(((mparser_execution_summary*)0)->elapsed_nanoseconds)))
#define MPARSER_SOURCE_LOAD_OPTIONS_SIZE                                \
    ((uint32_t)(offsetof(mparser_source_load_options, search_path_count) + \
                sizeof(((mparser_source_load_options*)0)->search_path_count)))
#define MPARSER_SYSTEM_CONTEXT_OPTIONS_SIZE                             \
    ((uint32_t)(offsetof(mparser_system_context_options,                \
                         maximum_file_read_bytes) +                     \
                sizeof(((mparser_system_context_options*)0)             \
                           ->maximum_file_read_bytes)))
#define MPARSER_SOURCE_UNIT_SIZE                                        \
    ((uint32_t)(offsetof(mparser_source_unit, source_size) +            \
                sizeof(((mparser_source_unit*)0)->source_size)))

#define MPARSER_INVOCATION_OPTIONS_INIT(options)                        \
    mparser_invocation_options_init_sized(                              \
        (options), sizeof(*(options)), MPARSER_C_ABI_GENERATION)
#define MPARSER_EXECUTION_SUMMARY_INIT(summary)                         \
    mparser_execution_summary_init_sized(                              \
        (summary), sizeof(*(summary)), MPARSER_C_ABI_GENERATION)
#define MPARSER_SOURCE_LOAD_OPTIONS_INIT(options)                       \
    mparser_source_load_options_init_sized(                            \
        (options), sizeof(*(options)), MPARSER_C_ABI_GENERATION)
#define MPARSER_SYSTEM_CONTEXT_OPTIONS_INIT(options)                    \
    mparser_system_context_options_init_sized(                         \
        (options), sizeof(*(options)), MPARSER_C_ABI_GENERATION)

MPARSER_C_API uint32_t mparser_c_abi_generation(void);
MPARSER_C_API uint32_t mparser_c_abi_revision(void);
MPARSER_C_API uint32_t mparser_version_major(void);
MPARSER_C_API uint32_t mparser_version_minor(void);
MPARSER_C_API uint32_t mparser_version_patch(void);
typedef uint32_t mparser_variable_name_status;
#define MPARSER_VARIABLE_NAME_VALID 0u
#define MPARSER_VARIABLE_NAME_EMPTY 1u
#define MPARSER_VARIABLE_NAME_TOO_LONG 2u
#define MPARSER_VARIABLE_NAME_INVALID_START 3u
#define MPARSER_VARIABLE_NAME_INVALID_CHARACTER 4u
#define MPARSER_VARIABLE_NAME_RESERVED_KEYWORD 5u

/* Stateless R2024b name validation. ASCII letter first, then ASCII letters,
 * digits or underscore, no reserved keyword. No compilation or execution.
 * Null data is allowed only for size zero. out_status must be non-null.
 * Invalid API arguments return INVALID_ARGUMENT without modifying out_status.
 * Name failures return OK with the reason. Checks are ordered: empty, length,
 * first character, remaining characters, keyword. Embedded NUL is invalid. */
MPARSER_C_API size_t mparser_variable_name_max_length(void);
MPARSER_C_API mparser_api_status mparser_validate_variable_name(
    mparser_utf8_view name, mparser_variable_name_status* out_status);
MPARSER_C_API mparser_utf8_view
mparser_api_status_name(mparser_api_status status);

/* A debugger is single-execution. Replace breakpoints and request pause may
 * run concurrently with execution. Source names match the compiled source
 * names exactly; a breakpoint stops before every executable statement on its
 * line. Installing a debugger suppresses optimized regions for that call.
 * User data must remain alive until all invocations using the handle finish.
 * Invalid resume actions terminate with MParser:DebugCallbackFailed. */
MPARSER_C_API mparser_api_status mparser_debugger_create(
    mparser_debug_sink_callback sink, void* user_data,
    mparser_debugger** out_debugger);
MPARSER_C_API void mparser_debugger_retain(mparser_debugger* debugger);
MPARSER_C_API void mparser_debugger_release(mparser_debugger* debugger);
MPARSER_C_API mparser_api_status mparser_debugger_set_breakpoints(
    mparser_debugger* debugger, const mparser_breakpoint* points, size_t count);
/* Replaces the entire breakpoint set. Empty conditions are unconditional.
 * Conditions run in the current frame with the invocation's capabilities.
 * False conditions skip the breakpoint; invalid conditions pause with diagnostics.
 * Matching conditions run in order until one is true, empty, or fails. */
MPARSER_C_API mparser_api_status mparser_debugger_set_conditional_breakpoints(
    mparser_debugger* debugger, const mparser_conditional_breakpoint* points, size_t count);
MPARSER_C_API mparser_api_status mparser_debugger_request_pause(
    mparser_debugger* debugger);
MPARSER_C_API mparser_debug_reason mparser_debug_event_reason(
    const mparser_debug_event* event);
MPARSER_C_API uint64_t mparser_debug_event_sequence(
    const mparser_debug_event* event);
MPARSER_C_API size_t mparser_debug_event_frame_count(
    const mparser_debug_event* event);
MPARSER_C_API size_t mparser_debug_event_condition_diagnostic_count(
    const mparser_debug_event* event);
/* Borrowed until the callback returns, like other event views. */
MPARSER_C_API const mparser_diagnostic* mparser_debug_event_condition_diagnostic(
    const mparser_debug_event* event, size_t index);
/* Frames are ordered outermost to current. Frame info/views are borrowed. */
MPARSER_C_API mparser_api_status mparser_debug_event_frame(
    const mparser_debug_event* event, size_t frame_index,
    mparser_debug_frame_info* out_info);
/* The name is borrowed; the returned value is an owned regular SDK value.
 * Mutable objects/handles retain the usual module/runtime ownership rules. */
MPARSER_C_API mparser_api_status mparser_debug_event_variable(
    const mparser_debug_event* event, size_t frame_index, size_t variable_index,
    mparser_utf8_view* out_name, mparser_value** out_value);
/* Call only on the execution thread while this borrowed event is active.
 * Evaluates source in the selected live frame, including assignment side effects.
 * Frame snapshots remain unchanged. Nested evaluation is rejected. The owned
 * result uses ordinary output/diagnostic accessors and may outlive the callback.
 * Script commands use requested_output_count=0; expressions may request outputs.
 * The invocation must grant MPARSER_SYSTEM_CAPABILITY_DYNAMIC_EVALUATION.
 * Evaluation failures are reported in the result, not as an API status error. */
/* Select the pause's current navigation frame instead of an explicit index. */
#define MPARSER_DEBUG_SELECTED_FRAME ((size_t)-1)
MPARSER_C_API mparser_api_status mparser_debug_event_evaluate(
    const mparser_debug_event* event, size_t frame_index,
    const char* source, size_t source_size, size_t requested_output_count,
    mparser_result** out_result);

MPARSER_C_API mparser_api_status
mparser_invocation_options_init(mparser_invocation_options* options);
MPARSER_C_API mparser_api_status mparser_invocation_options_init_sized(
    void* storage,
    size_t storage_size,
    uint32_t abi_generation);
MPARSER_C_API mparser_api_status
mparser_execution_summary_init(mparser_execution_summary* summary);
MPARSER_C_API mparser_api_status mparser_execution_summary_init_sized(
    void* storage,
    size_t storage_size,
    uint32_t abi_generation);
MPARSER_C_API mparser_api_status
mparser_source_unit_init(mparser_source_unit* source);
MPARSER_C_API mparser_api_status mparser_source_load_options_init(
    mparser_source_load_options* options);
MPARSER_C_API mparser_api_status mparser_source_load_options_init_sized(
    void* storage,
    size_t storage_size,
    uint32_t abi_generation);
MPARSER_C_API mparser_api_status
mparser_system_context_options_init(
    mparser_system_context_options* options);
MPARSER_C_API mparser_api_status
mparser_system_context_options_init_sized(
    void* storage,
    size_t storage_size,
    uint32_t abi_generation);

MPARSER_C_API mparser_api_status
mparser_system_context_create_rooted_native(
    const mparser_system_context_options* options,
    mparser_system_context** out_context);
MPARSER_C_API void
mparser_system_context_retain(mparser_system_context* context);
MPARSER_C_API void
mparser_system_context_release(mparser_system_context* context);
MPARSER_C_API mparser_system_capability
mparser_system_context_capabilities(
    const mparser_system_context* context);

/* Revision 8: visits a detached ordered path snapshot without executing source.
 * Views are borrowed only for each callback. Callbacks run without context locks
 * and return nonzero to continue; zero aborts with SYSTEM_CONTEXT_FAILED.
 * SearchPaths capability is required. No callbacks occur on a snapshot error.
 */
typedef uint32_t (*mparser_search_path_visitor)(
    void* user_data, size_t index, mparser_utf8_view path);
MPARSER_C_API mparser_api_status mparser_system_context_visit_search_paths(
    const mparser_system_context* context,
    mparser_search_path_visitor visitor,
    void* user_data);
/* Replaces the entire ordered list after validating every directory. Relative
 * paths resolve against one captured context cwd; rooted constraints apply.
 * First occurrence wins for duplicates. Empty list clears paths; empty entries,
 * NUL and malformed UTF-8 are invalid arguments. Failures preserve the list.
 */
MPARSER_C_API mparser_api_status mparser_system_context_set_search_paths(
    mparser_system_context* context,
    const mparser_utf8_view* paths,
    size_t path_count);

MPARSER_C_API mparser_api_status mparser_module_compile_utf8(
    const char* source,
    size_t source_size,
    const char* source_name,
    size_t source_name_size,
    mparser_module** out_module);
MPARSER_C_API mparser_api_status mparser_module_compile_sources(
    const mparser_source_unit* sources,
    size_t source_count,
    mparser_module** out_module);
MPARSER_C_API mparser_api_status mparser_module_compile_utf8_with_options(
    const char* source,
    size_t source_size,
    const char* source_name,
    size_t source_name_size,
    const mparser_source_load_options* options,
    mparser_module** out_module);
MPARSER_C_API mparser_api_status mparser_module_load_file_utf8(
    const char* entry_path,
    size_t entry_path_size,
    const mparser_source_load_options* options,
    mparser_module** out_module);
MPARSER_C_API void mparser_module_retain(mparser_module* module);
MPARSER_C_API void mparser_module_release(mparser_module* module);
MPARSER_C_API uint32_t
mparser_module_is_valid(const mparser_module* module);
MPARSER_C_API size_t
mparser_module_source_count(const mparser_module* module);
MPARSER_C_API mparser_utf8_view
mparser_module_source_name(const mparser_module* module, size_t index);
MPARSER_C_API mparser_source_kind
mparser_module_source_kind(const mparser_module* module, size_t index);
MPARSER_C_API mparser_utf8_view
mparser_module_source_primary_function(
    const mparser_module* module, size_t index);
MPARSER_C_API uint32_t
mparser_module_source_has_top_level_statements(
    const mparser_module* module, size_t index);
MPARSER_C_API uint32_t
mparser_module_source_is_pure_function_file(
    const mparser_module* module, size_t index);
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
MPARSER_C_API mparser_api_status
mparser_module_execute_with_system_context(
    const mparser_module* module,
    const mparser_system_context* context,
    const mparser_invocation_options* options,
    mparser_result** out_result);
MPARSER_C_API mparser_api_status mparser_module_create_session(
    const mparser_module* module,
    mparser_session** out_session);
MPARSER_C_API mparser_api_status
mparser_module_create_session_with_system_context(
    const mparser_module* module,
    const mparser_system_context* context,
    mparser_session** out_session);

MPARSER_C_API mparser_api_status mparser_runtime_create(
    const mparser_system_context* context,
    mparser_runtime** out_runtime);
MPARSER_C_API void mparser_runtime_retain(mparser_runtime* runtime);
MPARSER_C_API void mparser_runtime_release(mparser_runtime* runtime);
MPARSER_C_API mparser_api_status mparser_runtime_execute(
    mparser_runtime* runtime,
    const mparser_module* module,
    const mparser_invocation_options* options,
    mparser_result** out_result);
MPARSER_C_API mparser_api_status mparser_runtime_clear_global(
    mparser_runtime* runtime,
    const char* name,
    size_t name_size);
MPARSER_C_API mparser_api_status
mparser_runtime_clear_globals(mparser_runtime* runtime);
MPARSER_C_API mparser_api_status
mparser_runtime_reset(mparser_runtime* runtime);

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
/* Immutable session graph at execution completion, including no-output plots.
 * Borrowed until result release. An empty view means execution did not produce
 * a snapshot (for example, compilation/validation failure or export failure).
 * A successful empty graph is a nonempty JSON record with an empty objects array.
 */
MPARSER_C_API mparser_utf8_view
mparser_result_graphics_json(const mparser_result* result);
/* ABI 2.7: select legacy (0) or identity-bearing (3) immutable snapshot.
 * Other versions and null arguments return INVALID_ARGUMENT. Output is cleared
 * on failure. A missing snapshot returns OK with an empty view; ownership and
 * lifetime are the same as mparser_result_graphics_json. */
MPARSER_C_API mparser_api_status mparser_result_graphics_json_versioned(
    const mparser_result* result, uint32_t version, mparser_utf8_view* out_json);
MPARSER_C_API mparser_utf8_view
mparser_result_output_name(const mparser_result* result, size_t index);
MPARSER_C_API mparser_api_status mparser_result_output(
    const mparser_result* result,
    size_t index,
    mparser_value** out_value);
MPARSER_C_API size_t
mparser_result_output_event_count(const mparser_result* result);
MPARSER_C_API mparser_output_kind
mparser_result_output_event_kind(
    const mparser_result* result, size_t index);
MPARSER_C_API uint64_t
mparser_result_output_event_sequence(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_utf8_view
mparser_result_output_event_text(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_utf8_view
mparser_result_output_event_source_name(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_source_position
mparser_result_output_event_source_begin(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_source_position
mparser_result_output_event_source_end(
    const mparser_result* result, size_t index);
MPARSER_C_API size_t
mparser_result_top_level_expression_count(
    const mparser_result* result);
MPARSER_C_API mparser_api_status
mparser_result_top_level_expression_value(
    const mparser_result* result,
    size_t index,
    mparser_value** out_value);
MPARSER_C_API uint32_t
mparser_result_top_level_expression_output_suppressed(
    const mparser_result* result, size_t index);
MPARSER_C_API uint64_t
mparser_result_top_level_expression_sequence(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_utf8_view
mparser_result_top_level_expression_source_name(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_source_position
mparser_result_top_level_expression_source_begin(
    const mparser_result* result, size_t index);
MPARSER_C_API mparser_source_position
mparser_result_top_level_expression_source_end(
    const mparser_result* result, size_t index);
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
MPARSER_C_API mparser_api_status mparser_value_create_missing_array(
    const size_t* dimensions,
    size_t rank,
    mparser_value** out_value);
MPARSER_C_API mparser_api_status mparser_value_create_numeric(
    const size_t* dimensions,
    size_t rank,
    const mparser_numeric_buffer* buffer,
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
MPARSER_C_API mparser_api_status mparser_value_get_numeric_buffer(
    const mparser_value* value,
    mparser_numeric_buffer* out_buffer);
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

/* Returns an owned character-array JSON snapshot for graphics handles or arrays.
 * JSON is ASCII with Unicode escapes. The snapshot and handle remain readable
 * after their originating result/module/runtime is released. Deleted handles
 * serialize with valid=false and a null reference. Non-graphics values return
 * TYPE_MISMATCH. On failure *out_json is null. Release with mparser_value_release.
 * Arrays use dimensions, graphs, and logical-column-major elements carrying
 * graph/valid/reference. Graphs are deduplicated by identity within the record.
 * Graph mutation and serialization are ordered by the graphs' own locks. */
MPARSER_C_API mparser_api_status mparser_value_graphics_json(
    const mparser_value* value, mparser_value** out_json);

/* ABI 2.7: version 0 retains legacy automatic v1/v2 selection; version 3
 * includes graphIdentity/objectIdentity decimal strings, scoped to this loaded
 * kernel instance. Other versions return INVALID_ARGUMENT with null output.
 * Ownership, locking, scalar/array and deleted-handle rules match the function
 * above. Identities are not authorization tokens and expire on kernel reload. */
MPARSER_C_API mparser_api_status mparser_value_graphics_json_versioned(
    const mparser_value* value, uint32_t version, mparser_value** out_json);

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
