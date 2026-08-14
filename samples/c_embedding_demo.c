#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

static const char k_source[] =
    "function total = sumTo(limit)\n"
    "total = 0;\n"
    "for i = 1:limit\n"
    "    total = total + i;\n"
    "end\n"
    "end\n"
    "\n"
    "function out = spin()\n"
    "out = 0;\n"
    "while 1\n"
    "    out = out + 1;\n"
    "end\n"
    "end\n"
    "\n"
    "function out = identity(value)\n"
    "out = value;\n"
    "end\n";

static const char k_host_source[] =
    "formatted = sprintf(\"value=%d\", 42);\n"
    "disp(formatted)\n"
    "written = fprintf(\"pi=%.1f\\n\", 3.14);\n"
    "40 + 2\n"
    "41 + 2;\n";

typedef struct output_capture {
    size_t count;
} output_capture;

static int bytes_equal(
    const char* data, size_t size, const char* expected) {
    const size_t expected_size = strlen(expected);
    return size == expected_size &&
           (size == 0 || memcmp(data, expected, size) == 0);
}

static int view_ends_with(
    mparser_utf8_view view, const char* suffix) {
    const size_t suffix_size = strlen(suffix);
    return view.size >= suffix_size &&
           memcmp(view.data + view.size - suffix_size,
                  suffix, suffix_size) == 0;
}

static mparser_output_disposition print_output(
    void* user_data,
    uint64_t sequence,
    mparser_output_kind kind,
    const char* text,
    size_t text_size,
    const char* source_name,
    size_t source_name_size,
    mparser_source_position source_begin,
    mparser_source_position source_end) {
    output_capture* capture = (output_capture*)user_data;
    const mparser_utf8_view source = {
        source_name, source_name_size};
    const int expected =
        capture != NULL && sequence == capture->count &&
        view_ends_with(source, "c_host_integration_demo.m") &&
        source_begin.line > 0 && source_end.line > 0 &&
        ((capture->count == 0 &&
          kind == MPARSER_OUTPUT_DISPLAY &&
          bytes_equal(text, text_size, "value=42\n\n")) ||
         (capture->count == 1 &&
          kind == MPARSER_OUTPUT_STANDARD &&
          bytes_equal(text, text_size, "pi=3.1\n")));
    if (!expected) {
        return MPARSER_OUTPUT_REJECT;
    }
    ++capture->count;
    fwrite(text, 1, text_size, stdout);
    return MPARSER_OUTPUT_ACCEPT;
}

static int check_status(mparser_api_status status,
                        const char* operation) {
    if (status == MPARSER_API_STATUS_OK) {
        return 1;
    }
    {
        const mparser_utf8_view name =
            mparser_api_status_name(status);
        fprintf(stderr, "%s failed: %.*s\n", operation,
                (int)name.size, name.data);
    }
    return 0;
}

static mparser_invocation_options options_for(const char* entry) {
    mparser_invocation_options options;
    MPARSER_INVOCATION_OPTIONS_INIT(&options);
    options.entry_name = entry;
    options.entry_name_size = strlen(entry);
    options.has_requested_output_count = 1;
    options.requested_output_count = 1;
    return options;
}

static mparser_api_status create_double_scalar(
    double value, mparser_value** output) {
    const size_t dimensions[2] = {1, 1};
    const mparser_numeric_buffer buffer = {
        MPARSER_NUMERIC_DOUBLE, 0, &value, NULL, 1};
    return mparser_value_create_numeric(
        dimensions, 2, &buffer, output);
}

static int read_value_scalar(
    const mparser_value* input, double* value) {
    mparser_numeric_buffer buffer = {0};
    if (mparser_value_get_numeric_buffer(input, &buffer) !=
            MPARSER_API_STATUS_OK ||
        buffer.numeric_class != MPARSER_NUMERIC_DOUBLE ||
        buffer.is_complex != 0 ||
        buffer.element_count != 1) {
        return 0;
    }
    *value = ((const double*)buffer.real_data)[0];
    return 1;
}

static int read_scalar(const mparser_result* result, double* value) {
    mparser_value* output = NULL;
    int valid = 0;
    if (!mparser_result_succeeded(result) ||
        mparser_result_output(result, 0, &output) !=
            MPARSER_API_STATUS_OK) {
        return 0;
    }
    valid = read_value_scalar(output, value);
    mparser_value_release(output);
    return valid;
}

int main(void) {
    mparser_module* module = NULL;
    mparser_session* session = NULL;
    mparser_value* argument = NULL;
    const mparser_value* arguments[1];
    mparser_invocation_options options;
    mparser_execution_summary summary;
    mparser_result* result = NULL;
    mparser_module* host_module = NULL;
    mparser_result* host_result = NULL;
    mparser_value* host_expression = NULL;
    mparser_source_load_options load_options;
    output_capture capture = {0};
    double total = 0;
    double recovered = 0;
    double host_answer = 0;

    if (!check_status(
            mparser_module_compile_utf8(
                k_source, strlen(k_source),
                "c_embedding_demo.m",
                strlen("c_embedding_demo.m"), &module),
            "compile") ||
        !check_status(
            mparser_module_create_session(module, &session),
            "create session") ||
        !check_status(
            create_double_scalar(100.0, &argument),
            "create argument")) {
        return 1;
    }

    arguments[0] = argument;
    options = options_for("sumTo");
    options.arguments = arguments;
    options.argument_count = 1;
    if (!check_status(
            mparser_module_execute(module, &options, &result),
            "sumTo") ||
        !read_scalar(result, &total)) {
        return 1;
    }
    mparser_result_release(result);
    result = NULL;
    mparser_value_release(argument);
    argument = NULL;

    options = options_for("spin");
    options.max_instruction_count = 64;
    if (!check_status(
            mparser_session_execute(session, &options, &result),
            "limited spin") ||
        !check_status(
            MPARSER_EXECUTION_SUMMARY_INIT(&summary),
            "summary init") ||
        !check_status(
            mparser_result_execution_summary(result, &summary),
            "execution summary") ||
        summary.stop_reason != MPARSER_STOP_INSTRUCTION_LIMIT) {
        return 1;
    }
    mparser_result_release(result);
    result = NULL;

    if (!check_status(
            create_double_scalar(42.0, &argument),
            "create recovery argument")) {
        return 1;
    }
    arguments[0] = argument;
    options = options_for("identity");
    options.arguments = arguments;
    options.argument_count = 1;
    if (!check_status(
            mparser_session_execute(session, &options, &result),
            "session recovery") ||
        !read_scalar(result, &recovered)) {
        return 1;
    }
    mparser_result_release(result);
    result = NULL;

    if (!check_status(
            mparser_source_load_options_init(&load_options),
            "source load options") ||
        !check_status(
            mparser_module_compile_utf8_with_options(
                k_host_source, strlen(k_host_source),
                "c_host_integration_demo.m",
                strlen("c_host_integration_demo.m"),
                &load_options, &host_module),
            "compile host script") ||
        !mparser_module_is_valid(host_module) ||
        mparser_module_source_count(host_module) != 1 ||
        mparser_module_source_kind(host_module, 0) !=
            MPARSER_SOURCE_SCRIPT ||
        !mparser_module_source_has_top_level_statements(
            host_module, 0) ||
        mparser_module_source_is_pure_function_file(host_module, 0)) {
        return 1;
    }

    if (!check_status(
            mparser_invocation_options_init(&options),
            "host invocation options")) {
        return 1;
    }
    options.output_sink = print_output;
    options.output_user_data = &capture;
    if (!check_status(
            mparser_module_execute(
                host_module, &options, &host_result),
            "execute host script") ||
        !mparser_result_succeeded(host_result) ||
        capture.count != 2 ||
        mparser_result_output_event_count(host_result) != 2 ||
        mparser_result_top_level_expression_count(host_result) != 2 ||
        !check_status(
            mparser_result_top_level_expression_value(
                host_result, 1, &host_expression),
            "read host expression") ||
        !read_value_scalar(host_expression, &host_answer) ||
        !mparser_result_top_level_expression_output_suppressed(
            host_result, 1) ||
        mparser_result_top_level_expression_sequence(
            host_result, 1) != 3) {
        return 1;
    }

    printf("sumTo(100) = %.0f\n", total);
    printf("limited stop = instruction-limit\n");
    printf("recovered = %.0f\n", recovered);
    printf("host answer = %.0f\n", host_answer);
    printf("summary = 5050,instruction-limit,42,host-output-2-2\n");

    mparser_value_release(host_expression);
    mparser_result_release(host_result);
    mparser_module_release(host_module);
    mparser_value_release(argument);
    mparser_session_release(session);
    mparser_module_release(module);
    return 0;
}
