#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

typedef struct debug_state {
    size_t calls;
    int failed;
    mparser_value* retained_local;
} debug_state;

static int view_equals(mparser_utf8_view view, const char* expected) {
    const size_t length = strlen(expected);
    return view.size == length && view.data &&
           memcmp(view.data, expected, length) == 0;
}

#define PAUSE_CHECK(condition) do { \
    if (!(condition)) { state->failed = __LINE__; return MPARSER_DEBUG_STOP; } \
} while (0)

static mparser_debug_action on_pause(void* user_data,
                                    const mparser_debug_event* event) {
    debug_state* state = (debug_state*)user_data;
    mparser_debug_frame_info frame;
    size_t index;
    ++state->calls;
    PAUSE_CHECK(mparser_debug_event_reason(event) == MPARSER_DEBUG_BREAKPOINT);
    PAUSE_CHECK(mparser_debug_event_frame_count(event) == 2);
    PAUSE_CHECK(mparser_debug_event_sequence(event) == state->calls - 1);
    PAUSE_CHECK(mparser_debug_event_frame(event, 0, &frame) == MPARSER_API_STATUS_OK);
    PAUSE_CHECK(frame.kind == MPARSER_DEBUG_FRAME_SCRIPT && frame.source_begin.line == 2);
    PAUSE_CHECK(mparser_debug_event_frame(event, 1, &frame) == MPARSER_API_STATUS_OK);
    PAUSE_CHECK(frame.kind == MPARSER_DEBUG_FRAME_FUNCTION &&
                frame.source_begin.line == 6 &&
                frame.supplied_argument_count == 1 && frame.requested_output_count == 1);
    PAUSE_CHECK(view_equals(frame.function_name, "inner") &&
                view_equals(frame.source_name, "debug.m"));
    for (index = 0; index < frame.variable_count; ++index) {
        mparser_utf8_view name;
        mparser_value* value = NULL;
        PAUSE_CHECK(mparser_debug_event_variable(event, 1, index, &name, &value) ==
                    MPARSER_API_STATUS_OK);
        if (view_equals(name, "local")) {
            mparser_value_release(state->retained_local);
            state->retained_local = value;
        } else {
            mparser_value_release(value);
        }
    }
    PAUSE_CHECK(state->retained_local != NULL);
    PAUSE_CHECK(mparser_debug_event_frame(event, 2, &frame) ==
                MPARSER_API_STATUS_OUT_OF_RANGE);
    PAUSE_CHECK(frame.variable_count == 0);
    return MPARSER_DEBUG_CONTINUE;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "debugger C API failure at %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; } } while (0)

int main(void) {
    static const char source[] =
        "x=3;\ny=inner(x);\nz=y+1;\nfunction out=inner(in)\n"
        "local=[in in+1];\nout=sum(local);\nend\n";
    mparser_module* module = NULL;
    mparser_result* result = NULL;
    mparser_debugger* debugger = NULL;
    mparser_invocation_options options;
    mparser_numeric_buffer buffer;
    mparser_breakpoint point = {{"debug.m", 7}, 6};
    mparser_breakpoint invalid = {{"debug.m", 7}, 0};
    debug_state state = {0, 0, NULL};
    int failed = 0;
    CHECK(mparser_debugger_create(NULL, NULL, &debugger) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT && debugger == NULL);
    CHECK(mparser_debugger_request_pause(NULL) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_debugger_set_breakpoints(NULL, NULL, 0) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_debug_event_frame_count(NULL) == 0);
    CHECK(mparser_module_compile_utf8(source, sizeof(source)-1, "debug.m", 7, &module) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_debugger_create(on_pause, &state, &debugger) == MPARSER_API_STATUS_OK);
    mparser_debugger_retain(debugger);
    mparser_debugger_release(debugger);
    CHECK(mparser_debugger_set_breakpoints(debugger, &point, 1) == MPARSER_API_STATUS_OK);
    CHECK(mparser_debugger_set_breakpoints(debugger, &invalid, 1) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) == MPARSER_API_STATUS_OK);
    CHECK(options.struct_size == sizeof(options) && options.debugger == NULL);
    options.debugger = debugger;
    CHECK(mparser_module_execute(module, &options, &result) == MPARSER_API_STATUS_OK);
    CHECK(mparser_result_succeeded(result) && state.failed == 0 && state.calls == 1);
    mparser_result_release(result);
    result = NULL;
    CHECK(mparser_value_get_numeric_buffer(state.retained_local, &buffer) ==
          MPARSER_API_STATUS_OK && buffer.element_count == 2);
    CHECK(((const double*)buffer.real_data)[0] == 3 &&
          ((const double*)buffer.real_data)[1] == 4);

    /* A shorter negotiated root must never read the debugger tail. */
    options.struct_size = MPARSER_INVOCATION_OPTIONS_SIZE;
    CHECK(mparser_module_execute(module, &options, &result) == MPARSER_API_STATUS_OK);
    CHECK(mparser_result_succeeded(result) && state.calls == 1);
    puts("debugger C API = frames,locals,retained,invalid-input,sized-tail");
cleanup:
    mparser_result_release(result);
    mparser_module_release(module);
    mparser_debugger_release(debugger);
    mparser_value_release(state.retained_local);
    return failed;
}
