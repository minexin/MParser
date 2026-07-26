#include "c_api_v1_snapshot.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static const char k_entry_source[] =
    "function out = entry(value)\n"
    "out = helper(value);\n"
    "end\n";

static const char k_helper_source[] =
    "function out = helper(value)\n"
    "out = value + 2;\n"
    "end\n";

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
    mparser_source_unit sources[2];
    mparser_module* module = NULL;
    mparser_value* argument = NULL;
    const mparser_value* arguments[1];
    mparser_invocation_options options;
    mparser_execution_summary summary;
    mparser_result* result = NULL;
    int succeeded = 0;

    if (mparser_c_abi_version() != MPARSER_C_ABI_VERSION ||
        mparser_source_unit_init(&sources[0]) !=
            MPARSER_API_STATUS_OK ||
        mparser_source_unit_init(&sources[1]) !=
            MPARSER_API_STATUS_OK) {
        goto cleanup;
    }
    sources[0].source_name = "entry.m";
    sources[0].source_name_size = strlen("entry.m");
    sources[0].source = k_entry_source;
    sources[0].source_size = strlen(k_entry_source);
    sources[1].source_name = "helper.m";
    sources[1].source_name_size = strlen("helper.m");
    sources[1].source = k_helper_source;
    sources[1].source_size = strlen(k_helper_source);

    if (mparser_module_compile_sources(sources, 2, &module) !=
            MPARSER_API_STATUS_OK ||
        !module || !mparser_module_is_valid(module) ||
        mparser_value_create_scalar(
            40.0, MPARSER_NUMERIC_DOUBLE, &argument) !=
            MPARSER_API_STATUS_OK ||
        mparser_invocation_options_init(&options) !=
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
        mparser_execution_summary_init(&summary) !=
            MPARSER_API_STATUS_OK ||
        mparser_result_execution_summary(result, &summary) !=
            MPARSER_API_STATUS_OK ||
        summary.struct_size != sizeof(summary) ||
        summary.abi_version != MPARSER_C_ABI_VERSION) {
        goto cleanup;
    }

    puts("c api v1 compatibility = 42,abi-1");
    succeeded = 1;

cleanup:
    mparser_result_release(result);
    mparser_value_release(argument);
    mparser_module_release(module);
    return succeeded ? 0 : 1;
}
