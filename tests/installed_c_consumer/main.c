#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

static const char k_source[] =
    "function [sum_value, difference] = pair(left, right)\n"
    "sum_value = left + right;\n"
    "difference = left - right;\n"
    "end\n";

static int read_scalar(const mparser_result* result,
                       size_t index,
                       double expected) {
    mparser_value* output = NULL;
    const double* data = NULL;
    size_t count = 0;
    double difference = 0.0;
    int valid = 0;
    if (mparser_result_output(result, index, &output) !=
            MPARSER_API_STATUS_OK ||
        mparser_value_numeric_data(output, &data, &count) !=
            MPARSER_API_STATUS_OK ||
        count != 1) {
        mparser_value_release(output);
        return 0;
    }

    difference = data[0] - expected;
    if (difference < 0.0) {
        difference = -difference;
    }
    valid = difference < 1e-9;
    mparser_value_release(output);
    return valid;
}

int main(void) {
    mparser_module* module = NULL;
    mparser_value* left = NULL;
    mparser_value* right = NULL;
    const mparser_value* arguments[2];
    mparser_invocation_options options;
    mparser_result* result = NULL;
    int succeeded = 0;

    if (mparser_version_major() != MPARSER_EXPECTED_VERSION_MAJOR ||
        mparser_version_minor() != MPARSER_EXPECTED_VERSION_MINOR ||
        mparser_version_patch() != MPARSER_EXPECTED_VERSION_PATCH ||
        mparser_c_abi_version() != MPARSER_C_ABI_VERSION ||
        mparser_c_abi_revision() != MPARSER_C_ABI_REVISION ||
        mparser_module_compile_utf8(
            k_source, strlen(k_source),
            "installed_consumer.m",
            strlen("installed_consumer.m"), &module) !=
            MPARSER_API_STATUS_OK ||
        mparser_value_create_scalar(
            39.0, MPARSER_NUMERIC_DOUBLE, &left) !=
            MPARSER_API_STATUS_OK ||
        mparser_value_create_scalar(
            3.0, MPARSER_NUMERIC_DOUBLE, &right) !=
            MPARSER_API_STATUS_OK ||
        MPARSER_INVOCATION_OPTIONS_INIT(&options) !=
            MPARSER_API_STATUS_OK) {
        goto cleanup;
    }

    arguments[0] = left;
    arguments[1] = right;
    options.entry_name = "pair";
    options.entry_name_size = strlen("pair");
    options.arguments = arguments;
    options.argument_count = 2;
    options.has_requested_output_count = 1;
    options.requested_output_count = 2;
    if (mparser_module_execute(module, &options, &result) !=
            MPARSER_API_STATUS_OK ||
        !mparser_result_succeeded(result) ||
        mparser_result_output_count(result) != 2 ||
        !read_scalar(result, 0, 42.0) ||
        !read_scalar(result, 1, 36.0)) {
        goto cleanup;
    }

    printf("installed-consumer = %u.%u.%u,42,36,abi-%u.%u\n",
           mparser_version_major(), mparser_version_minor(),
           mparser_version_patch(), mparser_c_abi_version(),
           mparser_c_abi_revision());
    succeeded = 1;

cleanup:
    mparser_result_release(result);
    mparser_value_release(right);
    mparser_value_release(left);
    mparser_module_release(module);
    return succeeded ? 0 : 1;
}
