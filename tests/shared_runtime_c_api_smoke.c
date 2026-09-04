#include "mparser/c_api.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static const char k_producer_source[] =
    "function out = makeAffine(factor)\n"
    "offset = 2;\n"
    "out = @(x) x * factor + offset;\n"
    "end\n"
    "function out = writeGlobal(value)\n"
    "global shared_value\n"
    "shared_value = value;\n"
    "out = shared_value;\n"
    "end\n"
    "function out = nextPersistent(step)\n"
    "persistent count\n"
    "if isempty(count)\n"
    "count = 0;\n"
    "end\n"
    "count = count + step;\n"
    "out = count;\n"
    "end\n";

static const char k_consumer_source[] =
    "function out = applyHandle(fn, value)\n"
    "out = fn(value);\n"
    "end\n"
    "function out = applyCell(box, value)\n"
    "fn = box{1};\n"
    "out = fn(value);\n"
    "end\n"
    "function out = readGlobal()\n"
    "global shared_value\n"
    "out = shared_value;\n"
    "end\n";

static int fail(const char* expression, int line) {
    fprintf(stderr, "shared runtime C API check failed at line %d: %s\n",
            line, expression);
    return 1;
}

#define CHECK(expression)                                                   \
    do {                                                                    \
        if (!(expression)) {                                                \
            return fail(#expression, __LINE__);                            \
        }                                                                   \
    } while (0)

static mparser_api_status create_scalar(double number,
                                        mparser_value** out_value) {
    const size_t dimensions[2] = {1, 1};
    const mparser_numeric_buffer buffer = {
        MPARSER_NUMERIC_DOUBLE, 0, &number, NULL, 1};
    return mparser_value_create_numeric(
        dimensions, 2, &buffer, out_value);
}

static int scalar_equals(const mparser_value* value, double expected) {
    mparser_numeric_buffer buffer;
    memset(&buffer, 0, sizeof(buffer));
    return value != NULL &&
           mparser_value_get_kind(value) == MPARSER_VALUE_NUMERIC &&
           mparser_value_get_numeric_buffer(value, &buffer) ==
               MPARSER_API_STATUS_OK &&
           buffer.numeric_class == MPARSER_NUMERIC_DOUBLE &&
           buffer.is_complex == 0 && buffer.real_data != NULL &&
           buffer.element_count == 1 &&
           fabs(((const double*)buffer.real_data)[0] - expected) < 1e-9;
}

static mparser_api_status runtime_execute(
    mparser_runtime* runtime, const mparser_module* module,
    const char* entry, const mparser_value* const* arguments,
    size_t argument_count, mparser_result** out_result) {
    mparser_invocation_options options;
    mparser_api_status status =
        MPARSER_INVOCATION_OPTIONS_INIT(&options);
    if (status != MPARSER_API_STATUS_OK) {
        return status;
    }
    options.entry_name = entry;
    options.entry_name_size = strlen(entry);
    options.arguments = arguments;
    options.argument_count = argument_count;
    options.requested_output_count = 1;
    options.has_requested_output_count = 1;
    options.backend = MPARSER_BACKEND_BYTECODE;
    return mparser_runtime_execute(
        runtime, module, &options, out_result);
}

static int result_scalar_equals(const mparser_result* result,
                                double expected) {
    mparser_value* output = NULL;
    int matches = 0;
    if (result != NULL && mparser_result_succeeded(result) != 0 &&
        mparser_result_output_count(result) == 1 &&
        mparser_result_output(result, 0, &output) ==
            MPARSER_API_STATUS_OK) {
        matches = scalar_equals(output, expected);
    }
    mparser_value_release(output);
    return matches;
}

int main(void) {
    mparser_module* producer = NULL;
    mparser_module* consumer = NULL;
    mparser_runtime* runtime = NULL;
    mparser_runtime* other_runtime = NULL;
    mparser_result* result = NULL;
    mparser_value* factor = NULL;
    mparser_value* input = NULL;
    mparser_value* closure = NULL;
    mparser_value* cell = NULL;
    mparser_value* other_closure = NULL;

    CHECK(mparser_runtime_create(NULL, NULL) ==
          MPARSER_API_STATUS_INVALID_ARGUMENT);
    CHECK(mparser_runtime_create(NULL, &runtime) ==
          MPARSER_API_STATUS_OK);
    CHECK(runtime != NULL);
    mparser_runtime_retain(runtime);
    mparser_runtime_release(runtime);

    CHECK(mparser_module_compile_utf8(
              k_producer_source, sizeof(k_producer_source) - 1,
              "shared_runtime_c_producer.m",
              sizeof("shared_runtime_c_producer.m") - 1,
              &producer) == MPARSER_API_STATUS_OK);
    CHECK(mparser_module_compile_utf8(
              k_consumer_source, sizeof(k_consumer_source) - 1,
              "shared_runtime_c_consumer.m",
              sizeof("shared_runtime_c_consumer.m") - 1,
              &consumer) == MPARSER_API_STATUS_OK);
    CHECK(producer != NULL && consumer != NULL);

    CHECK(create_scalar(3.0, &factor) == MPARSER_API_STATUS_OK);
    {
        const mparser_value* arguments[1] = {factor};
        CHECK(runtime_execute(runtime, producer, "makeAffine",
                              arguments, 1, &result) ==
              MPARSER_API_STATUS_OK);
    }
    CHECK(result != NULL && mparser_result_succeeded(result) != 0);
    CHECK(mparser_result_output(result, 0, &closure) ==
          MPARSER_API_STATUS_OK);
    CHECK(closure != NULL && mparser_value_is_module_bound(closure) != 0);
    mparser_result_release(result);
    result = NULL;
    mparser_module_release(producer);
    producer = NULL;

    CHECK(create_scalar(4.0, &input) == MPARSER_API_STATUS_OK);
    {
        const mparser_value* arguments[2] = {closure, input};
        CHECK(runtime_execute(runtime, consumer, "applyHandle",
                              arguments, 2, &result) ==
              MPARSER_API_STATUS_OK);
    }
    CHECK(result_scalar_equals(result, 14.0));
    mparser_result_release(result);
    result = NULL;

    {
        mparser_invocation_options options;
        const mparser_value* arguments[2] = {closure, input};
        CHECK(MPARSER_INVOCATION_OPTIONS_INIT(&options) ==
              MPARSER_API_STATUS_OK);
        options.entry_name = "applyHandle";
        options.entry_name_size = sizeof("applyHandle") - 1;
        options.arguments = arguments;
        options.argument_count = 2;
        options.requested_output_count = 1;
        options.has_requested_output_count = 1;
        CHECK(mparser_module_execute(consumer, &options, &result) ==
              MPARSER_API_STATUS_OWNER_MISMATCH);
        CHECK(result == NULL);
    }

    CHECK(mparser_runtime_create(NULL, &other_runtime) ==
          MPARSER_API_STATUS_OK);
    {
        const mparser_value* arguments[2] = {closure, input};
        CHECK(runtime_execute(other_runtime, consumer, "applyHandle",
                              arguments, 2, &result) ==
              MPARSER_API_STATUS_OWNER_MISMATCH);
        CHECK(result == NULL);
    }

    {
        const size_t dimensions[2] = {1, 1};
        const mparser_value* elements[1] = {closure};
        CHECK(mparser_value_create_cell(
                  dimensions, 2, elements, 1, &cell) ==
              MPARSER_API_STATUS_OK);
    }
    CHECK(cell != NULL && mparser_value_is_module_bound(cell) != 0);
    {
        const mparser_value* arguments[2] = {cell, input};
        CHECK(runtime_execute(runtime, consumer, "applyCell",
                              arguments, 2, &result) ==
              MPARSER_API_STATUS_OK);
    }
    CHECK(result_scalar_equals(result, 14.0));
    mparser_result_release(result);
    result = NULL;

    {
        mparser_value* other_factor = NULL;
        mparser_module* other_producer = NULL;
        const mparser_value* arguments[1];
        CHECK(mparser_module_compile_utf8(
                  k_producer_source, sizeof(k_producer_source) - 1,
                  "shared_runtime_c_other_producer.m",
                  sizeof("shared_runtime_c_other_producer.m") - 1,
                  &other_producer) == MPARSER_API_STATUS_OK);
        CHECK(create_scalar(2.0, &other_factor) ==
              MPARSER_API_STATUS_OK);
        arguments[0] = other_factor;
        CHECK(runtime_execute(other_runtime, other_producer,
                              "makeAffine", arguments, 1, &result) ==
              MPARSER_API_STATUS_OK);
        CHECK(mparser_result_output(result, 0, &other_closure) ==
              MPARSER_API_STATUS_OK);
        mparser_result_release(result);
        result = NULL;
        mparser_value_release(other_factor);
        mparser_module_release(other_producer);
    }
    {
        const size_t dimensions[2] = {1, 2};
        const mparser_value* elements[2] = {closure, other_closure};
        mparser_value* rejected = NULL;
        CHECK(mparser_value_create_cell(
                  dimensions, 2, elements, 2, &rejected) ==
              MPARSER_API_STATUS_OWNER_MISMATCH);
        CHECK(rejected == NULL);
    }

    {
        mparser_module* state_producer = NULL;
        mparser_value* value = NULL;
        const mparser_value* arguments[1];
        CHECK(mparser_module_compile_utf8(
                  k_producer_source, sizeof(k_producer_source) - 1,
                  "shared_runtime_c_state_producer.m",
                  sizeof("shared_runtime_c_state_producer.m") - 1,
                  &state_producer) == MPARSER_API_STATUS_OK);
        CHECK(create_scalar(9.0, &value) == MPARSER_API_STATUS_OK);
        arguments[0] = value;
        CHECK(runtime_execute(runtime, state_producer, "writeGlobal",
                              arguments, 1, &result) ==
              MPARSER_API_STATUS_OK);
        CHECK(result_scalar_equals(result, 9.0));
        mparser_result_release(result);
        result = NULL;
        CHECK(runtime_execute(runtime, consumer, "readGlobal",
                              NULL, 0, &result) ==
              MPARSER_API_STATUS_OK);
        CHECK(result_scalar_equals(result, 9.0));
        mparser_result_release(result);
        result = NULL;
        CHECK(mparser_runtime_clear_global(
                  runtime, "shared_value",
                  sizeof("shared_value") - 1) ==
              MPARSER_API_STATUS_OK);
        CHECK(mparser_runtime_clear_globals(runtime) ==
              MPARSER_API_STATUS_OK);
        CHECK(mparser_runtime_reset(runtime) ==
              MPARSER_API_STATUS_OK);
        mparser_value_release(value);
        mparser_module_release(state_producer);
    }

    mparser_value_release(other_closure);
    mparser_value_release(cell);
    mparser_value_release(closure);
    mparser_value_release(input);
    mparser_value_release(factor);
    mparser_module_release(consumer);
    mparser_runtime_release(other_runtime);
    mparser_runtime_release(runtime);

    puts("shared runtime c api = retained,ownership,composition,state");
    return 0;
}
