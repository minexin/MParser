#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

typedef struct debug_state {
    size_t calls;
    int failed;
    mparser_value* retained_local;
    mparser_result* evaluation;
    int condition_error;
} debug_state;

static int view_equals(mparser_utf8_view view, const char* expected) {
    const size_t length = strlen(expected);
    return view.size == length && view.data &&
           memcmp(view.data, expected, length) == 0;
}

#define PAUSE_CHECK(condition) do { \
    if (!(condition)) { state->failed = __LINE__; \
        fprintf(stderr, "debug pause failure at %d: %s\n", __LINE__, #condition); \
        return MPARSER_DEBUG_STOP; } \
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
    {
        static const char* commands[] = {
            "assert(sum(local)==7); dbup;",
            "assert(x==3); x=12;",
            "assert(x==12); dbdown;",
            "assert(sum(local)==7);"
        };
        size_t command;
        for (command = 0; command < sizeof(commands)/sizeof(commands[0]); ++command) {
            mparser_result* navigation = NULL;
            PAUSE_CHECK(mparser_debug_event_evaluate(event, MPARSER_DEBUG_SELECTED_FRAME,
                commands[command], strlen(commands[command]), 0, &navigation) == MPARSER_API_STATUS_OK);
            if (!mparser_result_succeeded(navigation)) {
                mparser_result_release(navigation);
                PAUSE_CHECK(0);
            }
            mparser_result_release(navigation);
        }
    }
    {
        static const char expression[] = "sum(local)";
        static const char assignment[] = "local=[10 20];";
        static const char invalid[] = "missing_debug_name";
        static const char display[] = "disp(local);";
        mparser_value* value = NULL;
        mparser_numeric_buffer buffer;
        PAUSE_CHECK(mparser_debug_event_evaluate(event, 2, expression,
            sizeof(expression)-1, 1, &state->evaluation) == MPARSER_API_STATUS_OUT_OF_RANGE);
        PAUSE_CHECK(state->evaluation == NULL);
        PAUSE_CHECK(mparser_debug_event_evaluate(event, 1, invalid,
            sizeof(invalid)-1, 1, &state->evaluation) == MPARSER_API_STATUS_OK);
        PAUSE_CHECK(!mparser_result_succeeded(state->evaluation) &&
            mparser_result_diagnostic_count(state->evaluation) != 0);
        mparser_result_release(state->evaluation);
        state->evaluation = NULL;
        PAUSE_CHECK(mparser_debug_event_evaluate(event, 1, expression,
            sizeof(expression)-1, 1, &state->evaluation) == MPARSER_API_STATUS_OK);
        PAUSE_CHECK(mparser_result_succeeded(state->evaluation));
        PAUSE_CHECK(mparser_result_output(state->evaluation, 0, &value) == MPARSER_API_STATUS_OK);
        PAUSE_CHECK(mparser_value_get_numeric_buffer(value, &buffer) == MPARSER_API_STATUS_OK &&
            buffer.element_count == 1 && ((const double*)buffer.real_data)[0] == 7);
        mparser_value_release(value);
        mparser_result_release(state->evaluation);
        state->evaluation = NULL;
        PAUSE_CHECK(mparser_debug_event_evaluate(event, 1, assignment,
            sizeof(assignment)-1, 0, &state->evaluation) == MPARSER_API_STATUS_OK);
        PAUSE_CHECK(mparser_result_succeeded(state->evaluation));
        mparser_result_release(state->evaluation);
        state->evaluation = NULL;
        PAUSE_CHECK(mparser_debug_event_evaluate(event, 1, display,
            sizeof(display)-1, 0, &state->evaluation) == MPARSER_API_STATUS_OK);
        PAUSE_CHECK(mparser_result_succeeded(state->evaluation) &&
            mparser_result_output_event_count(state->evaluation) != 0);
    }
    return MPARSER_DEBUG_CONTINUE;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "debugger C API failure at %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; } } while (0)

static mparser_debug_action on_condition(void* user_data,
                                        const mparser_debug_event* event) {
    debug_state* state = (debug_state*)user_data;
    const size_t count = mparser_debug_event_condition_diagnostic_count(event);
    ++state->calls;
    PAUSE_CHECK((count != 0) == state->condition_error);
    PAUSE_CHECK(mparser_debug_event_condition_diagnostic(event, count) == NULL);
    if (count) {
        const mparser_diagnostic* diagnostic = mparser_debug_event_condition_diagnostic(event, 0);
        PAUSE_CHECK(diagnostic != NULL);
        PAUSE_CHECK(mparser_diagnostic_identifier(diagnostic).size != 0);
        PAUSE_CHECK(mparser_diagnostic_message(diagnostic).size != 0);
    }
    return MPARSER_DEBUG_CONTINUE;
}

int main(void) {
    static const char source[] =
        "x=3;\ny=inner(x);\nz=y+1;\nfunction out=inner(in)\n"
        "local=[in in+1];\nout=sum(local);\nend\n";
    mparser_module* module = NULL;
    mparser_result* result = NULL;
    mparser_debugger* debugger = NULL;
    mparser_system_context* context = NULL;
    mparser_system_context_options system_options;
    mparser_invocation_options options;
    mparser_numeric_buffer buffer;
    mparser_breakpoint point = {{"debug.m", 7}, 6};
    mparser_breakpoint invalid = {{"debug.m", 7}, 0};
    debug_state state = {0, 0, NULL, NULL, 0};
    int failed = 0;
    CHECK(mparser_debugger_create(NULL, NULL, &debugger) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT && debugger == NULL);
    CHECK(mparser_debugger_request_pause(NULL) == MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_debugger_set_breakpoints(NULL, NULL, 0) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_debug_event_frame_count(NULL) == 0);
    CHECK(mparser_debug_event_evaluate(NULL, 0, NULL, 0, 0, &result) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT && result == NULL);
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
    CHECK(MPARSER_SYSTEM_CONTEXT_OPTIONS_INIT(&system_options) == MPARSER_API_STATUS_OK);
    system_options.capabilities = MPARSER_SYSTEM_CAPABILITY_DYNAMIC_EVALUATION;
    system_options.root_directory.data = ".";
    system_options.root_directory.size = 1;
    CHECK(mparser_system_context_create_rooted_native(&system_options, &context) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_module_execute_with_system_context(module, context, &options, &result) ==
          MPARSER_API_STATUS_OK);
    CHECK(mparser_result_succeeded(result) && state.failed == 0 && state.calls == 1);
    {
        size_t index;
        int found = 0;
        int caller_changed = 0;
        for (index = 0; index < mparser_result_variable_count(result); ++index) {
            mparser_utf8_view name;
            mparser_value* value = NULL;
            CHECK(mparser_result_variable(result, index, &name, &value) == MPARSER_API_STATUS_OK);
            if (view_equals(name, "z")) {
                CHECK(mparser_value_get_numeric_buffer(value, &buffer) == MPARSER_API_STATUS_OK);
                found = buffer.element_count == 1 && ((const double*)buffer.real_data)[0] == 31;
            }
            if (view_equals(name, "x")) {
                CHECK(mparser_value_get_numeric_buffer(value, &buffer) == MPARSER_API_STATUS_OK);
                caller_changed = buffer.element_count == 1 && ((const double*)buffer.real_data)[0] == 12;
            }
            mparser_value_release(value);
        }
        CHECK(found && caller_changed);
    }
    CHECK(mparser_result_succeeded(state.evaluation) &&
          mparser_result_output_event_text(state.evaluation, 0).size != 0);
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
    mparser_result_release(result);
    result = NULL;
    mparser_debugger_release(debugger);
    debugger = NULL;
    CHECK(mparser_debugger_create(on_condition, &state, &debugger) == MPARSER_API_STATUS_OK);
    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) == MPARSER_API_STATUS_OK);
    options.debugger = debugger;
    CHECK(mparser_debug_event_condition_diagnostic_count(NULL) == 0 &&
          mparser_debug_event_condition_diagnostic(NULL, 0) == NULL);
    {
        int mode;
        for (mode = 0; mode < 3; ++mode) {
            const char* expression = mode == 0 ? "sum(local)==7" :
                mode == 1 ? "sum(local)==8" : "unknown_condition";
            mparser_conditional_breakpoint condition = {{"debug.m", 7}, 6, {NULL, 0}};
            condition.condition.data = expression;
            condition.condition.size = strlen(expression);
            state.calls = 0;
            state.condition_error = mode == 2;
            CHECK(mparser_debugger_set_conditional_breakpoints(debugger, &condition, 1) ==
                  MPARSER_API_STATUS_OK);
            condition.condition.data = NULL;
            CHECK(mparser_debugger_set_conditional_breakpoints(debugger, &condition, 1) ==
                  MPARSER_API_STATUS_INVALID_ARGUMENT);
            CHECK(mparser_module_execute_with_system_context(module, context, &options, &result) ==
                  MPARSER_API_STATUS_OK);
            CHECK(mparser_result_succeeded(result) && !state.failed &&
                  state.calls == (mode == 1 ? 0u : 1u));
            mparser_result_release(result);
            result = NULL;
        }
    }
    puts("debugger C API = frames,locals,evaluation,conditions,assignment,recovery,retained,invalid-input,sized-tail");
cleanup:
    mparser_result_release(state.evaluation);
    mparser_system_context_release(context);
    mparser_result_release(result);
    mparser_module_release(module);
    mparser_debugger_release(debugger);
    mparser_value_release(state.retained_local);
    return failed;
}
