#include "mparser/c_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct input_state {
    size_t calls;
    int mode;
    int failed;
    mparser_cancel_token* token;
} input_state;

static input_state null_context;

static mparser_input_status on_input(void* user_data, mparser_input_mode mode,
                                    mparser_utf8_view prompt,
                                    mparser_utf8_view* text,
                                    mparser_utf8_view* error) {
    input_state* state = user_data ? (input_state*)user_data : &null_context;
    ++state->calls;
    if (mode != MPARSER_INPUT_TEXT || prompt.size != 2 ||
        memcmp(prompt.data, "p:", 2) != 0) {
        state->failed = 1;
    }
    switch (state->mode) {
    case 1:
        text->data = NULL;
        text->size = 1;
        return MPARSER_INPUT_READY;
    case 2:
        return (mparser_input_status)999;
    case 3:
        error->data = "host failed";
        error->size = 11;
        return MPARSER_INPUT_ERROR;
    case 4:
        return MPARSER_INPUT_END;
    case 5:
        text->data = "\xff";
        text->size = 1;
        return MPARSER_INPUT_READY;
    case 6:
        mparser_cancel_token_request(state->token);
        return MPARSER_INPUT_PENDING;
    case 7:
        return MPARSER_INPUT_PENDING;
    default:
        /* Unused views are ignored, including during Pending polling. */
        error->data = NULL;
        error->size = 1;
        if (state->calls == 1) {
            text->data = NULL;
            text->size = 1;
            return MPARSER_INPUT_PENDING;
        }
        text->data = "  hi  ";
        text->size = 6;
        return MPARSER_INPUT_READY;
    }
}

static int diagnostic_contains(const mparser_result* result, const char* text) {
    size_t index;
    const size_t length = strlen(text);
    for (index = 0; index < mparser_result_diagnostic_count(result); ++index) {
        const mparser_utf8_view message = mparser_diagnostic_message(
            mparser_result_diagnostic(result, index));
        size_t offset;
        for (offset = 0; offset + length <= message.size; ++offset) {
            if (memcmp(message.data + offset, text, length) == 0) return 1;
        }
    }
    return 0;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "input C API failure at %d: %s\n", __LINE__, #condition); \
    failed = 1; goto cleanup; } } while (0)

static int run_backend(mparser_backend backend) {
    static const char source[] =
        "s=input('p:','s'); assert(isequal(s,'  hi  '));\n";
    static const char* errors[] = {"invalid text view", "invalid status",
                                  "host failed", "end", "UTF-8"};
    mparser_module* module = NULL;
    mparser_result* result = NULL;
    mparser_invocation_options options;
    mparser_invocation_options* sized = NULL;
    input_state state = {0, 0, 0, NULL};
    size_t size;
    int test;
    int failed = 0;
    CHECK(mparser_module_compile_utf8(source, sizeof(source)-1,
          "input.m", 7, &module) == MPARSER_API_STATUS_OK);
    CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) == MPARSER_API_STATUS_OK);
    CHECK(options.input_source == NULL && options.input_user_data == NULL);
    options.backend = backend;
    options.input_source = on_input;
    options.input_user_data = &state;
    CHECK(mparser_module_execute(module, &options, &result) == MPARSER_API_STATUS_OK);
    CHECK(mparser_result_succeeded(result) && state.calls == 2 && !state.failed);
    mparser_result_release(result);
    result = NULL;

    /* Allocate only negotiated bytes so sanitizers detect reads past the tail. */
    for (test = 0; test < 3; ++test) {
        size = test == 0 ? MPARSER_INVOCATION_OPTIONS_SIZE :
            (test == 1 ? offsetof(mparser_invocation_options, input_source) :
                         offsetof(mparser_invocation_options, input_user_data));
        options.struct_size = (uint32_t)size;
        sized = (mparser_invocation_options*)malloc(size);
        CHECK(sized != NULL);
        memcpy(sized, &options, size);
        memset(&null_context, 0, sizeof(null_context));
        CHECK(mparser_module_execute(module, sized, &result) == MPARSER_API_STATUS_OK);
        CHECK(state.calls == 2);
        if (test == 2) {
            CHECK(mparser_result_succeeded(result));
            CHECK(null_context.calls == 2 && !null_context.failed);
        } else {
            CHECK(!mparser_result_succeeded(result));
            CHECK(mparser_result_diagnostic_count(result) > 0);
            CHECK(null_context.calls == 0);
        }
        free(sized);
        sized = NULL;
        mparser_result_release(result);
        result = NULL;
    }
    options.struct_size = sizeof(options);
    for (test = 1; test <= 5; ++test) {
        state.calls = 0;
        state.mode = test;
        CHECK(mparser_module_execute(module, &options, &result) == MPARSER_API_STATUS_OK);
        CHECK(!mparser_result_succeeded(result) && state.calls == 1 && !state.failed);
        CHECK(diagnostic_contains(result, errors[test-1]));
        mparser_result_release(result);
        result = NULL;
    }
    CHECK(mparser_cancel_token_create(&state.token) == MPARSER_API_STATUS_OK);
    for (test = 6; test <= 7; ++test) {
        mparser_execution_summary summary;
        state.calls = 0;
        state.mode = test;
        options.cancellation_token = test == 6 ? state.token : NULL;
        options.max_wall_time_nanoseconds = test == 7 ? 100000000 : 0;
        CHECK(mparser_module_execute(module, &options, &result) == MPARSER_API_STATUS_OK);
        CHECK(!mparser_result_succeeded(result) && !state.failed);
        CHECK(test != 6 || state.calls == 1);
        CHECK(MPARSER_EXECUTION_SUMMARY_INIT(&summary) == MPARSER_API_STATUS_OK);
        CHECK(mparser_result_execution_summary(result, &summary) == MPARSER_API_STATUS_OK);
        CHECK(summary.stop_reason == (test == 6 ? MPARSER_STOP_CANCELLED :
                                                 MPARSER_STOP_WALL_TIME_LIMIT));
        mparser_result_release(result);
        result = NULL;
    }
cleanup:
    free(sized);
    mparser_result_release(result);
    mparser_module_release(module);
    mparser_cancel_token_release(state.token);
    return failed;
}

int main(void) {
    if (run_backend(MPARSER_BACKEND_BYTECODE) ||
        run_backend(MPARSER_BACKEND_PORTABLE) ||
        run_backend(MPARSER_BACKEND_AUTOMATIC)) return 1;
    puts("input C API = sized-tail,context,pending,text,errors,cancel,timeout,backends");
    return 0;
}
