#include "mparser/c_api.h"

#include <stdio.h>
#include <string.h>

static void print_module_diagnostics(const mparser_module* module) {
    size_t index;
    for (index = 0;
         index < mparser_module_diagnostic_count(module);
         ++index) {
        const mparser_diagnostic* diagnostic =
            mparser_module_diagnostic(module, index);
        const mparser_utf8_view source =
            mparser_diagnostic_source_name(diagnostic);
        const mparser_utf8_view message =
            mparser_diagnostic_message(diagnostic);
        fprintf(stderr, "%.*s: %.*s\n",
                (int)source.size,
                source.data != NULL ? source.data : "",
                (int)message.size,
                message.data != NULL ? message.data : "");
    }
}

static int read_scalar_variable(const mparser_result* result,
                                const char* expected_name,
                                double* output) {
    size_t index;
    for (index = 0;
         index < mparser_result_variable_count(result);
         ++index) {
        mparser_utf8_view name;
        mparser_value* value = NULL;
        const double* data = NULL;
        size_t count = 0;
        if (mparser_result_variable(
                result, index, &name, &value) !=
            MPARSER_API_STATUS_OK) {
            return 0;
        }
        if (name.size == strlen(expected_name) &&
            memcmp(name.data, expected_name, name.size) == 0) {
            const int valid =
                mparser_value_numeric_data(
                    value, &data, &count) ==
                    MPARSER_API_STATUS_OK &&
                count == 1;
            if (valid) {
                *output = data[0];
            }
            mparser_value_release(value);
            return valid;
        }
        mparser_value_release(value);
    }
    return 0;
}

int main(int argc, char** argv) {
    mparser_source_load_options load_options;
    mparser_utf8_view search_path;
    mparser_module* module = NULL;
    mparser_invocation_options invocation;
    mparser_result* result = NULL;
    mparser_api_status status;
    double scaled = 0;
    double revealed = 0;
    double offset = 0;
    double twice = 0;

    if (argc != 3) {
        fprintf(stderr,
                "usage: mparser_c_source_graph_demo "
                "<entry.m> <search-path>\n");
        return 2;
    }

    MPARSER_SOURCE_LOAD_OPTIONS_INIT(&load_options);
    search_path.data = argv[2];
    search_path.size = strlen(argv[2]);
    load_options.search_paths = &search_path;
    load_options.search_path_count = 1;
    status = mparser_module_load_file_utf8(
        argv[1], strlen(argv[1]), &load_options, &module);
    if (status != MPARSER_API_STATUS_OK) {
        const mparser_utf8_view name =
            mparser_api_status_name(status);
        fprintf(stderr, "load failed: %.*s\n",
                (int)name.size, name.data);
        print_module_diagnostics(module);
        mparser_module_release(module);
        return 1;
    }

    MPARSER_INVOCATION_OPTIONS_INIT(&invocation);
    status = mparser_module_execute(
        module, &invocation, &result);
    if (status != MPARSER_API_STATUS_OK ||
        !mparser_result_succeeded(result) ||
        !read_scalar_variable(result, "scaled", &scaled) ||
        !read_scalar_variable(result, "revealed", &revealed) ||
        !read_scalar_variable(result, "offset_value", &offset) ||
        !read_scalar_variable(result, "static_value", &twice)) {
        fprintf(stderr, "source graph execution failed\n");
        mparser_result_release(result);
        mparser_module_release(module);
        return 1;
    }

    printf("source-count = %zu\n",
           mparser_module_source_count(module));
    printf("summary = %.0f,%.0f,%.0f,%.0f\n",
           scaled, revealed, offset, twice);

    mparser_result_release(result);
    mparser_module_release(module);
    return 0;
}
