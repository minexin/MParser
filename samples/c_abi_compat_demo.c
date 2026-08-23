#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

typedef struct future_invocation_options {
    mparser_invocation_options value;
    unsigned char future_tail[16];
} future_invocation_options;

typedef struct future_execution_summary {
    mparser_execution_summary value;
    unsigned char future_tail[16];
} future_execution_summary;

static const char k_source[] =
    "function out = addTwo(value)\n"
    "out = value + 2;\n"
    "end\n";

static int tail_is_zero(const unsigned char* tail, size_t size) {
    size_t index;
    for (index = 0; index < size; ++index) {
        if (tail[index] != 0) {
            return 0;
        }
    }
    return 1;
}

static mparser_api_status create_double_scalar(
    double value, mparser_value** output) {
    const size_t dimensions[2] = {1, 1};
    const mparser_numeric_buffer buffer = {
        MPARSER_NUMERIC_DOUBLE, 0, &value, NULL, 1};
    return mparser_value_create_numeric(
        dimensions, 2, &buffer, output);
}

int main(void) {
    mparser_module* module = NULL;
    mparser_value* argument = NULL;
    const mparser_value* arguments[1];
    future_invocation_options invocation;
    future_execution_summary summary;
    mparser_result* result = NULL;
    mparser_value* output = NULL;
    mparser_numeric_buffer numeric_buffer = {0};
    int succeeded = 0;

    if (mparser_module_compile_utf8(
            k_source, strlen(k_source), "c_abi_compat_demo.m",
            strlen("c_abi_compat_demo.m"), &module) !=
            MPARSER_API_STATUS_OK ||
        create_double_scalar(40.0, &argument) !=
            MPARSER_API_STATUS_OK ||
        mparser_invocation_options_init_sized(
            &invocation, sizeof(invocation),
            MPARSER_C_ABI_GENERATION) != MPARSER_API_STATUS_OK ||
        !tail_is_zero(
            invocation.future_tail,
            sizeof(invocation.future_tail))) {
        goto cleanup;
    }

    arguments[0] = argument;
    invocation.value.entry_name = "addTwo";
    invocation.value.entry_name_size = strlen("addTwo");
    invocation.value.arguments = arguments;
    invocation.value.argument_count = 1;
    invocation.value.requested_output_count = 1;
    invocation.value.has_requested_output_count = 1;
    if (mparser_module_execute(
            module, &invocation.value, &result) !=
            MPARSER_API_STATUS_OK ||
        !mparser_result_succeeded(result) ||
        mparser_result_output(result, 0, &output) !=
            MPARSER_API_STATUS_OK ||
        mparser_value_get_numeric_buffer(
            output, &numeric_buffer) !=
            MPARSER_API_STATUS_OK ||
        numeric_buffer.numeric_class != MPARSER_NUMERIC_DOUBLE ||
        numeric_buffer.is_complex != 0 ||
        numeric_buffer.element_count != 1 ||
        ((const double*)numeric_buffer.real_data)[0] != 42.0 ||
        mparser_execution_summary_init_sized(
            &summary, sizeof(summary),
            MPARSER_C_ABI_GENERATION) != MPARSER_API_STATUS_OK ||
        mparser_result_execution_summary(
            result, &summary.value) != MPARSER_API_STATUS_OK ||
        !tail_is_zero(
            summary.future_tail, sizeof(summary.future_tail))) {
        goto cleanup;
    }

    printf("api = 1.3, abi-generation = %u, revision = %u, "
           "result = %.0f, request-bytes = %u\n",
           mparser_c_abi_generation(), mparser_c_abi_revision(),
           ((const double*)numeric_buffer.real_data)[0],
           invocation.value.struct_size);
    succeeded = 1;

cleanup:
    mparser_value_release(output);
    mparser_result_release(result);
    mparser_value_release(argument);
    mparser_module_release(module);
    return succeeded ? 0 : 1;
}
