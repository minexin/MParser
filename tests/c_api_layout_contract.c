#include "mparser/c_api.h"

#include <stddef.h>
#include <stdio.h>

#define REQUIRE_OFFSET(type, field, expected) \
    _Static_assert(offsetof(type, field) == (expected), \
                   #type "." #field " offset changed")

_Static_assert(sizeof(void*) == 8,
               "MParser v1 release platforms require 64-bit pointers");
_Static_assert(sizeof(size_t) == 8,
               "MParser v1 release platforms require 64-bit size_t");

_Static_assert(MPARSER_C_ABI_VERSION_MAJOR == 2u,
               "C ABI major changed");
_Static_assert(MPARSER_C_ABI_REVISION == 0u,
               "C ABI revision changed");
_Static_assert(MPARSER_API_STATUS_ALLOCATION_FAILED == 6u,
               "allocation status changed");
_Static_assert(MPARSER_API_STATUS_SOURCE_LOAD_FAILED == 9u,
               "status range changed");
_Static_assert(MPARSER_INVOCATION_RUNTIME_FAILED == 3u,
               "invocation status range changed");
_Static_assert(MPARSER_BACKEND_NATIVE == 3u,
               "backend range changed");
_Static_assert(MPARSER_EXECUTION_TIER_MIXED == 3u,
               "execution tier range changed");
_Static_assert(MPARSER_STOP_DIAGNOSTIC_LIMIT == 6u,
               "stop reason range changed");
_Static_assert(MPARSER_VALUE_FUNCTION_HANDLE == 7u,
               "value kind range changed");
_Static_assert(MPARSER_NUMERIC_UINT64 == 10u,
               "numeric class range changed");

_Static_assert(sizeof(mparser_numeric_buffer) == 32,
               "mparser_numeric_buffer size changed");
REQUIRE_OFFSET(mparser_numeric_buffer, numeric_class, 0);
REQUIRE_OFFSET(mparser_numeric_buffer, is_complex, 4);
REQUIRE_OFFSET(mparser_numeric_buffer, real_data, 8);
REQUIRE_OFFSET(mparser_numeric_buffer, imaginary_data, 16);
REQUIRE_OFFSET(mparser_numeric_buffer, element_count, 24);

_Static_assert(sizeof(mparser_utf8_view) == 16,
               "mparser_utf8_view size changed");
REQUIRE_OFFSET(mparser_utf8_view, data, 0);
REQUIRE_OFFSET(mparser_utf8_view, size, 8);

_Static_assert(sizeof(mparser_utf16_view) == 24,
               "mparser_utf16_view size changed");
REQUIRE_OFFSET(mparser_utf16_view, data, 0);
REQUIRE_OFFSET(mparser_utf16_view, size, 8);
REQUIRE_OFFSET(mparser_utf16_view, missing, 16);

_Static_assert(sizeof(mparser_source_unit) == 40,
               "mparser_source_unit size changed");
REQUIRE_OFFSET(mparser_source_unit, struct_size, 0);
REQUIRE_OFFSET(mparser_source_unit, abi_version, 4);
REQUIRE_OFFSET(mparser_source_unit, source_name, 8);
REQUIRE_OFFSET(mparser_source_unit, source_name_size, 16);
REQUIRE_OFFSET(mparser_source_unit, source, 24);
REQUIRE_OFFSET(mparser_source_unit, source_size, 32);

_Static_assert(sizeof(mparser_source_load_options) == 24,
               "mparser_source_load_options size changed");
REQUIRE_OFFSET(mparser_source_load_options, struct_size, 0);
REQUIRE_OFFSET(mparser_source_load_options, abi_version, 4);
REQUIRE_OFFSET(mparser_source_load_options, search_paths, 8);
REQUIRE_OFFSET(mparser_source_load_options, search_path_count, 16);

_Static_assert(sizeof(mparser_named_value) == 24,
               "mparser_named_value size changed");
REQUIRE_OFFSET(mparser_named_value, name, 0);
REQUIRE_OFFSET(mparser_named_value, name_size, 8);
REQUIRE_OFFSET(mparser_named_value, value, 16);

_Static_assert(sizeof(mparser_source_position) == 16,
               "mparser_source_position size changed");
REQUIRE_OFFSET(mparser_source_position, offset, 0);
REQUIRE_OFFSET(mparser_source_position, line, 8);
REQUIRE_OFFSET(mparser_source_position, column, 12);

_Static_assert(sizeof(mparser_invocation_options) == 128,
               "mparser_invocation_options size changed");
REQUIRE_OFFSET(mparser_invocation_options, struct_size, 0);
REQUIRE_OFFSET(mparser_invocation_options, abi_version, 4);
REQUIRE_OFFSET(mparser_invocation_options, entry_name, 8);
REQUIRE_OFFSET(mparser_invocation_options, entry_name_size, 16);
REQUIRE_OFFSET(mparser_invocation_options, arguments, 24);
REQUIRE_OFFSET(mparser_invocation_options, argument_count, 32);
REQUIRE_OFFSET(mparser_invocation_options, requested_output_count, 40);
REQUIRE_OFFSET(mparser_invocation_options, has_requested_output_count, 48);
REQUIRE_OFFSET(mparser_invocation_options, initial_workspace, 56);
REQUIRE_OFFSET(mparser_invocation_options, initial_workspace_count, 64);
REQUIRE_OFFSET(mparser_invocation_options, backend, 72);
REQUIRE_OFFSET(mparser_invocation_options, collect_profile, 76);
REQUIRE_OFFSET(mparser_invocation_options, max_instruction_count, 80);
REQUIRE_OFFSET(mparser_invocation_options, max_wall_time_nanoseconds, 88);
REQUIRE_OFFSET(mparser_invocation_options, max_call_depth, 96);
REQUIRE_OFFSET(mparser_invocation_options, max_array_bytes, 104);
REQUIRE_OFFSET(mparser_invocation_options, max_diagnostic_count, 112);
REQUIRE_OFFSET(mparser_invocation_options, cancellation_token, 120);

_Static_assert(sizeof(mparser_execution_summary) == 128,
               "mparser_execution_summary size changed");
REQUIRE_OFFSET(mparser_execution_summary, struct_size, 0);
REQUIRE_OFFSET(mparser_execution_summary, abi_version, 4);
REQUIRE_OFFSET(mparser_execution_summary, requested_backend, 8);
REQUIRE_OFFSET(mparser_execution_summary, effective_tier, 12);
REQUIRE_OFFSET(mparser_execution_summary, profiling_collected, 16);
REQUIRE_OFFSET(mparser_execution_summary, fallback_occurred, 20);
REQUIRE_OFFSET(mparser_execution_summary, resource_controls_active, 24);
REQUIRE_OFFSET(
    mparser_execution_summary, optimized_execution_suppressed, 28);
REQUIRE_OFFSET(mparser_execution_summary, stop_reason, 32);
REQUIRE_OFFSET(mparser_execution_summary, executed_instruction_count, 40);
REQUIRE_OFFSET(mparser_execution_summary, typed_region_count, 48);
REQUIRE_OFFSET(mparser_execution_summary, typed_region_attempt_count, 56);
REQUIRE_OFFSET(mparser_execution_summary, typed_region_execution_count, 64);
REQUIRE_OFFSET(mparser_execution_summary, typed_region_fallback_count, 72);
REQUIRE_OFFSET(mparser_execution_summary, native_compilation_count, 80);
REQUIRE_OFFSET(mparser_execution_summary, native_cache_hit_count, 88);
REQUIRE_OFFSET(mparser_execution_summary, maximum_call_depth, 96);
REQUIRE_OFFSET(mparser_execution_summary, maximum_array_bytes, 104);
REQUIRE_OFFSET(mparser_execution_summary, maximum_diagnostic_count, 112);
REQUIRE_OFFSET(mparser_execution_summary, elapsed_nanoseconds, 120);

int main(void) {
    puts("c api layout contract = abi-2.0,64-bit");
    return 0;
}
