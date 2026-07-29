#include "public_contract/c_abi/1.1/c_api_snapshot.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int read_scalar(const mparser_result* result, double expected) {
    mparser_value* output = NULL;
    const double* data = NULL;
    size_t count = 0;
    int matched = 0;
    if (mparser_result_succeeded(result) &&
        mparser_result_output_count(result) == 1 &&
        mparser_result_output(result, 0, &output) ==
            MPARSER_API_STATUS_OK &&
        mparser_value_numeric_data(output, &data, &count) ==
            MPARSER_API_STATUS_OK &&
        count == 1 && fabs(data[0] - expected) < 1e-9) {
        matched = 1;
    }
    mparser_value_release(output);
    return matched;
}

int main(void) {
    static const char source[] =
        "function out = entry(value)\n"
        "out = value + 2;\n"
        "end\n";
    mparser_module* module = NULL;
    mparser_value* argument = NULL;
    const mparser_value* arguments[1];
    mparser_invocation_options options;
    mparser_execution_summary summary;
    mparser_source_load_options load_options;
    mparser_result* result = NULL;
    int succeeded = 0;

    if (mparser_c_abi_version() != 1u ||
        mparser_c_abi_revision() != 1u ||
        mparser_invocation_options_init_sized(
            &options, sizeof(options), 1u) !=
            MPARSER_API_STATUS_OK ||
        mparser_execution_summary_init_sized(
            &summary, sizeof(summary), 1u) !=
            MPARSER_API_STATUS_OK ||
        mparser_source_load_options_init_sized(
            &load_options, sizeof(load_options), 1u) !=
            MPARSER_API_STATUS_OK ||
        options.struct_size != sizeof(options) ||
        summary.struct_size != sizeof(summary) ||
        load_options.struct_size != sizeof(load_options) ||
        mparser_module_compile_utf8(
            source, sizeof(source) - 1,
            "abi_1_1_snapshot.m",
            strlen("abi_1_1_snapshot.m"),
            &module) != MPARSER_API_STATUS_OK ||
        !module || !mparser_module_is_valid(module) ||
        mparser_value_create_scalar(
            40.0, MPARSER_NUMERIC_DOUBLE, &argument) !=
            MPARSER_API_STATUS_OK) {
        goto cleanup;
    }

    arguments[0] = argument;
    options.entry_name = "entry";
    options.entry_name_size = strlen("entry");
    options.arguments = arguments;
    options.argument_count = 1;
    options.requested_output_count = 1;
    options.has_requested_output_count = 1;
    if (mparser_module_execute(module, &options, &result) !=
            MPARSER_API_STATUS_OK ||
        !read_scalar(result, 42.0) ||
        mparser_result_execution_summary(result, &summary) !=
            MPARSER_API_STATUS_OK ||
        summary.abi_version != 1u) {
        goto cleanup;
    }

    puts("c api 1.1 snapshot compatibility = 42,abi-1.1");
    succeeded = 1;

cleanup:
    mparser_result_release(result);
    mparser_value_release(argument);
    mparser_module_release(module);
    return succeeded ? 0 : 1;
}
