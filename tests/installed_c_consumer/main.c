#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

static const char k_source[] =
    "function [sum_value, difference] = pair(left, right)\n"
    "sum_value = left + right;\n"
    "difference = left - right;\n"
    "end\n";

static const char k_host_source[] =
    "formatted = sprintf(\"value=%d\", 42);\n"
    "disp(formatted)\n"
    "written = fprintf(\"pi=%.1f\\n\", 3.14);\n"
    "40 + 2\n"
    "41 + 2;\n";

typedef struct output_capture {
    size_t count;
    int valid;
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

static mparser_output_disposition capture_output(
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
    if (capture == NULL) {
        return MPARSER_OUTPUT_REJECT;
    }

    capture->valid = capture->valid &&
                     sequence == capture->count &&
                     view_ends_with(source, "installed_host.m") &&
                     source_begin.line > 0 && source_end.line > 0;
    if (capture->count == 0) {
        capture->valid = capture->valid &&
                         kind == MPARSER_OUTPUT_DISPLAY &&
                         bytes_equal(text, text_size, "value=42\n\n");
    } else if (capture->count == 1) {
        capture->valid = capture->valid &&
                         kind == MPARSER_OUTPUT_STANDARD &&
                         bytes_equal(text, text_size, "pi=3.1\n");
    } else {
        capture->valid = 0;
    }
    ++capture->count;
    return MPARSER_OUTPUT_ACCEPT;
}

static mparser_api_status create_double_scalar(
    double value, mparser_value** output) {
    const size_t dimensions[2] = {1, 1};
    const mparser_numeric_buffer buffer = {
        MPARSER_NUMERIC_DOUBLE, 0, &value, NULL, 1};
    return mparser_value_create_numeric(
        dimensions, 2, &buffer, output);
}

static int value_is_scalar(
    const mparser_value* value, double expected) {
    mparser_numeric_buffer buffer = {0};
    double difference = 0.0;
    if (mparser_value_get_numeric_buffer(value, &buffer) !=
            MPARSER_API_STATUS_OK ||
        buffer.numeric_class != MPARSER_NUMERIC_DOUBLE ||
        buffer.is_complex != 0 ||
        buffer.element_count != 1) {
        return 0;
    }

    difference =
        ((const double*)buffer.real_data)[0] - expected;
    if (difference < 0.0) {
        difference = -difference;
    }
    return difference < 1e-9;
}

static int read_scalar(const mparser_result* result,
                       size_t index,
                       double expected) {
    mparser_value* output = NULL;
    int valid = 0;
    if (mparser_result_output(result, index, &output) !=
        MPARSER_API_STATUS_OK) {
        return 0;
    }
    valid = value_is_scalar(output, expected);
    mparser_value_release(output);
    return valid;
}

static int run_host_contract(void) {
    mparser_source_load_options load_options;
    mparser_invocation_options options;
    mparser_module* module = NULL;
    mparser_result* result = NULL;
    mparser_value* value = NULL;
    output_capture capture = {0, 1};
    int succeeded = 0;

    if (mparser_source_load_options_init(&load_options) !=
            MPARSER_API_STATUS_OK ||
        mparser_module_compile_utf8_with_options(
            k_host_source, strlen(k_host_source),
            "installed_host.m", strlen("installed_host.m"),
            &load_options, &module) != MPARSER_API_STATUS_OK ||
        module == NULL || !mparser_module_is_valid(module) ||
        mparser_module_source_count(module) != 1 ||
        mparser_module_source_kind(module, 0) != MPARSER_SOURCE_SCRIPT ||
        !mparser_module_source_has_top_level_statements(module, 0) ||
        mparser_module_source_is_pure_function_file(module, 0) ||
        !view_ends_with(
            mparser_module_source_name(module, 0),
            "installed_host.m") ||
        mparser_invocation_options_init(&options) !=
            MPARSER_API_STATUS_OK) {
        goto cleanup;
    }

    options.output_sink = capture_output;
    options.output_user_data = &capture;
    if (mparser_module_execute(module, &options, &result) !=
            MPARSER_API_STATUS_OK ||
        result == NULL || !mparser_result_succeeded(result) ||
        !capture.valid || capture.count != 2 ||
        mparser_result_output_event_count(result) != 2 ||
        mparser_result_output_event_kind(result, 0) !=
            MPARSER_OUTPUT_DISPLAY ||
        mparser_result_output_event_sequence(result, 0) != 0 ||
        !bytes_equal(
            mparser_result_output_event_text(result, 0).data,
            mparser_result_output_event_text(result, 0).size,
            "value=42\n\n") ||
        mparser_result_output_event_kind(result, 1) !=
            MPARSER_OUTPUT_STANDARD ||
        mparser_result_output_event_sequence(result, 1) != 1 ||
        !bytes_equal(
            mparser_result_output_event_text(result, 1).data,
            mparser_result_output_event_text(result, 1).size,
            "pi=3.1\n") ||
        mparser_result_top_level_expression_count(result) != 2) {
        goto cleanup;
    }

    if (mparser_result_top_level_expression_value(
            result, 0, &value) != MPARSER_API_STATUS_OK ||
        !value_is_scalar(value, 42.0) ||
        mparser_result_top_level_expression_output_suppressed(
            result, 0) ||
        mparser_result_top_level_expression_sequence(result, 0) != 2) {
        goto cleanup;
    }
    mparser_value_release(value);
    value = NULL;
    if (mparser_result_top_level_expression_value(
            result, 1, &value) != MPARSER_API_STATUS_OK ||
        !value_is_scalar(value, 43.0) ||
        !mparser_result_top_level_expression_output_suppressed(
            result, 1) ||
        mparser_result_top_level_expression_sequence(result, 1) != 3 ||
        !view_ends_with(
            mparser_result_top_level_expression_source_name(
                result, 1),
            "installed_host.m")) {
        goto cleanup;
    }

    succeeded = 1;

cleanup:
    mparser_value_release(value);
    mparser_result_release(result);
    mparser_module_release(module);
    return succeeded;
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
        mparser_c_abi_generation() != MPARSER_C_ABI_GENERATION ||
        mparser_c_abi_revision() != MPARSER_C_ABI_REVISION ||
        mparser_module_compile_utf8(
            k_source, strlen(k_source),
            "installed_consumer.m",
            strlen("installed_consumer.m"), &module) !=
            MPARSER_API_STATUS_OK ||
        create_double_scalar(39.0, &left) !=
            MPARSER_API_STATUS_OK ||
        create_double_scalar(3.0, &right) !=
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
        !read_scalar(result, 1, 36.0) ||
        !run_host_contract()) {
        goto cleanup;
    }

    printf("installed-consumer = %u.%u.%u,42,36,"
           "abi-generation-%u-revision-%u,host-output-2-2\n",
           mparser_version_major(), mparser_version_minor(),
           mparser_version_patch(), mparser_c_abi_generation(),
           mparser_c_abi_revision());
    succeeded = 1;

cleanup:
    mparser_result_release(result);
    mparser_value_release(right);
    mparser_value_release(left);
    mparser_module_release(module);
    return succeeded ? 0 : 1;
}
