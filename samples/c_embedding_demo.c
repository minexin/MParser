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

static int read_scalar(const mparser_result* result, double* value) {
    mparser_value* output = NULL;
    mparser_numeric_buffer buffer = {0};
    if (!mparser_result_succeeded(result) ||
        mparser_result_output(result, 0, &output) !=
            MPARSER_API_STATUS_OK ||
        mparser_value_get_numeric_buffer(output, &buffer) !=
            MPARSER_API_STATUS_OK ||
        buffer.numeric_class != MPARSER_NUMERIC_DOUBLE ||
        buffer.is_complex != 0 ||
        buffer.element_count != 1) {
        mparser_value_release(output);
        return 0;
    }
    *value = ((const double*)buffer.real_data)[0];
    mparser_value_release(output);
    return 1;
}

int main(void) {
    mparser_module* module = NULL;
    mparser_session* session = NULL;
    mparser_value* argument = NULL;
    const mparser_value* arguments[1];
    mparser_invocation_options options;
    mparser_execution_summary summary;
    mparser_result* result = NULL;
    double total = 0;
    double recovered = 0;

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

    printf("sumTo(100) = %.0f\n", total);
    printf("limited stop = instruction-limit\n");
    printf("recovered = %.0f\n", recovered);
    printf("summary = 5050,instruction-limit,42\n");

    mparser_result_release(result);
    mparser_value_release(argument);
    mparser_session_release(session);
    mparser_module_release(module);
    return 0;
}
